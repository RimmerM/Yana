#include <Core.h>
#include <File.h>
#include <cstdlib>
#include <cstring>
#include "../compiler/parse/parser.h"
#include "../compiler/resolve/analyze.h"
#include "../compiler/resolve/lower.h"
#include "../compiler/resolve/print.h"
#include "../compiler/lower/lower_print.h"
#include "../compiler/lower/lower_validate.h"
#include "../compiler/codegen/x64/gen.h"
#include "../compiler/codegen/js/gen.h"
#include "Net/Stream.h"
#include "Net/File.h"

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

using namespace Tritium;

// Supplies both the source text diagnostics quote and the modules an `import` names. An
// imported module comes from `resolve/modules/<Name>.yana`, so a fixture can exercise real
// cross-module name resolution without the driver needing a project file.
struct TestProvider: SourceProvider, ModuleProvider {
    struct Loaded {
        StringId name;
        Ptr<char, HeapDeleter> text;
        Size length;
        ast::Module* ast;
    };

    StringView source;
    Context* context = nullptr;
    Array<Loaded> loaded;

    ~TestProvider() override {
        for(auto& entry: loaded) delete entry.ast;
    }

    StringView getSource(StringId id) override {
        for(auto& entry: loaded) {
            if(entry.name == id) return StringView { entry.text.get(), entry.length };
        }

        return source;
    }

    const Location* getNode(LocationId id) override {
        return context ? context->getLocation(id) : nullptr;
    }

    ast::Module* getModule(StringId name) override {
        for(auto& entry: loaded) {
            if(entry.name == name) return entry.ast;
        }

        auto path = String("resolve/modules/") + context->findName(name) + String(".yana");
        auto opened = File::openFile(path, readAccess());
        if(opened.isErr()) return nullptr;

        auto file = opened.moveUnwrapOk();
        auto size = file.size();
        Ptr<char, HeapDeleter> text { (char*)hAlloc(size) };
        file.read({ (Byte*)text.get(), size });

        Lexer lexer(*context, context->diagnostics, StringView { text.get(), size }, name);
        Parser parser(*context, lexer, name);
        auto ast = new ast::Module(parser.parseModule());

        loaded.push(Loaded { name, ::move(text), size, ast });
        return ast;
    }
};

/*
 * A diagnostics sink that keeps what was reported instead of printing it.
 *
 * A rejection fixture asserts the diagnostics themselves, so they have to be comparable text rather
 * than something written to the console. The recorded form is deliberately narrower than
 * PrintDiagnostics': the line, the column, the level and the message, and none of the quoted source
 * - a fixture asserting the underline would be asserting the diagnostic printer rather than the
 * rule that produced the diagnostic.
 */
struct RecordDiagnostics: Diagnostics {
    using Diagnostics::Diagnostics;

    void message(Level level, StringView text, const Location* where) override {
        Diagnostics::message(level, text, where);

        const char* type;
        switch(level) {
            case ErrorLevel: type = "error"; break;
            case WarningLevel: type = "warning"; break;
            default: type = "message"; break;
        }

        format(recorded, String("%@:%@: %@: %@\n"),
               where ? where->sourceStart.line + 1 : 0,
               where ? where->sourceStart.column : 0,
               type, text);
    }

    StringBuilder recorded;
};

static bool fileExists(const String& path) {
    auto info = File::info(path);
    return info && !info.unwrapOk().isDirectory;
}

static bool compareText(const String& path, ByteBuffer actual) {
    auto opened = File::openFile(path, readAccess());
    if(opened.isErr()) {
        println("cannot open file %@: error %@", path, (U32)opened.unwrapErr());
        return false;
    }

    auto file = opened.moveUnwrapOk();
    auto size = file.size();
    Ptr<Byte, HeapDeleter> expected { (Byte*)hAlloc(size) };
    file.read({ expected.get(), size });

    auto equal = size == actual.length && compareMem(expected.get(), actual.ptr, size) == 0;
    if(!equal) {
        println("Fail (%@). Got:", path);
        print(StringView { (const char*)actual.ptr, actual.length });
        println("\nExpected:");
        print(StringView { (const char*)expected.get(), size });
        println("");
    }
    return equal;
}

template<class Print>
static void writeText(const String& path, Print&& printValue) {
    try {
        Net::FileStream file;
        file.open(path, writeAccess(), File::CreateAlways);
        Net::Writer writer(Net::WriteStream(file), 16384);
        printValue(writer);
        writer.flush();
    } catch(const Net::Exception& error) {
        logError("cannot create expect file \"%@\": %@", path, error.description);
    }
}

static Maybe<I64> readExpectedRun(const String& path) {
    auto info = File::info(path);
    if(!info || info.unwrapOk().isDirectory) return Nothing();

    auto opened = File::openFile(path, readAccess());
    if(opened.isErr()) return Nothing();
    auto file = opened.moveUnwrapOk();
    auto size = file.size();
    Ptr<char, HeapDeleter> text { (char*)hAlloc(size + 1) };
    file.read({ (Byte*)text.get(), size });
    text.get()[size] = 0;

    char* end = nullptr;
    auto value = strtoll(text.get(), &end, 10);
    return end == text.get() ? Nothing() : Just(I64(value));
}

static Maybe<I64> executeMain(Context& context, Program& resolved, LowerModule& module) {
#if defined(__x86_64__) && (defined(__unix__) || defined(__APPLE__))
    auto base = *module.arena;
    AsmModule assembly;

    for(auto functionPointer: module.functions) {
        auto function = base[functionPointer];
        MachineFunction machine;
        transformFunction(context, base, *function, machine);
        auto registers = allocateRegisters(context, base, *function, machine);
        genFunction(context, base, assembly, *function, machine, registers);
    }

    // Globals go after every function, since this is a flat buffer rather than an object file
    // with sections - see AsmModule::addGlobal.
    for(auto globalPointer: module.globals) assembly.addGlobal(base, base[globalPointer]);
    assembly.resolveRelocations();

    auto mainName = Context::nameHash("main", 4);
    auto foundMain = module.functions.get(mainName);
    if(!foundMain) return Nothing();
    auto mainFunction = base[foundMain.unwrap()];
    auto offset = assembly.functionOffsets.getValue(mainFunction);
    if(!offset) return Nothing();

    auto byteCount = assembly.buffer.offset();
    auto page = Size(sysconf(_SC_PAGESIZE));
    auto allocationSize = (byteCount + page - 1) & ~(page - 1);
    auto memory = mmap(nullptr, allocationSize, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if(memory == MAP_FAILED) return Nothing();

    // The addresses inside constant data are only knowable now: a witness table holding a function
    // pointer needs the address the module was actually mapped at. Patched before the copy rather
    // than after, since it is the assembler's buffer that is patched and the mapping is a copy of
    // it. A linker would emit these as dynamic relocations and let the loader do the same thing.
    assembly.applyDataRelocations((Byte*)memory);
    copy(assembly.buffer.buffer, (Byte*)memory, byteCount);

    // Writable as well as executable, because the code and the globals share one mapping here.
    // A real linker gives them separate sections with separate protection; this driver exists to
    // run a fixture, and splitting the buffer would mean teaching it what a section is.
    if(mprotect(memory, allocationSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        munmap(memory, allocationSize);
        return Nothing();
    }

    auto resolvedMain = resolved.root->functions.get(mainName);
    if(!resolvedMain) {
        munmap(memory, allocationSize);
        return Nothing();
    }
    auto returnType = (*resolved.types)[(*resolved.arena)[resolvedMain.unwrap()]->returnType];

    I64 result;
    auto address = (Byte*)memory + offset.unwrap();
    if(returnType->kind == Type::Int &&
       ((IntType*)returnType)->width == IntType::Long) {
        result = ((I64 (*)())address)();
    } else {
        result = ((I32 (*)())address)();
    }
    munmap(memory, allocationSize);
    return Just(result);
#else
    (void)context;
    (void)resolved;
    (void)module;
    return Nothing();
#endif
}

/*
 * A fixture that asserts what the resolver *rejects*.
 *
 * Ownership is the first milestone where the interesting output of a fixture is a diagnostic
 * rather than an instruction, so rejection is a fixture mode rather than a failure: a `.yana` file
 * with a `.errors.expect` beside it is expected to report, and the reported text is what is
 * compared. Nothing after resolution runs for one - an IR built while errors were being reported
 * has no reason to lower or to mean anything.
 *
 * The mode is opted into by the file's existence rather than by anything in the source, so a new
 * rejection fixture is an empty `.errors.expect` plus a `generate` run.
 */
static bool runRejectionTest(const String& path, const String& errorPath, StringView source, bool generate) {
    TestProvider provider;
    provider.source = source;
    RecordDiagnostics diagnostics(provider);
    Context context(diagnostics);
    provider.context = &context;

    auto name = context.addUnqualifiedName("ResolveTest", 11);
    Lexer lexer(context, diagnostics, source, name);
    Parser parser(context, lexer, name);
    auto ast = parser.parseModule();
    resolveProgram(context, ast, &provider);

    if(generate) {
        writeText(errorPath, [&](Net::Writer& writer) {
            writer.writeString(StringView { diagnostics.recorded.pointer(), diagnostics.recorded.size() });
        });
    }

    auto pass = true;
    if(!diagnostics.errorCount()) {
        println("Fail (%@): expected the resolver to report, and it accepted the program.", path);
        pass = false;
    }

    pass = compareText(errorPath, ByteBuffer((Byte*)diagnostics.recorded.pointer(), diagnostics.recorded.size())) && pass;
    println("Running test \"%@\"... %@", path, pass ? "Pass."_v : "Fail."_v);
    return pass;
}

/*
 * The same fixture, compiled the other way.
 *
 * Design.md's "Generic and specialized code" makes both forms first-class outputs and says the
 * equivalence runs both ways: removing a specialization must only make code slower, and adding one
 * must only make it faster. Neither may change what the program does, so this compiles the whole
 * thing with specialization declined and checks that `main` still answers the same.
 *
 * A fixture whose callees have requirements no witness exists for yet falls back to specializing
 * per call site, so this passes trivially for those rather than failing - which is the honest
 * report while the witness half is being filled in.
 */
static bool runGenericPass(const String& path, StringView source, I64 expected) {
    TestProvider provider;
    provider.source = source;
    PrintDiagnostics diagnostics(provider);
    Context context(diagnostics);
    provider.context = &context;

    auto name = context.addUnqualifiedName("ResolveTest", 11);
    Lexer lexer(context, diagnostics, source, name);
    Parser parser(context, lexer, name);
    auto ast = parser.parseModule();
    auto module = resolveProgram(context, ast, &provider, Program::Specialization::Generic);

    if(diagnostics.errorCount()) {
        println("Fail (%@): forced-generic resolution produced %@ diagnostics.", path, diagnostics.errorCount());
        return false;
    }

    auto lowered = lowerProgram(context, *module);
    if(!validateLowerModule(&diagnostics, lowered.get())) {
        println("Fail (%@): forced-generic lowering produced invalid lower IR.", path);
        return false;
    }

    auto actual = executeMain(context, *module, *lowered);
    if(!actual || actual.unwrap() != expected) {
        println("Fail (%@): forced-generic amd64 main returned %@, expected %@.", path,
                actual ? actual.unwrap() : I64(-1), expected);
        return false;
    }

    return true;
}

/*
 * The same fixture, compiled for the JavaScript target.
 *
 * A second resolution rather than a second walk of the first one, because `@platform` selects which
 * declarations *exist* (Analysis-JS.md §2.4): a JS build and a native build do not share a resolved
 * program, and pretending they did would be the "the semantics are whatever the backend does" drift
 * the second target is there to prevent.
 *
 * Opted into by the `.js.expect` file, on the same terms as the ownership dump. Most fixtures reach
 * `Native` - through `[a]`, through anything heap-placed - and a JS target that has no host `Array`
 * yet has nothing useful to say about those; the ones that opt in are the ones that are about
 * something this backend implements.
 */
static bool runJsPass(const String& path, const String& jsPath, StringView source, bool generate,
                      bool forceGeneric) {
    TestProvider provider;
    provider.source = source;
    PrintDiagnostics diagnostics(provider);
    Context context(diagnostics);
    context.settings.mode = CompileMode::JsExecutable;
    provider.context = &context;

    auto name = context.addUnqualifiedName("ResolveTest", 11);
    Lexer lexer(context, diagnostics, source, name);
    Parser parser(context, lexer, name);
    auto ast = parser.parseModule();
    auto module = resolveProgram(context, ast, &provider,
                                 forceGeneric ? Program::Specialization::Generic
                                              : Program::Specialization::Always);

    if(diagnostics.errorCount()) {
        println("Fail (%@): resolving for the JS target produced %@ diagnostics.", path, diagnostics.errorCount());
        return false;
    }

    auto file = js::genProgram(context, *module);

    if(diagnostics.errorCount()) {
        println("Fail (%@): the JS backend produced %@ diagnostics.", path, diagnostics.errorCount());
        return false;
    }

    if(generate) {
        writeText(jsPath, [&](Net::Writer& writer) {
            js::formatFile(writer, context, *file, false);
        });
    }

    Net::Writer writer(16384);
    js::formatFile(writer, context, *file, false);
    return compareText(jsPath, writer.getBuffered());
}

static bool runTest(const String& path, StringView source, bool generate) {
    auto errorPath = path + String(".errors.expect");
    if(fileExists(errorPath)) return runRejectionTest(path, errorPath, source, generate);

    /*
     * A fixture that is *about* the erased ABI compiles in that mode to begin with, so its expected
     * IR is the erased form rather than a pile of specializations. Opted into by a marker file
     * beside the source, for the same reason the ownership dump is: most fixtures are not about
     * this, and generating the other form for all of them would assert the optimizer everywhere.
     */
    auto forceGeneric = fileExists(path + String(".generic"));

    TestProvider provider;
    provider.source = source;
    PrintDiagnostics diagnostics(provider);
    Context context(diagnostics);
    provider.context = &context;

    auto name = context.addUnqualifiedName("ResolveTest", 11);
    Lexer lexer(context, diagnostics, source, name);
    Parser parser(context, lexer, name);
    auto ast = parser.parseModule();
    auto module = resolveProgram(context, ast, &provider,
                                 forceGeneric ? Program::Specialization::Generic
                                              : Program::Specialization::Always);
    if(diagnostics.errorCount()) {
        println("Fail (%@): resolver produced %@ diagnostics.", path, diagnostics.errorCount());
        return false;
    }

    auto resolvePath = path + String(".resolve.expect");
    auto lowerPath = path + String(".lower.expect");

    if(generate) {
        writeText(resolvePath, [&](Net::Writer& writer) {
            printProgram(writer, context, *module);
        });
    }

    Net::Writer resolveWriter(16384);
    printProgram(resolveWriter, context, *module);
    auto pass = compareText(resolvePath, resolveWriter.getBuffered());

    // The ownership dump is opt-in per fixture rather than produced for every one. Liveness of a
    // function with nothing to own says nothing, and generating it everywhere would put a third
    // expectation beside twenty fixtures that are not about ownership.
    auto ownPath = path + String(".own.expect");
    if(fileExists(ownPath)) {
        if(generate) {
            writeText(ownPath, [&](Net::Writer& writer) {
                printOwnership(writer, context, *module);
            });
        }

        Net::Writer ownWriter(16384);
        printOwnership(ownWriter, context, *module);
        pass = compareText(ownPath, ownWriter.getBuffered()) && pass;
    }

    auto jsPath = path + String(".js.expect");
    if(fileExists(jsPath)) pass = runJsPass(path, jsPath, source, generate, forceGeneric) && pass;

    auto lowered = lowerProgram(context, *module);

    // Written before it is validated, deliberately: the whole value of a validation failure is
    // being able to look at the IR that failed, and it is exactly then that the expect file has
    // not been produced yet.
    if(generate) {
        writeText(lowerPath, [&](Net::Writer& writer) {
            printModule(writer, context, *lowered->arena, *lowered);
        });
    }

    if(!validateLowerModule(&diagnostics, lowered.get())) {
        println("Fail (%@): lowering produced invalid lower IR.", path);
        return false;
    }

    Net::Writer lowerWriter(16384);
    printModule(lowerWriter, context, *lowered->arena, *lowered);
    pass = compareText(lowerPath, lowerWriter.getBuffered()) && pass;

    auto runPath = path + String(".run.expect");
    if(auto expected = readExpectedRun(runPath)) {
        auto actual = executeMain(context, *module, *lowered);
        if(!actual || actual.unwrap() != expected.unwrap()) {
            println("Fail (%@): amd64 main returned %@, expected %@.", path,
                    actual ? actual.unwrap() : I64(-1), expected.unwrap());
            pass = false;
        }

        // The same program with every concrete generic call site forced through the erased ABI
        // instead of a specialization. Implementation-Generics.md §14 asks for exactly this, and
        // for the reason it gives: it is the most direct guard against a semantic decision quietly
        // moving into the optimizer. Only the *result* is compared - the two forms legitimately
        // produce different IR, and that they do is the point.
        if(!forceGeneric) pass = runGenericPass(path, source, expected.unwrap()) && pass;
    }

    println("Running test \"%@\"... %@", path, pass ? "Pass."_v : "Fail."_v);
    return pass;
}

int main(int argc, const char** argv) {
    auto generate = argc > 1 && String(argv[1]) == "generate";
    Array<String> tests;

    listDirectory("resolve", [&](const String& name, bool directory) {
        if(directory) return;
        if(auto dot = findLastChar(stringView(name), '.')) {
            if(String(dot + 1, name.text() + name.size() - dot - 1) == "yana") {
                tests.push(String("resolve/") + name);
            }
        }
    });

    if(tests.isEmpty()) {
        println("no resolve tests found");
        return 1;
    }

    auto pass = true;
    for(auto& test: tests) {
        auto opened = File::openFile(test, readAccess());
        if(opened.isErr()) {
            println("cannot open file %@: error %@", test, (U32)opened.unwrapErr());
            pass = false;
            continue;
        }

        auto file = opened.moveUnwrapOk();
        auto size = file.size();
        Ptr<char, HeapDeleter> source { (char*)hAlloc(size) };
        file.read({ (Byte*)source.get(), size });
        pass = runTest(test, { source.get(), size }, generate) && pass;
    }

    return pass ? 0 : 1;
}
