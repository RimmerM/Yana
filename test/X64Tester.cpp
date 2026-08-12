// Standalone x64-codegen test driver.
//
// Parses a `.lower` file (the same self-contained lower-IR text format used by LowerTester.cpp -
// see compiler/lower/lower_parser.h), then for every function runs the x64 backend pipeline
// (transformFunction -> allocateRegisters -> genFunction) and compares an annotated disassembly
// listing against a golden `<name>.lower.expect` file, with the same `generate` mode as
// LowerTester.cpp to (re)write goldens.
//
// The golden format is line-per-instruction: the lower-IR text for the instruction (via the
// existing nameForInst()), the resolved registers for its operands, and the hex bytes it was
// encoded to - chosen over a raw hex dump of the whole function so a single changed encoding
// shows up as one changed line in review, not a file-wide byte shift.
#include <Core.h>
#include <File.h>
#include <cstdio>
#include <cstring>
#include "../compiler/lower/lower_parser.h"
#include "../compiler/lower/lower_print.h"
#include "../compiler/codegen/x64/gen.h"
#include "Net/Stream.h"
#include "Net/File.h"
#include "directives.h"

using namespace Tritium;

// Codegen settings a test file selects for itself, written as a comment on any line:
//
//     # frame-pointer: all
//     # extensions: avx
//
// Two test files with the same functions and different directives is how the settings are compared,
// which keeps every golden an ordinary one.
//
// `extensions` is what makes the VEX and EVEX encodings testable: a fixture that names one gets the
// forms that need it, and one that names none gets exactly the bytes it got before those forms
// existed. It sets `explicitExtensions` for the same reason the command line does - the local
// backend takes an unnamed extension as absent, so a golden cannot change with the machine that ran
// it (see CompileSettings::explicitExtensions).
static void applyDirectives(CompileSettings& settings, StringView content) {
    auto find = [&](StringView directive, auto&& match) {
        for(Size i = 0; i + directive.length <= content.length; i++) {
            if(compareMem(content.ptr + i, directive.ptr, directive.length) != 0) continue;
            match(content.ptr + i + directive.length, content.length - i - directive.length);
        }
    };

    static const struct { StringView name; FramePointerMode mode; } modes[] = {
        { "all"_v, FramePointerMode::All },
        { "non-leaf"_v, FramePointerMode::NonLeaf },
        { "needed"_v, FramePointerMode::Needed },
    };

    find("# frame-pointer: "_v, [&](const char* rest, Size left) {
        for(auto& m: modes) {
            if(m.name.length <= left && compareMem(rest, m.name.ptr, m.name.length) == 0) {
                settings.framePointer = m.mode;
            }
        }
    });

    // The extension level itself is `directives.h`', shared with the two drivers that compile the
    // `resolve/` corpus. Shared rather than repeated because those two and this one have to agree:
    // the level decides what `Vec(Float)` *is*, so a table that drifted would not be a difference of
    // encoding but a difference of program.
    applyExtensionDirective(settings, content);
}

struct TestProvider: SourceProvider {
    StringView source;

    StringView getSource(StringId module) override {
        return source;
    }

    const Location* getNode(LocationId id) override {
        return nullptr;
    }
};

static void writeHex(Net::Writer& w, Byte b) {
    static const char digits[] = "0123456789abcdef";
    char out[2] = { digits[b >> 4], digits[b & 0xf] };
    w.writeBytes((const Byte*)out, 2);
}

static void writeInt(Net::Writer& w, Size v) {
    char buf[24];
    auto len = snprintf(buf, sizeof(buf), "%zu", v);
    w.writeBytes((const Byte*)buf, len);
}

static void writeSigned(Net::Writer& w, I64 v) {
    char buf[24];
    auto len = snprintf(buf, sizeof(buf), "%lld", (long long)v);
    w.writeBytes((const Byte*)buf, len);
}

// The parts of an address that live in the instruction rather than in a register. The base and index
// are already visible in the operand list, but the scale and displacement are encoded straight into
// the ModRM/SIB bytes of whatever folds this address in, and would otherwise only be readable as hex.
static void writeAddressDetail(Net::Writer& w, Context& context, LowerBase base, LowerInst& inst) {
    if(inst.kind != LowerInst::X86Address && inst.kind != LowerInst::X86Lea) return;

    auto& address = (LowerInstX86Address&)inst;

    // A rip-relative address is the symbol and nothing else - no base, no index, no displacement to
    // print - so it is named rather than described. Without this a pooled constant folded into an
    // arithmetic operand reads as `scale=1 disp=0`, which says nothing about which constant it is.
    if(address.symbol) {
        w.writeString(" [rip + "_v);
        w.writeString(context.findName(base[address.symbol]->name));
        w.writeString("]"_v);
        return;
    }

    w.writeString(" scale="_v);
    writeInt(w, address.scale);
    w.writeString(" disp="_v);
    writeSigned(w, I64(I32(address.displacement)));
}

static void writeRegName(Net::Writer& w, MachineLocation at) {
    static const char* intNames[16] = {
        "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
        "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
    };

    auto idx = at.index;

    switch(at.kind) {
        case LocationKind::Invalid:
            w.writeString("-"_v);
            break;
        case LocationKind::Physical:
            if(at.bank == BankGpr && idx < 16) {
                w.writeString(StringView { intNames[idx], strlen(intNames[idx]) });
            } else if(at.bank == BankVector) {
                w.writeString("xmm"_v);
                writeInt(w, idx);
            } else if(at.bank == BankMask) {
                w.writeString("k"_v);
                writeInt(w, idx);
            } else {
                w.writeString("?"_v);
            }
            break;
        case LocationKind::StackSlot:
            w.writeString("stack:"_v);
            writeInt(w, idx);
            break;
        case LocationKind::Rematerializable:
            // Not a place at all: the value is recreated wherever it is read, and the index names
            // the recipe in FunctionRegs::remats that does it.
            w.writeString("remat:"_v);
            writeInt(w, idx);
            break;
    }
}

// Where each operand ended up. An operand that occupies no location at all - an immediate the
// encoding carries, an address folded into a ModRM byte - prints as "-", which is what its invalid
// location says.
static void writeRegList(Net::Writer& w, SmallBuffer<ResolvedOperand> regs) {
    w.writeByte('[');
    for(Size i = 0; i < regs.size(); i++) {
        if(i > 0) w.writeString(", "_v);
        writeRegName(w, regs[i].at);
    }
    w.writeByte(']');
}

struct TraceEntry {
    LowerInst* inst;
    InstRegs regs;
    U32 start;
    U32 end;
};

struct TraceContext {
    Array<TraceEntry> entries;
};

static void onEmitInst(void* ctx, LowerInst* inst, const InstRegs& regs, U32 start, U32 end) {
    auto trace = (TraceContext*)ctx;
    trace->entries.push(TraceEntry {
        .inst = inst,
        .regs = InstRegs { regs.uses, regs.creates, regs.address, regs.hasAddress, regs.moves, regs.postMoves },
        .start = start,
        .end = end,
    });
}

static void writeMoveList(Net::Writer& w, StringView label, SmallBuffer<RegMove> moves) {
    Size shown = 0;

    for(auto& m: moves) {
        if(m.from == m.to) continue;

        if(shown == 0) { w.writeByte(' '); w.writeString(label); w.writeByte('['); }
        else w.writeString(", "_v);

        shown++;
        writeRegName(w, m.from);
        w.writeString(m.swap ? "<->"_v : "->"_v);
        writeRegName(w, m.to);
    }

    if(shown > 0) w.writeByte(']');
}

// Prints the annotated codegen trace for a whole module: the lower-IR text (unannotated, exactly
// as LowerTester.cpp's golden files show it) followed by a per-instruction disassembly trace.
static void printTrace(Net::Writer& writer, Context& context, LowerBase base, LowerModule& module) {
    PrintContext print;
    printModule(writer, context, base, module);

    // Every function is emitted into a single AsmModule before anything is printed, so that a
    // direct call's rel32 is actually resolved against the callee's real offset (relocations can
    // only be resolved once every function in the module has been assigned one). Trace entries
    // carry absolute buffer offsets, so they stay valid across the whole shared buffer.
    AsmModule asm_;

    // Sized up front - the number of functions is already known, so pushing never has to grow.
    Array<TraceContext> traces(U32(module.functions.size()));

    // Not reset between functions, unlike the run tester's: a trace entry holds the instruction
    // record it was emitted from, and those live in the scratch until the whole module has been
    // printed - see RegScratch.
    RegScratch scratch;
    FunctionRegs regs;
    MachineFunction machine;

    for(auto fo: module.functionOrder) {
        auto fun = base[fo];
        machine.reset();
        transformFunction(context, base, *fun, machine);
        allocateRegisters(context, base, *fun, machine, scratch, regs);

        traces.push();
        genFunction(context, base, asm_, *fun, machine, regs, &onEmitInst, &traces[traces.size() - 1]);
    }

    // After all code, so the two never interleave - see AsmModule::addGlobal.
    for(auto& g: module.globalOrder) {
        asm_.addGlobal(base, base[g]);
    }

    asm_.resolveRelocations(module.imageAnchor ? base[module.imageAnchor] : nullptr);

    Size funIndex = 0;
    for(auto fo: module.functionOrder) {
        auto fun = base[fo];
        auto& trace = traces[funIndex++];

        writer.writeString("\n--- codegen: "_v);
        writer.writeString(context.findName(fun->name));
        writer.writeString(" ---\n"_v);

        LowerBlock* currentBlock = nullptr;
        bool seenPrologue = false;

        for(auto& e: trace.entries) {
            // The prologue belongs to the function rather than to any block, and is reported with a
            // null instruction (see InstEmitCallback). So is a *shared* epilogue, which is the other
            // sequence that belongs to the whole function - and it is reported after every block
            // where the prologue is reported before any, which is what tells the two apart. A
            // function that duplicates its epilogue has one inside the byte range of each `ret`.
            if(!e.inst) {
                writer.writeString(seenPrologue ? "  epilogue  => "_v : "  prologue  => "_v);
                seenPrologue = true;
                for(auto off = e.start; off < e.end; off++) writeHex(writer, asm_.buffer.buffer[off]);
                writer.writeByte('\n');
                continue;
            }

            auto block = base[e.inst->block];
            if(block != currentBlock) {
                currentBlock = block;
                writer.writeString("block "_v);
                if(block->name) writer.writeString(context.findName(block->name));
                else { writer.writeString("#"_v); writeInt(writer, block->index); }
                writer.writeString(":\n"_v);
            }

            writer.writeString("  "_v);
            writer.writeString(nameForInst(base, *e.inst));
            writeAddressDetail(writer, context, base, *e.inst);
            writer.writeString(" uses="_v);
            writeRegList(writer, e.regs.uses);
            writer.writeString(" creates="_v);
            writeRegList(writer, e.regs.creates);
            writeMoveList(writer, "pre="_v, e.regs.moves);
            writeMoveList(writer, "post="_v, e.regs.postMoves);
            writer.writeString("  => "_v);

            for(auto off = e.start; off < e.end; off++) {
                writeHex(writer, asm_.buffer.buffer[off]);
            }
            writer.writeByte('\n');
        }
    }
}

static bool compareAgainst(Context& context, LowerBase base, LowerModule& module, const String& comparePath) {
    Net::Writer writer(16384);
    printTrace(writer, context, base, module);
    auto string = writer.getBuffered();

    auto result = File::openFile(comparePath, readAccess());
    if(result.isErr()) {
        println("cannot open file %@: error %@", comparePath, (U32)result.unwrapErr());
        return false;
    }

    auto file = result.moveUnwrapOk();
    auto size = file.size();
    Ptr<char, HeapDeleter> buffer { (char*)hAlloc(size) };
    file.read({ (Byte*)buffer.get(), size });

    auto equal = size == string.length && compareMem(buffer.get(), string.ptr, size) == 0;
    if(!equal) {
        println("Fail (%@). Got:", comparePath);
        print(StringView { (char*)string.ptr, string.length });
        println("\n\n\nExpected:");
        print(StringView { buffer.get(), size });
        print("\n\n\n");
    }

    return equal;
}

static void writeExpect(Context& context, LowerBase base, LowerModule& module, const String& path) {
    try {
        Net::FileStream file;
        file.open(path, writeAccess(), File::CreateAlways);

        Net::Writer writer(Net::WriteStream(file), 16384);
        printTrace(writer, context, base, module);
        writer.flush();
    } catch(const Net::Exception& e) {
        logError("Cannot create expect file \"%@\": %@", path, e.description);
    }
}

// One fixture. Answers whether it passed, which is what the driver's exit code is made of - a run
// that reports every failure in its output and then exits 0 is one nothing above it can act on.
bool x64Test(const String& path, StringView content) {
    print("Running test \"%@\"...", path);

    TestProvider provider;
    provider.source = content;

    PrintDiagnostics diagnostics(provider);
    Context context(diagnostics);
    applyDirectives(context.settings, content);

    LowerModule module(1024 * 1024);
    LowerLexer lexer(context, diagnostics, content);
    LowerParser parser(context, module, lexer);

    if(!parser.parseModule()) {
        println("Failed to parse test file.");
        return false;
    }

    auto expectPath = path + String(".expect");
    auto pass = compareAgainst(context, *module.arena, module, expectPath);

    println(pass ? "Pass."_v : "Fail."_v);
    return pass;
}

bool generateX64Test(const String& path, StringView content) {
    logInfo("Generating expect file for test \"%@\"", path);

    TestProvider provider;
    provider.source = content;

    PrintDiagnostics diagnostics(provider);
    Context context(diagnostics);
    applyDirectives(context.settings, content);

    LowerModule module(1024 * 1024);
    LowerLexer lexer(context, diagnostics, content);
    LowerParser parser(context, module, lexer);

    if(!parser.parseModule()) {
        println("Failed to parse test file.");
        return false;
    }

    auto expectPath = path + String(".expect");
    writeExpect(context, *module.arena, module, expectPath);
    println("Created expect file \"%@\".", expectPath);
    return true;
}

bool testX64(bool generate) {
    auto passed = true;
    Array<String> tests;

    listDirectory("x64", [&](const String& name, bool isDirectory) {
        if(!isDirectory && name != ".." && name != ".") {
            if(auto p = findLastChar(stringView(name), '.')) {
                String extension(p + 1, name.text() + name.size() - p - 1);
                if(extension == "lower") {
                    tests.push(String("x64/") + name);
                }
            }
        }
    });

    // A corpus of nothing is the same hole one level down: the driver has verified nothing, and
    // saying so is the only answer that cannot be mistaken for having run. The fixture paths are
    // relative to `test/`, so this is what a run from the wrong directory looks like.
    if(tests.size() == 0) {
        println("no tests found");
        return false;
    }

    for(auto& test: tests) {
        auto result = File::openFile(test, readAccess());
        if(result.isErr()) {
            println("cannot open file %@: error %@", test, (U32)result.unwrapErr());
            passed = false;
            continue;
        }

        auto file = result.moveUnwrapOk();
        auto size = file.size();
        Ptr<char, HeapDeleter> buffer { (char*)hAlloc(size) };
        file.read({ (Byte*)buffer.get(), size });

        if(generate) {
            if(!generateX64Test(test, { buffer.get(), size })) passed = false;
        } else {
            if(!x64Test(test, { buffer.get(), size })) passed = false;
        }
    }

    return passed;
}

int main(int argc, const char** argv) {
    bool generateExpects = false;

    for(int i = 1; i < argc; i++) {
        auto arg = String(argv[i]);
        if(arg == "generate") generateExpects = true;
    }

    return testX64(generateExpects) ? 0 : 1;
}
