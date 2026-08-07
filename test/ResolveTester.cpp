#include <Core.h>
#include <File.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include "../compiler/parse/parser.h"
#include "../compiler/resolve/analyze.h"
#include "../compiler/resolve/explain.h"
#include "../compiler/resolve/lower.h"
#include "../compiler/resolve/print.h"
#include "../compiler/repr/repr_print.h"
#include "../compiler/lower/lower_print.h"
#include "../compiler/lower/lower_validate.h"
#include "../compiler/codegen/x64/gen.h"
#include "../compiler/codegen/js/gen.h"
#include "../compiler/codegen/llvm/gen.h"
#include "Net/Stream.h"
#include "Net/File.h"
#include "shard.h"

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <spawn.h>
#include <csignal>

extern char** environ;
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
        transformFunction(context, base, *function, machine);

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
    assembly.applyDataRelocations((Byte*)memory);
    copy(assembly.buffer.buffer, (Byte*)memory, byteCount);

    // Writable as well as executable, because the code and the globals share one mapping here.
    // A real linker gives them separate sections with separate protection; this driver exists to
    // run a fixture, and splitting the buffer would mean teaching it what a section is.
    if(mprotect(memory, allocationSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        munmap(memory, allocationSize);
        return Nothing();
    }

    // How wide the result is, read off the entry's own signature rather than `main`'s - the two
    // agree by construction, since the entry answers what `main` answered, and asking the entry is
    // what keeps that a fact about one function instead of two.
    if(!resolved.entry) {
        munmap(memory, allocationSize);
        return Nothing();
    }

    auto returnType = (*resolved.types)[(*resolved.arena)[resolved.entry]->returnType];

    I64 result;
    auto address = (Byte*)memory + offset.unwrap();

    if(isUnit(*resolved.types, (*resolved.arena)[resolved.entry]->returnType)) {
        // A program that answers nothing exits zero, which is what the native wrapper says about the
        // same function (see addNativeEntry) and what C says about falling off the end of `main`.
        // Reading a register the callee never wrote would make a program with no `main` answer
        // whatever happened to be in it.
        ((void (*)())address)();
        result = 0;
    } else if(returnType->kind == Type::Int &&
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

/*
 * Runs the emitted JavaScript and compares what `main` answers with what the amd64 backend answered.
 *
 * The gap this closes is that nothing executed the JS at all: the backend was asserted by its own
 * golden text, so the two targets could - and repeatedly did - disagree about what a program *means*
 * with every fixture still green. Four such differences were found by hand before this existed, and
 * every one of them would have been a failing test here.
 *
 * The expected value is the one the native run is checked against, because agreeing with each other
 * is the property worth asserting; a fixture whose JS answer legitimately differs says so by putting
 * the other number in a `.js.run.expect` beside it, which today is `Platform.yana` and nothing else.
 * Opted into by `.js.expect` and `.run.expect` both existing, on the same terms as every other mode.
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

static bool runJsPass(const String& path, const String& jsPath, StringView source, bool generate,
                      bool forceGeneric, Maybe<I64> expectedRun, Maybe<I64>& ran) {
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
    auto pass = compareText(jsPath, writer.getBuffered());

    /*
     * Run it, whichever specialization mode the fixture asked for.
     *
     * `!forceGeneric` used to guard this, copied from the native side where it means "do not build a
     * second time" - `runGenericPass` is a *second* build of a fixture that is otherwise specialized,
     * and asserting the same number twice would be nothing. It means something else here, because
     * there is only ever one JS build and `forceGeneric` decides what it *is*: with the guard, a
     * `.generic` fixture's JavaScript was emitted, diffed and never executed.
     *
     * That is the half the second target exists for. `ErasedCopy.yana` is what found it - the
     * emitted file compared clean against its golden and answered 18 where both the fixture and the
     * native build say 25 - and `ErasedRelocate.yana` had been in the same position since it was
     * written.
     */
    if(expectedRun && nodeAvailable()) {
        auto actual = executeJsMain(path, writer.getBuffered());
        ran = actual;

        if(!actual || actual.unwrap() != expectedRun.unwrap()) {
            println("Fail (%@): JavaScript main returned %@, expected %@.", path,
                    actual ? actual.unwrap() : I64(-1), expectedRun.unwrap());
            pass = false;
        }
    }

    return pass;
}

/*
 * The same fixture, compiled with the IR optimizer switched off, on every target that runs.
 *
 * Design.md's rule for `Program::Specialization` applied to `compiler/opt`: an optimization may make
 * a program faster and may not make it different, so the honest test is compiling it both ways and
 * comparing what it does. Only the *result* is compared, for the same reason `runGenericPass`
 * compares only the result - the two forms legitimately produce different IR, and that they do is
 * the point.
 *
 * **Against the optimized build's own answers rather than against `.run.expect`.** That is what
 * makes this an assertion of its own instead of a second copy of the run check: the expectation
 * files are the compiler's output regenerated by `generate`, and the run number is a constant a
 * human worked out by hand and could equally have worked out by running the compiler. This compares
 * the compiler against itself, and there is no file either side of it.
 *
 * Both targets rather than one, because the same program is optimized once per target and a fold
 * that is right on native and wrong on JS is precisely the shape of bug this exists for. Every fold
 * in `compiler/opt` that declines a case declines it because the two targets would otherwise
 * disagree, and this is what notices if one of those judgements was wrong.
 *
 * The whole pipeline is re-run from the source rather than from the resolved program, because the
 * optimizer rewrites that program in place: there is no way to un-optimize one, and a second
 * resolution is the only honest way to get an unoptimized build of the same fixture.
 *
 * Nothing is reported for a target whose optimized build produced no answer - that failure has
 * already been reported by whoever ran it, and a second complaint about the same thing would only
 * make the first harder to find.
 */
static bool runUnoptimizedPass(const String& path, StringView source, bool forceGeneric,
                               Maybe<I64> optimizedNative, Maybe<I64> optimizedJs) {
    auto specialization = forceGeneric ? Program::Specialization::Generic
                                       : Program::Specialization::Always;

    auto resolveUnoptimized = [&](Context& context, TestProvider& provider, Diagnostics& diagnostics) {
        auto name = context.addUnqualifiedName("ResolveTest", 11);
        Lexer lexer(context, diagnostics, source, name);
        Parser parser(context, lexer, name);
        auto ast = parser.parseModule();
        return resolveProgram(context, ast, &provider, specialization);
    };

    auto pass = true;

    if(optimizedNative) {
        TestProvider provider;
        provider.source = source;
        PrintDiagnostics diagnostics(provider);
        Context context(diagnostics);
        context.settings.optimizeIr = false;
        provider.context = &context;

        auto module = resolveUnoptimized(context, provider, diagnostics);
        if(diagnostics.errorCount()) {
            println("Fail (%@): unoptimized resolution produced %@ diagnostics.", path,
                    diagnostics.errorCount());
            return false;
        }

        auto lowered = lowerProgram(context, *module);
        if(!validateLowerModule(&diagnostics, lowered.get())) {
            println("Fail (%@): unoptimized lowering produced invalid lower IR.", path);
            return false;
        }

        auto actual = executeMain(context, *module, *lowered);
        if(!actual || actual.unwrap() != optimizedNative.unwrap()) {
            println("Fail (%@): the optimizer changed the answer on amd64 - unoptimized main returned %@, optimized %@.",
                    path, actual ? actual.unwrap() : I64(-1), optimizedNative.unwrap());
            pass = false;
        }
    }

    // The JS half only where the optimized build ran it too: `optimizedJs` is set by `runJsPass`
    // exactly when it ran Node, so this needs no second copy of the rules about which fixtures do.
    if(!optimizedJs) return pass;

    {
        TestProvider provider;
        provider.source = source;
        PrintDiagnostics diagnostics(provider);
        Context context(diagnostics);
        context.settings.mode = CompileMode::JsExecutable;
        context.settings.optimizeIr = false;
        provider.context = &context;

        auto module = resolveUnoptimized(context, provider, diagnostics);
        if(diagnostics.errorCount()) {
            println("Fail (%@): unoptimized resolution for the JS target produced %@ diagnostics.",
                    path, diagnostics.errorCount());
            return false;
        }

        auto file = js::genProgram(context, *module);
        if(diagnostics.errorCount()) {
            println("Fail (%@): the unoptimized JS backend produced %@ diagnostics.", path,
                    diagnostics.errorCount());
            return false;
        }

        Net::Writer writer(16384);
        js::formatFile(writer, context, *file, false);

        auto actual = executeJsMain(path, writer.getBuffered());
        if(!actual || actual.unwrap() != optimizedJs.unwrap()) {
            println("Fail (%@): the optimizer changed the answer on JavaScript - unoptimized main returned %@, optimized %@.",
                    path, actual ? actual.unwrap() : I64(-1), optimizedJs.unwrap());
            pass = false;
        }
    }

    return pass;
}

/*
 * The same fixture, handed to LLVM.
 *
 * The lowered module rather than a second resolution, unlike the JS pass: this backend and the x64
 * one consume the same IR, so what it asserts is the translation into LLVM's form and nothing about
 * the stages in front of it. Opted into by the `.llvm.expect` file, on the same terms as the rest -
 * a fixture that says nothing new about the translation would only be a second copy of what
 * `.lower.expect` already asserts.
 *
 * The verifier runs whether or not the text matches, because a module LLVM rejects is a failure of
 * this backend even when the expect file was generated from it.
 */
static bool runLlvmPass(const String& path, const String& llvmPath, Context& context,
                        Diagnostics& diagnostics, LowerModule& lowered, bool generate) {
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

    /*
     * The `explain` query - Analysis-Ambient.md §7.3, opt-in on the same terms as the ownership dump.
     *
     * A different assertion from `.own.expect` even though it reads the same data: that file pins
     * what the passes *found*, and this one pins what a person is *told* - which rows appear, which
     * of them an editor calls surprising, and what a hover therefore contains. The second is the
     * whole point of the feature and none of it follows from the first.
     *
     * Before lowering, deliberately: the optimizer rewrites the program in place, and the answers
     * an editor shows are the ones the program had when it was written rather than after inlining
     * moved allocations around.
     */
    auto explainPath = path + String(".explain.expect");
    if(fileExists(explainPath)) {
        if(generate) {
            writeText(explainPath, [&](Net::Writer& writer) {
                printExplanations(writer, context, *module);
            });
        }

        Net::Writer explainWriter(16384);
        printExplanations(explainWriter, context, *module);
        pass = compareText(explainPath, explainWriter.getBuffered()) && pass;
    }

    /*
     * The layout this target chose, opt-in per fixture on the same terms as the ownership dump.
     *
     * This is the only assertion that can see a Repr decision nothing has consumed yet - a niche the
     * search found and the access lowering does not use yet is invisible in emitted code - so a
     * fixture about layout asserts this and a fixture about anything else does not generate it.
     */
    auto checkRepr = [&](const char* suffix, const ReprTarget& target) {
        auto reprPath = path + String(suffix);
        if(!fileExists(reprPath)) return;

        if(generate) {
            writeText(reprPath, [&](Net::Writer& writer) {
                printReprs(writer, context, *module, target);
            });
        }

        Net::Writer reprWriter(16384);
        printReprs(reprWriter, context, *module, target);
        pass = compareText(reprPath, reprWriter.getBuffered()) && pass;
    };

    /*
     * Both targets' answers for one source, from one resolved program.
     *
     * The two are separate files rather than one dump with two halves so that a fixture can assert
     * only the one it is about - but a fixture that touches packing or niches should have both,
     * because the interesting property is where they *differ*. `Maybe(Id)` being one word natively
     * and `number | null` on JS is the whole argument for computing layout at emission, and it is
     * only visible side by side.
     */
    checkRepr(".repr.expect", nativeReprTarget());
    checkRepr(".repr.js.expect", jsReprTarget());

    auto runPath = path + String(".run.expect");
    auto expectedRun = readExpectedRun(runPath);

    auto jsPath = path + String(".js.expect");

    // What the JS build actually answered, where it was run at all. Held out here because the
    // optimizer's equivalence check compares against it rather than against any file.
    Maybe<I64> jsRan;

    if(fileExists(jsPath)) {
        // A fixture whose two targets legitimately answer differently names the JS answer here.
        // `@platform` selecting different bodies is the one case, and it is a property of the
        // program rather than a difference of opinion between the backends.
        auto jsRun = readExpectedRun(path + String(".js.run.expect"));
        pass = runJsPass(path, jsPath, source, generate, forceGeneric,
                         jsRun ? jsRun : expectedRun, jsRan) && pass;
    }

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

    auto llvmPath = path + String(".llvm.expect");
    if(fileExists(llvmPath)) {
        pass = runLlvmPass(path, llvmPath, context, diagnostics, *lowered, generate) && pass;
    }

    /*
     * The resolve IR *after* compiler/opt has rewritten it, opt-in per fixture.
     *
     * `lowerProgram` calls `optimizeProgram`, which rewrites `*module` in place, so the same
     * printer that produced `.resolve.expect` above now prints the optimized form - the two files
     * side by side are the pass's before and after. Generated after lowering for exactly that
     * reason, and native rather than JS because this is the program lowering consumed.
     *
     * Opt-in because most fixtures are not about the optimizer: a fixture asserting a drop
     * placement would gain a second copy of its IR that moves whenever any pass changes, which is
     * the churn §7.5 of Analysis-Optimization.md warns about rather than an assertion.
     */
    auto optPath = path + String(".opt.expect");
    if(fileExists(optPath)) {
        if(generate) {
            writeText(optPath, [&](Net::Writer& writer) {
                printProgram(writer, context, *module);
            });
        }

        Net::Writer optWriter(16384);
        printProgram(optWriter, context, *module);
        pass = compareText(optPath, optWriter.getBuffered()) && pass;
    }

    Maybe<I64> nativeRan;

    if(auto expected = expectedRun) {
        auto actual = executeMain(context, *module, *lowered);
        nativeRan = actual;

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

        // The same program with compiler/opt switched off, on every target that ran, compared with
        // what the optimized build answered rather than with any file - see runUnoptimizedPass.
        pass = runUnoptimizedPass(path, source, forceGeneric, nativeRan, jsRan) && pass;
    }

    println("Running test \"%@\"... %@", path, pass ? "Pass."_v : "Fail."_v);
    return pass;
}

int main(int argc, const char** argv) {
    auto generate = false;
    U32 shard = 0;
    U32 shards = 1;

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

        if(parseShard(arg, shard, shards)) continue;
        only = arg;
    }

    Array<String> tests;

    listDirectory("resolve", [&](const String& name, bool directory) {
        if(directory) return;
        if(auto dot = findLastChar(stringView(name), '.')) {
            if(String(dot + 1, name.text() + name.size() - dot - 1) == "yana") {
                if(only.size() && !stringView(name).startsWith(stringView(only))) return;
                tests.push(String("resolve/") + name);
            }
        }
    });

    if(tests.isEmpty()) {
        println("no resolve tests found");
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
