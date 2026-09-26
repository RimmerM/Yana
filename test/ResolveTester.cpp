#include <Core.h>
#include <File.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include "../compiler/parse/parser.h"
#include "../compiler/resolve_mir/explain.h"
#include "../compiler/lower/lower_print.h"
#include "../compiler/lower/lower_validate.h"
#include "../compiler/codegen/x64/gen.h"
#include "../compiler/codegen/js/jsir.h"
#include "../compiler/codegen/llvm/gen.h"
#include "../compiler/resolve_ssa/pipeline.h"
#include "../compiler/resolve_ssa/print.h"
#include "../compiler/resolve_ssa/lower/repr_print.h"
#include "../compiler/resolve_mir/print.h"
#include "Net/Stream.h"
#include "Net/File.h"
#include "shard.h"
#include "directives.h"
#include "provider.h"

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <spawn.h>
#include <csignal>

extern char** environ;
#endif

using namespace Tritium;

// Where an `import` finds its module. Named once because two things read it: the provider
// (`provider.h`), which answers imports out of it, and the fixture listing in `main`, which must not
// mistake the modules for fixtures - they are the *inputs* of fixtures elsewhere in the corpus, and
// compiling one on its own reports the diagnostics of a program with no `main`.
static const char* kModuleDirectory = "resolve/modules";

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

/*
 * The front end every pass in this file opens with - provider, diagnostics, context, directives, the
 * settings it overrides and the parse.
 *
 * The passes here began with the same eleven lines and differed in two things: which diagnostics they
 * collect, and which settings they set. The settings went in the *middle* of the block, so a line
 * added to the front end had to be added once per pass, and one of them forgetting it
 * is a fixture compiled a way nothing says out loud. The diagnostics are the template parameter -
 * two passes record what was reported and the rest print it - and `settings` is a callback that runs
 * exactly where the overrides used to, between the directives and the parse. The members are in
 * declaration order, which is what makes the parse see the settings rather than the other way round.
 *
 * Not in `directives.h`, which is where this slice's plan put it: `TestProvider` reads
 * `resolve/modules/`, and that header is what three drivers share *because* it is only about
 * settings.
 */
template<class Diags>
struct FixtureFront {
    TestProvider provider;
    Diags diagnostics;
    Context context;
    ast::Module ast;

    template<class Settings>
    FixtureFront(StringView source, Settings&& settings):
        diagnostics(provider), context(diagnostics), ast(open(source, settings)) {}

    explicit FixtureFront(StringView source):
        FixtureFront(source, [](CompileSettings&) {}) {}

private:
    template<class Settings>
    ast::Module open(StringView source, Settings&& settings) {
        provider.source = source;
        provider.moduleRoot = String(kModuleDirectory);

        /*
         * The local backend, because it is what every native pass here runs: `executeModule` is the
         * x64 JIT, and `.mir.lower.expect` is documented as what `-backend local` is handed. The
         * field's default is LLVM (see CompileSettings::backend), and until J182 this driver lowered
         * for LLVM and then ran x64, so a transform gated to the local backend
         * (`LowerModule::consumer`) was in no golden and ran in no fixture. `runLlvmPass` lowers a
         * second time for LLVM. Ignored by the JavaScript passes, which never lower.
         */
        context.settings.backend = NativeBackend::Local;
        applyFixtureDirectives(context.settings, source);
        settings(context.settings);
        provider.context = &context;

        auto name = context.addUnqualifiedName("ResolveTest", 11);
        Lexer lexer(context, diagnostics, source, name);
        Parser parser(context, lexer, name);
        return parser.parseModule();
    }
};

// The JavaScript passes' front end. The native passes take the default, which leaves the platform
// alone rather than setting it to `Native`: a fixture may have said `# platform: js`, and the
// directives have already run by the time the settings callback does.
inline void jsSettings(CompileSettings& settings) {
    settings.platform = TargetPlatform::Js;
}

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

/*
 * Assembling a lowered module in memory and calling its entry point.
 *
 * `unitResult` and `wideResult` are handed in rather than read off the lowered module, which has no
 * types above its five machine ones - the callers read them off the entry's declaration.
 */
static Maybe<I64> executeModule(Context& context, LowerModule& module, bool unitResult, bool wideResult) {
#if defined(__x86_64__) && (defined(__unix__) || defined(__APPLE__))
    auto base = *module.arena;
    AsmModule assembly;

    // One of each across the whole module: the allocator writes into them rather than building its
    // own, so allocating a module costs the largest function's storage instead of the sum of every
    // function's. The records each function's `registers` points into are consumed by genFunction
    // before the next one is allocated, so the arena holding them is emptied in step - see
    // RegScratch::resetRecords.
    RegScratch scratch;
    FunctionRegs registers;
    MachineFunction machine;

    for(auto functionPointer: module.functionOrder) {
        auto function = base[functionPointer];
        machine.reset();
        // Refused by the backend and already reported - the driver's own diagnostic count is what
        // fails the fixture. Nothing after it may run: see `transformFunction`.
        if(!transformFunction(context, base, *function, machine)) continue;

        scratch.resetRecords();
        allocateRegisters(context, base, *function, machine, scratch, registers);
        genFunction(context, base, assembly, *function, machine, registers);
    }

    // Globals go after every function, since this is a flat buffer rather than an object file
    // with sections - see AsmModule::addGlobal.
    for(auto globalPointer: module.globalOrder) assembly.addGlobal(base, base[globalPointer]);
    assembly.resolveRelocations(module.imageAnchor ? base[module.imageAnchor] : nullptr);

    // The program's start rather than `main` by name: where the fixture has top-level statements,
    // `main` is what the synthesized entry calls last, and running `main` on its own would run the
    // fixture with its own initialization skipped - see Program::entry.
    if(!module.entry) return Nothing();

    auto foundMain = module.functions.get(module.entry);
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
    //
    // Checked, though a JIT's module has no absolute sites (`LowerModule::fixedLoadAddress` is zero
    // here): a refusal is a module that would read the wrong addresses, and running it would report
    // that as whatever the fixture then happened to answer.
    if(!assembly.applyDataRelocations((Byte*)memory)) {
        println("Fail: an absolute address does not fit the JIT mapping at %@.", (U64)memory);
        munmap(memory, allocationSize);
        return Nothing();
    }
    copy(assembly.buffer.buffer, (Byte*)memory, byteCount);

    // Writable as well as executable, because the code and the globals share one mapping here.
    // A real linker gives them separate sections with separate protection; this driver exists to
    // run a fixture, and splitting the buffer would mean teaching it what a section is.
    if(mprotect(memory, allocationSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        munmap(memory, allocationSize);
        return Nothing();
    }

    I64 result;
    auto address = (Byte*)memory + offset.unwrap();

    if(unitResult) {
        // A program that answers nothing exits zero, which is what the native wrapper says about the
        // same function (see addNativeEntry) and what C says about falling off the end of `main`.
        // Reading a register the callee never wrote would make a program with no `main` answer
        // whatever happened to be in it.
        ((void (*)())address)();
        result = 0;
    } else if(wideResult) {
        result = ((I64 (*)())address)();
    } else {
        result = ((I32 (*)())address)();
    }
    munmap(memory, allocationSize);
    return Just(result);
#else
    (void)context;
    (void)module;
    (void)unitResult;
    (void)wideResult;
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
 *
 * A rejection is the one output that has no golden of its own, because a program the stage refuses
 * produces no IR to print; this is where such a fixture is asserted, and it is the whole reason the
 * ownership analysis has fixtures about what it *reports* rather than only about what it emits.
 */
static bool runRejectionTest(const String& path, const String& errorPath, StringView source, bool generate) {
    FixtureFront<RecordDiagnostics> f(source);
    auto& context = f.context;
    auto& diagnostics = f.diagnostics;

    // Resolution, and past it only where resolution reported nothing: the pipeline's later halves
    // assume a program the checks accepted, so running them over one that was rejected asserts
    // nothing and asks the verifier about IR that was never meant to be complete.
    //
    // A clean one goes on - conversion, the optimizer and the lowering - because a few refusals are
    // the lowering's: whether a field shares a machine word is a layout question, so `addressOf` of
    // one escaping as a `%T` (`Reject.PackedAddress.yana`) is refused where the layout is known and
    // nowhere earlier.
    ssa::Pipeline pipeline(context);
    if(pipeline.resolve(f.ast, &f.provider) && !diagnostics.errorCount() && pipeline.convert() &&
       !diagnostics.errorCount() && pipeline.optimize() && !diagnostics.errorCount()) {
        (void)pipeline.lower();
    }

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
 * Running the emitted JavaScript, to compare what `main` answers with what the amd64 backend answered.
 *
 * The gap this closes is that nothing executed the JS at all: the backend was asserted by its own
 * golden text, so the two targets could - and repeatedly did - disagree about what a program *means*
 * with every fixture still green. Four such differences were found by hand before this existed, and
 * every one of them would have been a failing test here.
 *
 * The expected value is the one the native run is checked against, because agreeing with each other
 * is the property worth asserting; a fixture whose JS answer legitimately differs says so by putting
 * the other number in a `.js.run.expect` beside it - see `jsirExpectedRun`.
 *
 * The text run is the text just emitted rather than the file on disk, so a stale golden cannot make
 * this pass by testing the previous compiler's output.
 */
static bool nodeAvailable() {
    static int cached = -1;
    if(cached < 0) {
        cached = system("node --version > /dev/null 2>&1") == 0;
        if(!cached) println("Note: `node` is not on PATH, so the JavaScript output is not being run.");
    }

    return cached != 0;
}

#if defined(__unix__) || defined(__APPLE__)

/*
 * One Node process for the whole run, spoken to over a pair of pipes.
 *
 * A process per script was what this replaced, and it was the whole cost of the JavaScript half:
 * Node starts in about eleven milliseconds and the suite started it a hundred and seventy times -
 * once per fixture with a `.js.expect`, and once more for the unoptimized build it is compared
 * against. Neither the shell that `system()` used nor the two files the script and its output went
 * through were worth removing on their own; both were measured and both were free. Node's startup
 * is not, and it is the only way to get at it.
 *
 * What the fixtures need isolated is the *program*, not the process - a program that answers
 * correctly only because of what the previous one left behind is exactly what this suite exists to
 * catch. `node-harness.js` evaluates each script with `vm.runInNewContext`, which is a fresh global
 * and fresh intrinsics per script; the emitted code refers to nothing outside itself but
 * `console.log`, so there is nothing else for one fixture to leave for the next.
 *
 * A crash takes the harness with it, so the child is restarted on the next fixture rather than
 * making every fixture after the first failure fail too.
 */
struct NodeHarness {
    int toChild = -1;
    int fromChild = -1;
    pid_t child = -1;

    bool start() {
        int in[2];
        int out[2];
        if(pipe(in) != 0) return false;
        if(pipe(out) != 0) {
            close(in[0]);
            close(in[1]);
            return false;
        }

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, in[0], STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, in[1]);
        posix_spawn_file_actions_addclose(&actions, out[0]);

        // `posix_spawnp` rather than an absolute path, so which `node` runs is still the one PATH
        // names. stderr is left alone: the harness reports a script's failure in its answer, and
        // anything Node says on its own account is a problem worth seeing.
        const char* argv[] = { "node", "node-harness.js", nullptr };
        auto spawned = posix_spawnp(&child, "node", &actions, nullptr, (char* const*)argv, environ);
        posix_spawn_file_actions_destroy(&actions);

        close(in[0]);
        close(out[1]);

        if(spawned != 0) {
            close(in[1]);
            close(out[0]);
            child = -1;
            return false;
        }

        toChild = in[1];
        fromChild = out[0];
        return true;
    }

    void stop() {
        if(child < 0) return;

        close(toChild);
        close(fromChild);

        int status = 0;
        while(waitpid(child, &status, 0) < 0 && errno == EINTR) {}

        toChild = fromChild = -1;
        child = -1;
    }

    bool writeAll(const char* data, Size length) {
        while(length) {
            auto wrote = ::write(toChild, data, length);
            if(wrote <= 0) return false;

            data += wrote;
            length -= Size(wrote);
        }

        return true;
    }

    /// A byte at a time, because the count on this line says where the payload begins and reading
    /// past it would eat the payload's first bytes. Headers are a handful of bytes.
    bool readHeader(char* line, Size capacity) {
        Size at = 0;
        while(at < capacity - 1) {
            char c;
            auto got = ::read(fromChild, &c, 1);
            if(got <= 0) return false;
            if(c == '\n') break;

            line[at++] = c;
        }

        line[at] = 0;
        return true;
    }

    bool readAll(char* buffer, Size length) {
        Size at = 0;
        while(at < length) {
            auto got = ::read(fromChild, buffer + at, length - at);
            if(got <= 0) return false;

            at += Size(got);
        }

        return true;
    }

    /*
     * Runs one script. `threw` distinguishes a script that reported a failure from a harness that
     * stopped answering: the first is this fixture's problem and the second is everyone's.
     */
    bool run(ByteBuffer emitted, char* buffer, Size capacity, Size& read, bool& threw) {
        read = 0;
        buffer[0] = 0;
        threw = false;

        if(child < 0 && !start()) return false;

        // The script as it was emitted, and nothing appended to it. An emitted file calls the
        // program's entry itself now, so the status is the script's completion value and the harness
        // prints it - see Analysis-Initialization.md stage B. Appending a call of our own would run
        // a program with top-level statements twice.
        char header[64];
        auto length = snprintf(header, sizeof(header), "%zu\n", Size(emitted.length));

        if(!writeAll(header, Size(length)) || !writeAll((const char*)emitted.ptr, emitted.length)) {
            stop();
            return false;
        }

        char answer[64];
        if(!readHeader(answer, sizeof(answer))) {
            stop();
            return false;
        }

        char* rest = nullptr;
        threw = answer[0] == 'E';
        auto payload = Size(strtoll(answer + (threw ? 4 : 3), &rest, 10));

        // Read the whole payload even where it does not fit, so that the next answer starts where
        // the harness says it does rather than in the middle of this one's overflow.
        Size kept = 0;
        while(kept < payload) {
            char scratch[512];
            auto want = min(payload - kept, sizeof(scratch));
            if(!readAll(scratch, want)) {
                stop();
                return false;
            }

            for(Size i = 0; i < want && read < capacity - 1; i++) buffer[read++] = scratch[i];
            kept += want;
        }

        buffer[read] = 0;
        return !threw;
    }
};

static NodeHarness& nodeHarness() {
    static NodeHarness harness;
    return harness;
}

#endif

static Maybe<I64> executeJsMain(const String& path, ByteBuffer emitted) {
    char buffer[512] = {};
    Size read = 0;
    auto ok = false;

#if defined(__unix__) || defined(__APPLE__)
    // A test driver has no use for the default: a harness that has died turns every later write into
    // a signal that kills the driver rather than a failure it can report.
    static auto ignored = signal(SIGPIPE, SIG_IGN);
    (void)ignored;

    auto threw = false;
    ok = nodeHarness().run(emitted, buffer, sizeof(buffer), read, threw);
#endif

    if(!ok) {
        println("Fail (%@): running the emitted JavaScript failed: %@", path,
                StringView(buffer, U32(read)));
        return Nothing();
    }

    /*
     * The *last* line, because a fixture is allowed to print.
     *
     * The status is the final line by construction - the harness writes the script's completion
     * value there, which is what the entry call the file ends with produced - so anything before it
     * is the program's own output. Parsing from the front instead made a fixture that called `print`
     * report a JavaScript failure with no failure in it, which is the one thing `print` cannot be
     * tested without doing.
     */
    auto start = buffer;
    for(Size i = read; i > 0; i--) {
        if(buffer[i - 1] != '\n' && buffer[i - 1] != '\r') continue;
        if(i == read) continue;

        start = buffer + i;
        break;
    }

    char* end = nullptr;
    auto value = strtoll(start, &end, 10);
    return end == start ? Nothing() : Just(I64(value));
}

/*
 * The same fixture, handed to LLVM.
 *
 * The same optimized SSA rather than a second resolution, unlike the JS pass: this backend and the
 * x64 one consume the same IR, so what it asserts is the translation into LLVM's form and nothing
 * about the stages in front of it. Opted into by the `.mir.llvm.expect` file, on the same terms as
 * the rest - a fixture that says nothing new about the translation would only be a second copy of
 * what `.mir.lower.expect` already asserts.
 *
 * **Lowered a second time, for LLVM** (J182): the module the x64 run used was lowered for the local
 * backend, and what LLVM is handed differs from it - the calling convention's budget and the
 * transforms `LowerModule::consumer` gates. The lowering reads the SSA and the declarations and
 * writes only its own module, so a second one from the same pipeline is what `-backend llvm` would
 * have lowered; it costs one lowering for each of the few fixtures that opt in. Sharing the local
 * module instead would have asserted a module no build hands LLVM.
 *
 * The verifier runs whether or not the text matches, because a module LLVM rejects is a failure of
 * this backend even when the expect file was generated from it.
 */
static bool runLlvmPass(const String& path, const String& llvmPath, Context& context,
                        Diagnostics& diagnostics, ssa::Pipeline& pipeline, bool generate) {
    auto backend = context.settings.backend;
    context.settings.backend = NativeBackend::Llvm;
    auto relowered = pipeline.lower();
    context.settings.backend = backend;

    if(!relowered || diagnostics.errorCount()) {
        println("Fail (%@): the pipeline did not lower for LLVM.", path);
        return false;
    }

    if(!validateLowerModule(&diagnostics, relowered.get())) {
        println("Fail (%@): the pipeline produced invalid lower IR for LLVM.", path);
        return false;
    }

    auto& lowered = *relowered;
    llvm::LLVMContext llvm;
    auto errors = diagnostics.errorCount();
    auto module = llvmgen::genModule(llvm, context, lowered);

    if(diagnostics.errorCount() > errors) {
        println("Fail (%@): the LLVM backend produced %@ diagnostics.", path, diagnostics.errorCount() - errors);
        return false;
    }

    if(!llvmgen::verifyGenModule(context, *module)) {
        println("Fail (%@): the LLVM backend produced a module the verifier rejects.", path);
        return false;
    }

    if(generate) {
        writeText(llvmPath, [&](Net::Writer& writer) {
            llvmgen::printModule(writer, *module);
        });
    }

    Net::Writer writer(16384);
    llvmgen::printModule(writer, *module);
    return compareText(llvmPath, writer.getBuffered());
}

/*
 * The same fixture with the optimizer switched off.
 *
 * An optimization may make a program faster and may not make it different, so the honest test is
 * compiling it both ways and comparing what it does. Only the *result* is compared - the two forms
 * legitimately produce different IR, and that they do is the point.
 *
 * **Against the optimized build's own answer rather than against `.run.expect`**, which is what
 * makes this an assertion of its own instead of a second copy of the run check: the expectation
 * file is a number a human worked out, and this compares the compiler with itself.
 *
 * A second `Pipeline` from the same source rather than a second lowering of the same SSA: the
 * optimizer rewrites the body in place, and there is no way to un-optimize one.
 *
 * `-no-ir-opt` reaches the lower tier as well (see `lower/lower_pipeline.h`), so what this compares
 * is the whole of what the flag switches off. The JavaScript half is `runJsirPass`'s.
 */
static bool runUnoptimizedPass(const String& path, StringView source, I64 optimized) {
    FixtureFront<PrintDiagnostics> f(source, [](CompileSettings& settings) {
        settings.optimizeIr = false;
    });

    auto& context = f.context;
    auto& diagnostics = f.diagnostics;

    ssa::Pipeline pipeline(context);
    if(!pipeline.resolve(f.ast, &f.provider) || !pipeline.convert()) {
        println("Fail (%@): the unoptimized pipeline produced %@ diagnostics.", path,
                diagnostics.errorCount());
        return false;
    }

    auto decl = *pipeline.program->decl;
    auto entry = pipeline.program->entry ? decl[pipeline.program->entry] : nullptr;
    auto resultType = entry ? decl[entry->result.type] : nullptr;
    auto unitResult = !resultType || resultType->kind == mir::Type::Unit;
    auto wideResult = resultType && resultType->kind == mir::Type::Int &&
                      ((mir::IntType*)resultType)->bits == 64;

    // The same call order the optimized build takes, including the optimizer itself - whose gate
    // returns immediately here. The two builds differ in the setting rather than in which calls the
    // tester makes, which is what keeps this a test of the setting.
    pipeline.releaseMir();
    pipeline.optimize();

    auto lowered = pipeline.lower();
    if(!lowered || diagnostics.errorCount()) {
        println("Fail (%@): the unoptimized pipeline did not lower.", path);
        return false;
    }

    if(!validateLowerModule(&diagnostics, lowered.get())) {
        println("Fail (%@): the unoptimized pipeline produced invalid lower IR.", path);
        return false;
    }

    auto actual = executeModule(context, *lowered, unitResult, wideResult);
    if(actual && actual.unwrap() == optimized) return true;

    println("Fail (%@): the optimizer changed the answer - unoptimized main returned %@, optimized %@.",
            path, actual ? actual.unwrap() : I64(-1), optimized);
    return false;
}

/*
 * The fixture as a native program - Analysis-IR.md part 11.3.
 *
 * `.mir.expect` is written for every fixture, and `.ssa.expect` and `.mir.lower.expect` wherever
 * `generate` runs; a fixture that is about neither of the last two may delete them.
 *
 * `.ssa.opt.expect` is the fifth, and it is the SSA after `resolve_ssa/opt` has run. It is opt-in on
 * the stricter terms `.mir.repr.expect` has - the file exists or it does not, and `generate` never
 * creates one - because most fixtures are not about the optimizer and would otherwise grow a second
 * copy of their IR that moves whenever any pass changes. `.ssa.expect` stays the **conversion's**
 * output, printed before the optimizer runs, so the two side by side are the before and after of the
 * whole stage.
 *
 * `.run.expect` is what the program answers, shared with the JavaScript passes: two targets that
 * answer differently is the failure it catches. A fixture that has one is also compiled with the
 * optimizer switched off and the two answers compared - see `runUnoptimizedPass`.
 */
static bool runNativePass(const String& path, StringView source, bool generate,
                          Maybe<I64> expectedRun) {
    FixtureFront<PrintDiagnostics> f(source);
    auto& context = f.context;
    auto& diagnostics = f.diagnostics;

    ssa::Pipeline pipeline(context);
    if(!pipeline.resolve(f.ast, &f.provider)) {
        println("Fail (%@): the resolver produced %@ diagnostics.", path, diagnostics.errorCount());
        return false;
    }

    auto mirPath = path + String(".mir.expect");
    if(generate) {
        writeText(mirPath, [&](Net::Writer& writer) {
            mir::printProgram(writer, context, *pipeline.program);
        });
    }

    Net::Writer mirWriter(16384);
    mir::printProgram(mirWriter, context, *pipeline.program);
    auto pass = compareText(mirPath, mirWriter.getBuffered());

    /*
     * The `explain` query - Analysis-Ambient.md §7.3, opt-in by a `.explain.expect` beside the source.
     *
     * A different assertion from `.mir.expect` even though it reads the same program: that file pins
     * what the passes *found*, and this one pins what a person is *told* - which rows appear, which of
     * them an editor calls surprising, and what a hover therefore contains. The second is the whole
     * point of the feature and none of it follows from the first.
     *
     * Before conversion, deliberately: the locals' placements are read off the MIR bodies, which the
     * conversion releases, and the answers an editor shows are the ones the program had before the
     * optimizer moved anything around.
     */
    auto explainPath = path + String(".explain.expect");
    if(fileExists(explainPath)) {
        if(generate) {
            writeText(explainPath, [&](Net::Writer& writer) {
                mir::printExplanations(writer, context, *pipeline.program);
            });
        }

        Net::Writer explainWriter(16384);
        mir::printExplanations(explainWriter, context, *pipeline.program);
        pass = compareText(explainPath, explainWriter.getBuffered()) && pass;
    }

    /*
     * The native layout this stage chose, opt-in per fixture.
     *
     * This is the only assertion that can see a layout decision nothing has consumed yet - a niche
     * the search found and the access lowering does not use yet is invisible in emitted code. Before
     * conversion, because a layout is a fact about the declarations and asserting one has to be
     * possible for a fixture whose body does something this stage cannot lower yet.
     */
    auto mirReprPath = path + String(".mir.repr.expect");
    if(fileExists(mirReprPath)) {
        // Opt-in, and **not** created by `generate`. A fixture that is not about layout should not
        // grow a layout golden that then churns whenever the library changes; an empty file beside
        // the source is how one opts in.
        if(generate) {
            writeText(mirReprPath, [&](Net::Writer& writer) {
                ssa::printReprs(writer, context, *pipeline.program);
            });
        }

        Net::Writer reprWriter(16384);
        ssa::printReprs(reprWriter, context, *pipeline.program);
        pass = compareText(mirReprPath, reprWriter.getBuffered()) && pass;
    }

    if(!pipeline.convert()) {
        println("Fail (%@): SSA conversion or verification failed.", path);
        return false;
    }

    auto ssaPath = path + String(".ssa.expect");
    if(fileExists(ssaPath) || generate) {
        if(generate) {
            writeText(ssaPath, [&](Net::Writer& writer) {
                ssa::printProgram(writer, context, *pipeline.program, pipeline.converted);
            });
        }

        Net::Writer ssaWriter(16384);
        ssa::printProgram(ssaWriter, context, *pipeline.program, pipeline.converted);
        pass = compareText(ssaPath, ssaWriter.getBuffered()) && pass;
    }

    /*
     * The MIR region, released here - the driver's order, kept because it is the one that is
     * checked. Nothing below conversion may hold an MIR handle (Analysis-IR.md §8.8), and a released
     * region is what makes an accidental one fail loudly; a tester that held the region open would
     * be the one place in the compilation where that rule is not tested.
     */
    pipeline.releaseMir();

    /*
     * And the optimizer over that SSA - `resolve_ssa/opt`.
     *
     * After the `.ssa.expect` comparison above, deliberately: that golden is the **conversion's**
     * output, so it must not move when a pass is added. The pair of goldens is then the before and
     * after of the whole stage.
     */
    if(!pipeline.optimize()) {
        println("Fail (%@): the SSA optimizer produced IR the verifier rejects.", path);
        return false;
    }

    /*
     * The optimized form, opt-in per fixture and **not** created by `generate` - the same terms as
     * `.mir.repr.expect` above, and for the same reason. Most fixtures are not about the optimizer
     * and would otherwise grow a second copy of their IR that churns whenever any pass changes; an
     * empty file beside the source is how one opts in.
     */
    auto ssaOptPath = path + String(".ssa.opt.expect");
    if(fileExists(ssaOptPath)) {
        if(generate) {
            writeText(ssaOptPath, [&](Net::Writer& writer) {
                ssa::printProgram(writer, context, *pipeline.program, pipeline.converted);
            });
        }

        Net::Writer optWriter(16384);
        ssa::printProgram(optWriter, context, *pipeline.program, pipeline.converted);
        pass = compareText(ssaOptPath, optWriter.getBuffered()) && pass;
    }

    // Whether the entry answers nothing, and whether it answers something wider than a register
    // half - read off the declaration.
    auto decl = *pipeline.program->decl;
    auto entry = pipeline.program->entry ? decl[pipeline.program->entry] : nullptr;
    auto resultType = entry ? decl[entry->result.type] : nullptr;
    auto unitResult = !resultType || resultType->kind == mir::Type::Unit;
    auto wideResult = resultType && resultType->kind == mir::Type::Int &&
                      ((mir::IntType*)resultType)->bits == 64;

    auto lowered = pipeline.lower();
    if(!lowered || diagnostics.errorCount()) {
        println("Fail (%@): the pipeline did not lower.", path);
        return false;
    }

    // Written before it is validated, deliberately: the whole value of a validation failure is being
    // able to look at the IR that failed, and it is exactly then that the file has not been
    // produced yet.
    auto lowerPath = path + String(".mir.lower.expect");
    if(fileExists(lowerPath) || generate) {
        if(generate) {
            writeText(lowerPath, [&](Net::Writer& writer) {
                printModule(writer, context, *lowered->arena, *lowered);
            });
        }
    }

    if(!validateLowerModule(&diagnostics, lowered.get())) {
        println("Fail (%@): the pipeline produced invalid lower IR.", path);
        return false;
    }

    if(fileExists(lowerPath)) {
        Net::Writer lowerWriter(16384);
        printModule(lowerWriter, context, *lowered->arena, *lowered);
        pass = compareText(lowerPath, lowerWriter.getBuffered()) && pass;
    }

    // The lowering, handed to LLVM - `runLlvmPass`, on its terms: opted into by the file, and never
    // created by `generate`, since most fixtures say nothing new about the translation.
    auto llvmPath = path + String(".mir.llvm.expect");
    if(fileExists(llvmPath)) {
        pass = runLlvmPass(path, llvmPath, context, diagnostics, pipeline, generate) && pass;
    }

    if(auto expected = expectedRun) {
        auto actual = executeModule(context, *lowered, unitResult, wideResult);
        if(!actual || actual.unwrap() != expected.unwrap()) {
            println("Fail (%@): amd64 main returned %@, expected %@.", path,
                    actual ? actual.unwrap() : I64(-1), expected.unwrap());
            pass = false;
        }

        // And the same program with the optimizer switched off, compared against what this run
        // answered rather than against any file - see `runUnoptimizedPass`. Only where the optimized
        // build produced an answer at all: a second complaint about a failure already reported would
        // only make the first harder to find.
        if(actual) pass = runUnoptimizedPass(path, source, actual.unwrap()) && pass;
    }

    return pass;
}

// The answer both JavaScript pipelines are held to - `.run.expect`, or the documented exception a
// fixture whose JavaScript answer differs on purpose names in `.js.run.expect`.
static Maybe<I64> jsirExpectedRun(const String& path, const String& runPath) {
    auto js = readExpectedRun(path + String(".js.run.expect"));
    return js ? js : readExpectedRun(runPath);
}

/*
 * The same fixture as a **JavaScript** program, resolver and tier alike.
 *
 * Five goldens off one compilation, opted into one at a time. Three of them are the resolver's answer
 * for this target - `.js.mir.expect`, `.js.explain.expect` and `.js.ssa.expect`, milestone 3's and
 * additional to the native ones, so a fixture acquires one without any of its others moving - and two
 * are the tier's: `.jsir.expect` is the JSIR dump and `.jsir.js.expect` is the JavaScript it emits.
 *
 * **It was two passes until 2026-09-22 (J80)** and the front half was written twice. `runOwnershipJsPass`
 * resolved, converted and optimized for this target to print the first three, and this function did
 * exactly the same thing again to reach the tier - so a fixture carrying both goldens was compiled
 * twice for one target, and the two copies could drift about which settings they set. The three
 * goldens are printed at the same two points they were, which is what makes the fold byte-identical:
 * the MIR and the explanation before `convert`, since the conversion releases the bodies their
 * placements are read off, and the SSA before `optimize`.
 *
 * `emitJsir` is the second opt-in read apart from the first, and `resolve/HostOps.yana` is why: it is
 * a fixture about the resolver's JavaScript answer whose program this tier has no form for, so it
 * carries the three and not the two.
 *
 * Three builds and two front ends (§10.2). The optimized one is every golden and the answer the other
 * two are held to; the second is `-no-ir-opt`, which is a different *program* and therefore a
 * compilation of its own; and the third is the same SSA with the **JSIR** optimizer off, which is one
 * more `genJsirProgram` over the object the first one already built. That pair is what says which
 * tier a disagreement came from - a program that differs with `-no-ir-opt` and agrees with the third
 * has a defect above the fork, and the other way round has one below it, which is where
 * `opt_alias.cpp`'s rewrites are - and running the third off the same SSA is what makes it the one
 * axis rather than two: the input is identical by construction instead of by a second resolution
 * being assumed to agree.
 */
static bool runJsirPass(const String& path, StringView source, bool generate,
                        Maybe<I64> expectedRun, bool emitJsir, bool unoptimizedReports = false) {
    auto pass = true;

    // The dump first, because it is what `-mode jsir` writes and it is written *inside* the
    // per-function loop - a JSIR body does not survive it (§7.1), so there is nothing to print
    // afterwards.
    Net::Writer dumpWriter(16384);
    Net::Writer optimized(16384);
    Net::Writer noJsirOpt(16384);

    {
        FixtureFront<PrintDiagnostics> f(source, jsSettings);
        auto& context = f.context;
        auto& diagnostics = f.diagnostics;

        ssa::Pipeline pipeline(context);
        if(!pipeline.resolve(f.ast, &f.provider)) {
            println("Fail (%@): the resolver produced %@ diagnostics for JavaScript.", path,
                    diagnostics.errorCount());
            return false;
        }

        auto golden = [&](const char* suffix, auto&& print) {
            auto file = path + String(suffix);
            if(!fileExists(file)) return;

            if(generate) writeText(file, [&](Net::Writer& writer) { print(writer); });

            Net::Writer writer(16384);
            print(writer);
            pass = compareText(file, writer.getBuffered()) && pass;
        };

        golden(".js.mir.expect", [&](Net::Writer& writer) {
            mir::printProgram(writer, context, *pipeline.program);
        });

        /*
         * The `explain` query, for the **JavaScript** program - the native pass's `.explain.expect`
         * at the other target, opted into by `.js.explain.expect` on the same terms.
         *
         * A golden of its own rather than a second reading of the native one, because the two are
         * genuinely different reports: `@platform` selects which declarations exist, so a JavaScript
         * program has different callees, different placements and a different call-site index. Until
         * 2026-09-22 (J75) the driver refused `explain -platform js` outright, and this is what says
         * the refusal is gone and what the answer is.
         *
         * Before conversion, for the reason the MIR dump is: the locals' placements are read off the
         * MIR bodies, which the conversion releases.
         */
        golden(".js.explain.expect", [&](Net::Writer& writer) {
            mir::printExplanations(writer, context, *pipeline.program);
        });

        if(!pipeline.convert()) {
            println("Fail (%@): SSA conversion or verification failed for JavaScript.", path);
            return false;
        }

        golden(".js.ssa.expect", [&](Net::Writer& writer) {
            ssa::printProgram(writer, context, *pipeline.program, pipeline.converted);
        });

        pipeline.releaseMir();

        if(!pipeline.optimize()) {
            println("Fail (%@): the SSA optimizer produced IR the verifier rejects for JavaScript.",
                    path);
            return false;
        }

        if(!emitJsir) return pass;

        auto file = js::genJsirProgram(context, *pipeline.program, pipeline.converted, &dumpWriter);
        if(diagnostics.errorCount()) {
            println("Fail (%@): JSIR produced %@ diagnostics.", path, diagnostics.errorCount());
            return false;
        }

        js::formatFile(optimized, context, *file, false);

        // And the third configuration off the same SSA - see the header. The setting is read once,
        // in the per-function loop of `assemble.cpp`, so flipping it between two emissions is the
        // whole of what distinguishes them.
        context.settings.optimizeJsir = false;
        auto plain = js::genJsirProgram(context, *pipeline.program, pipeline.converted, nullptr);
        if(diagnostics.errorCount()) {
            println("Fail (%@): JSIR with its optimizer off produced %@ diagnostics.", path,
                    diagnostics.errorCount());
            return false;
        }

        js::formatFile(noJsirOpt, context, *plain, false);
    }

    auto jsirPath = path + String(".jsir.expect");
    if(generate) {
        writeText(jsirPath, [&](Net::Writer& writer) {
            auto bytes = dumpWriter.getBuffered();
            writer.writeBytes(bytes.ptr, bytes.length);
        });
    }

    pass = compareText(jsirPath, dumpWriter.getBuffered()) && pass;

    auto jsPath = path + String(".jsir.js.expect");
    if(generate) {
        writeText(jsPath, [&](Net::Writer& writer) {
            auto bytes = optimized.getBuffered();
            writer.writeBytes(bytes.ptr, bytes.length);
        });
    }

    pass = compareText(jsPath, optimized.getBuffered()) && pass;

    if(!expectedRun || !nodeAvailable()) return pass;

    auto answered = executeJsMain(path, optimized.getBuffered());
    if(!answered || answered.unwrap() != expectedRun.unwrap()) {
        println("Fail (%@): the JSIR pipeline's JavaScript returned %@, expected %@.", path,
                answered ? answered.unwrap() : I64(-1), expectedRun.unwrap());
        return false;
    }

    auto plain = executeJsMain(path, noJsirOpt.getBuffered());
    if(!plain || plain.unwrap() != answered.unwrap()) {
        println("Fail (%@): the build with the JSIR optimizer off returned %@, and the one with it "
                "on returned %@.", path, plain ? plain.unwrap() : I64(-1), answered.unwrap());
        return false;
    }

    // And `-no-ir-opt`, which is a different program above the fork and therefore its own
    // compilation rather than a second emission of the one above. A fixture whose unoptimized
    // program is a target gap the optimizer removes carries `.jsir.errors.expect` beside its dump,
    // and that build is `runJsirGapTest`'s instead - see there.
    if(unoptimizedReports) return pass;

    Net::Writer unoptimized(16384);
    {
        FixtureFront<PrintDiagnostics> f(source, [](CompileSettings& settings) {
            jsSettings(settings);
            settings.optimizeIr = false;
        });

        auto& context = f.context;
        auto& diagnostics = f.diagnostics;

        ssa::Pipeline pipeline(context);
        if(!pipeline.resolve(f.ast, &f.provider) || !pipeline.convert()) {
            println("Fail (%@): the unoptimized JavaScript build produced %@ diagnostics.", path,
                    diagnostics.errorCount());
            return false;
        }

        pipeline.releaseMir();
        if(!pipeline.optimize()) {
            println("Fail (%@): the unoptimized JavaScript build did not verify.", path);
            return false;
        }

        auto file = js::genJsirProgram(context, *pipeline.program, pipeline.converted, nullptr);
        if(diagnostics.errorCount()) {
            println("Fail (%@): the unoptimized JSIR build produced %@ diagnostics.", path,
                    diagnostics.errorCount());
            return false;
        }

        js::formatFile(unoptimized, context, *file, false);
    }

    auto other = executeJsMain(path, unoptimized.getBuffered());
    if(!other || other.unwrap() != answered.unwrap()) {
        println("Fail (%@): the unoptimized JSIR build returned %@, and the optimized one returned %@.",
                path, other ? other.unwrap() : I64(-1), answered.unwrap());
        return false;
    }

    return pass;
}

/*
 * A fixture whose JavaScript answer is a **deliberate target gap**, pinned by its diagnostic.
 *
 * `.jsir.errors.expect` is `.errors.expect` one tier down: the program resolves, converts and
 * optimizes, and what JSIR has no form for is reported by name. Opted into by the file's existence
 * and filled by `generate`, on exactly the terms a rejection fixture is.
 *
 * It exists because the sweep is not a golden. `lib/Native`'s `allocateHeap`, a bitcast between
 * vectors of different lane widths and the nine `X86.*` rows are gaps this target is *supposed* to
 * have, and the only thing standing over them was a number in a progress record - so a gap that
 * silently became a different gap, or a wrong answer, changed nothing anybody would see. What this
 * pins is the sentence: it fails when the construct stops being reported, when it starts being
 * reported somewhere else, and when the sentence changes.
 *
 * Beside `.jsir.expect` it is the **unoptimized** build's gap (J165): the optimized program
 * converts and is held to its dump and its answer, and the `-no-ir-opt` one - which keeps a
 * construct the optimizer removed - is expected to report this sentence instead of being run.
 * `Instance.yana` is the case: an address stored into a pointer field that only the optimized
 * program forwards away. Alone, the file pins the optimized build, which then has no dump.
 */
static bool runJsirGapTest(const String& path, const String& errorPath, StringView source,
                           bool generate, bool optimized) {
    FixtureFront<RecordDiagnostics> f(source, [&](CompileSettings& settings) {
        jsSettings(settings);
        settings.optimizeIr = optimized;
    });
    auto& context = f.context;
    auto& diagnostics = f.diagnostics;

    ssa::Pipeline pipeline(context);
    auto reached = pipeline.resolve(f.ast, &f.provider) && pipeline.convert();
    if(reached) {
        pipeline.releaseMir();
        reached = pipeline.optimize();
    }

    // The tier itself, where the program got that far. A fixture whose gap is the *resolver's*
    // rather than this target's still records what it reported, which is what makes the file say
    // which of the two a gap is.
    if(reached) js::genJsirProgram(context, *pipeline.program, pipeline.converted, nullptr);

    if(generate) {
        writeText(errorPath, [&](Net::Writer& writer) {
            writer.writeString(StringView { diagnostics.recorded.pointer(),
                                            diagnostics.recorded.size() });
        });
    }

    auto pass = true;
    if(!diagnostics.errorCount()) {
        println("Fail (%@): expected the JavaScript pipeline to report, and it compiled the program.",
                path);
        pass = false;
    }

    return compareText(errorPath, ByteBuffer((Byte*)diagnostics.recorded.pointer(),
                                             diagnostics.recorded.size())) && pass;
}

static bool runTest(const String& path, StringView source, bool generate) {
    auto errorPath = path + String(".errors.expect");
    if(fileExists(errorPath)) return runRejectionTest(path, errorPath, source, generate);

    auto runPath = path + String(".run.expect");

    // The same source as a JavaScript program, where a golden says so - opted into separately from
    // everything else, so that a fixture may have one, the other or both.
    auto js = fileExists(path + String(".js.ssa.expect"));

    // And the JSIR pipeline, opted into separately from everything else - see runJsirPass. A
    // fixture whose JavaScript answer is a deliberate gap opts in the other way, with the sentence
    // this target reports rather than with a dump - see runJsirGapTest.
    auto jsir = fileExists(path + String(".jsir.expect"));
    auto jsirGapPath = path + String(".jsir.errors.expect");
    auto jsirGap = fileExists(jsirGapPath);

    /*
     * A fixture that is a JavaScript program and nothing else - `# platform: js`, see directives.h.
     *
     * The native pass is a machine: a native golden, a lowering, a run. A fixture whose subject is a
     * host operation has none of those, because the declarations that reach one exist on no other
     * target, so what it asserts is the JavaScript pass and that is all it runs.
     */
    auto javaScript = fixtureIsJavaScript(source);
    auto pass = javaScript || runNativePass(path, source, generate, readExpectedRun(runPath));

    if(javaScript || js || jsir) {
        pass = runJsirPass(path, source, generate, jsirExpectedRun(path, runPath), jsir,
                           jsir && jsirGap) && pass;
    }

    if(jsirGap) pass = runJsirGapTest(path, jsirGapPath, source, generate, !jsir) && pass;

    println("Running test \"%@\"... %@", path, pass ? "Pass."_v : "Fail."_v);
    return pass;
}

int main(int argc, const char** argv) {
    auto generate = false;
    U32 shard = 0;
    U32 shards = 1;
    U32 jobs = 0;
    auto sharded = false;

    /*
     * An argument that is neither `generate` nor a shard spec names the one fixture to run, by
     * prefix.
     *
     * For when the suite cannot get far enough to reach the fixture in question: an assertion or a
     * crash in an earlier one takes the whole run down, and "does *this* fixture pass" is then
     * unanswerable without it. Matched as a prefix of the file name so that
     * `YanaResolveTest Subscript` runs the whole family.
     *
     * Held as a `String` rather than a `StringView` of one: the view used to be taken of a temporary
     * built in the condition that tested it, and outlived the buffer it pointed into.
     */
    String only;

    for(int i = 1; i < argc; i++) {
        auto arg = String(argv[i]);
        if(arg == "generate") {
            generate = true;
            continue;
        }

        if(parseShard(arg, shard, shards)) { sharded = true; continue; }
        if(parseJobs(arg, jobs)) continue;
        if(rejectFlagArgument(arg)) return 1;
        only = arg;
    }

    /*
     * Sharding before anything else this driver does, because the parent's whole job is to fork and
     * wait - see `forkShards`. A child returns from it with its own shard set and carries on as an
     * ordinary serial run, so everything below is written once and knows nothing about it.
     *
     * One shard per core unless something said otherwise: `chooseJobs` takes `jobs:n` where it was
     * written, `1` where the caller already named a `shard:i/n` - which is `run-tests.sh`, whose
     * whole point is that it did the splitting - and one per core otherwise. `jobs:1` is how a run
     * under a debugger asks for a single process.
     */
    jobs = chooseJobs(jobs, sharded);
    if(jobs > 1) {
        auto status = forkShards(jobs, shard, shards);
        if(status >= 0) return status;
    }

    Array<String> tests;

    /*
     * `resolve/` and the directories under it, one level deep.
     *
     * The corpus outgrew one flat directory, so a family of fixtures that belong together can be
     * filed together - `resolve/ownership/`, `resolve/loans/` and the rest. One level rather than a
     * full walk, because a directory of fixtures is filing
     * and a tree of them would be a second corpus with its own rules.
     *
     * The other drivers over this corpus - `YanaElfTest`, `YanaBench` - list one directory and stop,
     * so what is in a subdirectory is theirs to opt into rather than something they have to exclude.
     */
    auto collect = [&](const String& directory) {
        listDirectory(directory, [&](const String& name, bool isDirectory) {
            if(isDirectory) return;
            auto dot = findLastChar(stringView(name), '.');
            if(!dot) return;
            if(String(dot + 1, name.text() + name.size() - dot - 1) != "yana") return;
            if(only.size() && !stringView(name).startsWith(stringView(only))) return;

            tests.push(directory + String("/") + name);
        });
    };

    Array<String> directories;
    directories.push(String("resolve"));

    listDirectory("resolve", [&](const String& name, bool isDirectory) {
        // `.` and `..` are directory entries like any other, and descending into the first is what
        // ran every fixture twice the day this was written.
        if(!isDirectory || name == "." || name == "..") return;

        auto path = String("resolve/") + name;

        // The one subdirectory that is not fixtures - see `kModuleDirectory`.
        if(path == String(kModuleDirectory)) return;

        directories.push(path);
    });

    for(auto& directory: directories) collect(directory);

    if(tests.isEmpty()) {
        // Only the first shard, since every one of them has the same listing at this point -
        // sharding is applied below. Without the guard a name prefix that matches nothing printed
        // once per core, which reads as a broken run rather than as a bad argument.
        if(!shard) println("no resolve tests found");
        return 1;
    }

    // See shard.h. Applied after the listing rather than inside it so that which fixture lands in
    // which shard depends only on the corpus, not on what else was filtered out first.
    if(shards > 1) {
        Array<String> mine;
        for(U32 i = 0; i < tests.size(); i++) {
            if(i % shards == shard) mine.push(tests[i]);
        }

        tests = ::move(mine);
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

#if defined(__unix__) || defined(__APPLE__)
    // Closing the pipe is what ends the harness' read loop. Waited for rather than left to the
    // process exit, so that a driver run does not outlive its own child.
    nodeHarness().stop();
#endif

    return pass ? 0 : 1;
}
