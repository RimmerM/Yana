#include <Core.h>
#include <File.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include "../compiler/parse/parser.h"
#include "../compiler/resolve/analyze.h"
#include "../compiler/resolve/lower.h"
#include "../compiler/lower/lower_validate.h"
#include "../compiler/codegen/x64/gen.h"
#include "../compiler/codegen/js/gen.h"
#include "Net/Stream.h"
#include "Net/File.h"
#include "shard.h"
#include "directives.h"
#include "corpus.h"

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <spawn.h>
#include <csignal>

extern char** environ;
#endif

using namespace Tritium;

/*
 * The standard library's own suite - `lib/`, and what separates it from `resolve/`.
 *
 * A fixture here asserts what a library function *computes*. A fixture there asserts what the
 * compiler *decided* - which calls a literal desugars to, where a drop was placed, what a type's
 * layout is, which instruction an instance selected - and pins that decision in golden IR text.
 * The two corpora were one until this driver existed, and keeping them one cost the same thing
 * twice: close to 2MB of `.resolve.expect` and `.lower.expect` beside fourteen fixtures that say
 * nothing about the IR they print, regenerated in full whenever any pass changed anything.
 *
 * So there are no golden files here at all. What a fixture has is its source, and what this driver
 * asserts is:
 *
 *  - it resolves and lowers with no diagnostics, and the lower IR validates;
 *  - `main` runs to completion on amd64 and answers **0**;
 *  - the same, with specialization declined;
 *  - the same, with the IR optimizer switched off;
 *  - and, where the fixture opts in, the same on JavaScript.
 *
 * **Zero and not a number the fixture chose**, because a library fixture states its expectations
 * with `assert` rather than by encoding them in what it hands back. A sentinel return - `return
 * 0 - 7` for the seventh check - is what these fixtures used to do, and it made the corpus its own
 * decoder ring: the number in the golden file had to be worked out by adding up what every check
 * contributed, so adding a case anywhere renumbered the answer. An assertion stops the program
 * where it failed instead, and the run either completed or it did not.
 *
 * That is also why `main` is run in a **forked child**. `assert` is `checkCondition`, which is
 * `checkFailed`, which is `abortProcess` - so a failed assertion inside a driver that JITs and calls
 * the code would take the driver down with it, and a driver that dies is exactly what test/README
 * says the machine's own instability looks like. A test failure must not be indistinguishable from
 * that. The child runs the program and reports through its exit status; the parent survives to say
 * which fixture stopped and to run the next one.
 *
 * There is no `.run.expect` here and no `.js.run.expect`. A fixture that wants to check a number
 * asserts it, in the source, beside the reason.
 */

// Supplies the source text a diagnostic quotes, and the modules an `import` names. A library
// fixture imports library modules - `Math`, `File`, `Native` - which the compiler finds in `lib/`
// on its own; this answers `lib/modules/<Name>.yana` for the same reason the resolve driver answers
// `resolve/modules/`, so a fixture that needs a second module of its own can have one.
struct TestProvider: SourceProvider, ModuleProvider {
    struct Loaded {
        StringId name;
        Ptr<char, HeapDeleter> text;
        Size length;
        ast::Module* ast;
        ast::ModuleGroup* group;
    };

    StringView source;
    Context* context = nullptr;
    Array<Loaded> loaded;

    ~TestProvider() override {
        for(auto& entry: loaded) {
            delete entry.group;
            delete entry.ast;
        }
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

    ast::ModuleGroup* getModule(StringId name) override {
        for(auto& entry: loaded) {
            if(entry.name == name) return entry.group;
        }

        // `OpenExisting`, which is not a detail: the default mode *creates* the file, so an import
        // this directory does not answer would leave an empty module behind - and an empty module
        // shadows the library one of that name on every run afterwards, since the project's own
        // files are looked at first. That is precisely what a fixture importing `File` or `Math`
        // would hit here, where every import is a library module.
        auto path = String("lib/modules/") + context->findName(name) + String(".yana");
        auto opened = File::openFile(path, readAccess(), File::OpenExisting);
        if(opened.isErr()) return nullptr;

        auto file = opened.moveUnwrapOk();
        auto size = file.size();
        Ptr<char, HeapDeleter> text { (char*)hAlloc(size) };
        file.read({ (Byte*)text.get(), size });

        Lexer lexer(*context, context->diagnostics, StringView { text.get(), size }, name);
        Parser parser(*context, lexer, name);
        auto ast = new ast::Module(parser.parseModule());

        // A group of one file. These fixtures name a module per file, which is what a file that
        // wrote `module` would be under the directory rule too - Analysis-Modules.md §2.1.
        auto group = new ast::ModuleGroup { .name = name };
        group->files.push(ast);

        loaded.push(Loaded { name, ::move(text), size, ast, group });
        return group;
    }
};

static bool fileExists(const String& path) {
    auto info = File::info(path);
    return info && !info.unwrapOk().isDirectory;
}

/*
 * What one run of a fixture's `main` did.
 *
 * Three outcomes rather than an optional number, because they are three different reports and the
 * middle one is the whole reason this driver forks. `Answered` is a program that ran; `Stopped` is
 * an assertion that failed, which is the ordinary way a library fixture fails and says so with the
 * status a `SIGABRT` reports; `Broken` is this driver not managing to run it at all, which is a
 * problem with the driver or the machine rather than with the fixture.
 */
struct RunOutcome {
    enum Kind { Answered, Stopped, Broken } kind = Broken;
    I64 value = 0;

    // How it stopped, which the two targets say differently: a status or a signal natively, and a
    // thrown value on JavaScript, where there is no exit status a script can set.
    int status = 0;
    bool signalled = false;
    bool threw = false;
};

/*
 * Generates the module, maps it, and calls the program's entry in a child process.
 *
 * Everything above the fork is the resolve driver's `executeMain` - one allocator scratch and one
 * machine function across the whole module, globals after the code, data relocations applied to the
 * mapping rather than to the buffer. What is different is the last step and why.
 *
 * `fork` and not `posix_spawn`: what is being run is JIT-mapped code in *this* address space, and
 * there is no file to spawn. The child inherits the mapping already made, calls it, writes the
 * answer back over a pipe and `_exit`s - `_exit` and not `exit`, so it runs none of the parent's
 * atexit handlers and flushes none of the parent's buffers a second time.
 *
 * The pipe carries the answer because an exit status cannot: it is eight bits, and a library
 * fixture's answer is an `I64`. The status is what says whether the answer is there to be believed.
 */
static RunOutcome executeMainIsolated(Context& context, Program& resolved, LowerModule& module) {
    RunOutcome outcome;

#if defined(__x86_64__) && (defined(__unix__) || defined(__APPLE__))
    auto base = *module.arena;
    AsmModule assembly;

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

    for(auto globalPointer: module.globalOrder) assembly.addGlobal(base, base[globalPointer]);
    assembly.resolveRelocations(module.imageAnchor ? base[module.imageAnchor] : nullptr);

    // The program's start rather than `main` by name - see Program::entry. A library fixture has no
    // top-level statements today, but the entry is still what answers, and asking for it keeps that
    // a fact about one function rather than two.
    if(!module.entry || !resolved.entry) return outcome;

    auto foundMain = module.functions.get(module.entry);
    if(!foundMain) return outcome;
    auto mainFunction = base[foundMain.unwrap()];
    auto offset = assembly.functionOffsets.getValue(mainFunction);
    if(!offset) return outcome;

    auto byteCount = assembly.buffer.offset();
    auto page = Size(sysconf(_SC_PAGESIZE));
    auto allocationSize = (byteCount + page - 1) & ~(page - 1);
    auto memory = mmap(nullptr, allocationSize, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if(memory == MAP_FAILED) return outcome;

    assembly.applyDataRelocations((Byte*)memory);
    copy(assembly.buffer.buffer, (Byte*)memory, byteCount);

    if(mprotect(memory, allocationSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        munmap(memory, allocationSize);
        return outcome;
    }

    auto returnType = (*resolved.types)[(*resolved.arena)[resolved.entry]->returnType];
    auto unitResult = isUnit(*resolved.types, (*resolved.arena)[resolved.entry]->returnType);
    auto wide = returnType->kind == Type::Int && ((IntType*)returnType)->width == IntType::Long;
    auto address = (Byte*)memory + offset.unwrap();

    int answer[2];
    if(pipe(answer) != 0) {
        munmap(memory, allocationSize);
        return outcome;
    }

    // Anything this process has buffered is flushed before the fork rather than after it: a child
    // that inherits a half-full stdout buffer prints the parent's last line again on its way out.
    fflush(nullptr);

    auto child = fork();
    if(child < 0) {
        close(answer[0]);
        close(answer[1]);
        munmap(memory, allocationSize);
        return outcome;
    }

    if(child == 0) {
        close(answer[0]);

        I64 result;
        if(unitResult) {
            // A program that answers nothing exits zero, which is what the native wrapper says
            // about the same function and what C says about falling off the end of `main`. Reading
            // a register the callee never wrote would make it answer whatever was in it.
            ((void (*)())address)();
            result = 0;
        } else if(wide) {
            result = ((I64 (*)())address)();
        } else {
            result = ((I32 (*)())address)();
        }

        auto bytes = (const char*)&result;
        Size at = 0;
        while(at < sizeof(result)) {
            auto wrote = ::write(answer[1], bytes + at, sizeof(result) - at);
            if(wrote <= 0) break;
            at += Size(wrote);
        }

        _exit(0);
    }

    close(answer[1]);

    I64 result = 0;
    Size at = 0;
    while(at < sizeof(result)) {
        auto got = ::read(answer[0], (char*)&result + at, sizeof(result) - at);
        if(got <= 0) break;
        at += Size(got);
    }

    close(answer[0]);

    int status = 0;
    while(waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    munmap(memory, allocationSize);

    if(WIFSIGNALED(status)) {
        outcome.kind = RunOutcome::Stopped;
        outcome.status = WTERMSIG(status);
        outcome.signalled = true;
        return outcome;
    }

    if(!WIFEXITED(status)) return outcome;

    // A child that exited non-zero stopped before it wrote anything, which is what `checkFailed`
    // does: `exitProcess(134)`. A child that exited zero and wrote nothing is this driver's fault
    // rather than the fixture's, and stays `Broken`.
    if(WEXITSTATUS(status) != 0) {
        outcome.kind = RunOutcome::Stopped;
        outcome.status = WEXITSTATUS(status);
        return outcome;
    }

    if(at != sizeof(result)) return outcome;

    outcome.kind = RunOutcome::Answered;
    outcome.value = result;
    return outcome;
#else
    (void)context;
    (void)resolved;
    (void)module;
    return outcome;
#endif
}

// The one sentence a stopped or broken run is reported with, so that the four places that run a
// fixture say the same thing about the same outcome. `which` names the build - there are three or
// five of them per fixture and only the message says which one stopped.
static void reportOutcome(const String& path, StringView which, const RunOutcome& outcome) {
    switch(outcome.kind) {
        case RunOutcome::Stopped:
            if(outcome.threw) {
                // `hostFail` is `throw`, which is the most a script can do - see lib/Host.yana. The
                // sentence it carried has already been printed beside this.
                println("Fail (%@): the %@ build threw - a check or an assertion failed.", path, which);
            } else if(outcome.signalled) {
                println("Fail (%@): the %@ build was killed by signal %@.", path, which,
                        outcome.status);
            } else {
                // 134 is `abortProcess`, which is what a failed `assert` and a failed bounds check
                // both reach. Named as such, because "exited with 134" is the one status in this
                // driver that means the fixture worked exactly as designed and the library did not.
                println("Fail (%@): the %@ build stopped with status %@%@.", path, which,
                        outcome.status,
                        outcome.status == 134 ? " - a check or an assertion failed"_v : ""_v);
            }
            break;

        case RunOutcome::Broken:
            println("Fail (%@): the %@ build could not be run.", path, which);
            break;

        case RunOutcome::Answered:
            println("Fail (%@): the %@ build's main answered %@, expected 0.", path,
                    which, outcome.value);
            break;
    }
}

static bool nodeAvailable() {
    static int cached = -1;
    if(cached < 0) {
        cached = system("node --version > /dev/null 2>&1") == 0;
        if(!cached) println("Note: `node` is not on PATH, so the JavaScript half is not being run.");
    }

    return cached != 0;
}

#if defined(__unix__) || defined(__APPLE__)

/*
 * One Node process for the whole run, spoken to over a pair of pipes - the resolve driver's harness,
 * for the reason it gives there: Node starts in about eleven milliseconds, and a process per script
 * was the whole cost of the JavaScript half.
 *
 * Each script is still evaluated with `vm.runInNewContext` by `node-harness.js`, so the *program* is
 * never shared even though the process is. A script that throws - which is what `hostFail` does, and
 * therefore what a failed assertion does here - is reported in the answer rather than taking the
 * harness down; a crash that does take it down restarts it on the next fixture.
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

    bool run(ByteBuffer emitted, char* buffer, Size capacity, Size& read, bool& threw) {
        read = 0;
        buffer[0] = 0;
        threw = false;

        if(child < 0 && !start()) return false;

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

// The JavaScript side of `executeMainIsolated`. No fork here: the harness already runs the program
// in a context of its own, and a script that throws is an answer rather than a death.
static RunOutcome executeJsMain(ByteBuffer emitted) {
    RunOutcome outcome;
    char buffer[512] = {};
    Size read = 0;
    auto ok = false;
    auto threw = false;

#if defined(__unix__) || defined(__APPLE__)
    // A test driver has no use for the default: a harness that has died turns every later write
    // into a signal that kills the driver rather than a failure it can report.
    static auto ignored = signal(SIGPIPE, SIG_IGN);
    (void)ignored;

    ok = nodeHarness().run(emitted, buffer, sizeof(buffer), read, threw);
#endif

    if(!ok) {
        // A throw is the fixture stopping - `hostFail` is `throw`, so this is where a failed
        // assertion lands on this target. Anything else is the harness, and is Broken.
        outcome.kind = threw ? RunOutcome::Stopped : RunOutcome::Broken;
        outcome.threw = threw;
        if(threw) println("  the script threw: %@", StringView(buffer, U32(read)));
        return outcome;
    }

    /*
     * The *last* line, because a fixture is allowed to print. The status is the final line by
     * construction - the harness writes the script's completion value there, which is what the entry
     * call the emitted file ends with produced - so anything before it is the program's own output.
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
    if(end == start) return outcome;

    outcome.kind = RunOutcome::Answered;
    outcome.value = I64(value);
    return outcome;
}

/*
 * One build of one fixture, and the only assertion this driver makes about it.
 *
 * Three of these run per fixture, and two more where it opts into JavaScript. They differ only in
 * two switches, which is the point. A
 * library function's answer may not depend on whether the call site was specialized, on whether the
 * optimizer ran, or on which of the two backends is underneath it, and each of those is a build
 * here rather than a golden file. `which` is what tells the four apart in a failure.
 */
struct BuildOptions {
    StringView which;
    bool generic = false;
    bool optimize = true;
    bool js = false;
};

static bool runBuild(const String& path, StringView source, const BuildOptions& options) {
    TestProvider provider;
    provider.source = source;
    PrintDiagnostics diagnostics(provider);
    Context context(diagnostics);
    applyFixtureDirectives(context.settings, source);
    context.settings.optimizeIr = options.optimize;
    if(options.js) context.settings.mode = CompileMode::JsExecutable;
    provider.context = &context;

    auto name = context.addUnqualifiedName("LibTest", 7);
    Lexer lexer(context, diagnostics, source, name);
    Parser parser(context, lexer, name);
    auto ast = parser.parseModule();
    auto module = resolveProgram(context, ast, &provider,
                                 options.generic ? Program::Specialization::Generic
                                                 : Program::Specialization::Always);

    if(!module || diagnostics.errorCount()) {
        println("Fail (%@): the %@ build produced %@ diagnostics.", path, options.which,
                diagnostics.errorCount());
        return false;
    }

    RunOutcome outcome;

    if(options.js) {
        // A second resolution rather than a second walk of the first one, because `@platform`
        // selects which declarations *exist*: a JS build and a native build do not share a resolved
        // program. That is what the `js` switch above bought, and it is why this branch does not
        // lower - the JavaScript backend consumes the resolved program directly.
        auto file = js::genProgram(context, *module);
        if(diagnostics.errorCount()) {
            println("Fail (%@): the %@ backend produced %@ diagnostics.", path,
                    options.which, diagnostics.errorCount());
            return false;
        }

        Net::Writer writer(16384);
        js::formatFile(writer, context, *file, false);
        outcome = executeJsMain(writer.getBuffered());
    } else {
        auto lowered = lowerProgram(context, *module);
        if(!validateLowerModule(&diagnostics, lowered.get())) {
            println("Fail (%@): the %@ build produced invalid lower IR.", path,
                    options.which);
            return false;
        }

        outcome = executeMainIsolated(context, *module, *lowered);
    }

    if(outcome.kind == RunOutcome::Answered && outcome.value == 0) return true;

    reportOutcome(path, options.which, outcome);
    return false;
}

static bool runTest(const String& path, StringView source) {
    /*
     * The four builds, and what each of them is for.
     *
     * `amd64` is the fixture. `generic` is the same program with every concrete generic call site
     * forced through the erased ABI rather than a specialization, which is the most direct guard
     * against a semantic decision quietly living in the specializer - and a library is where that
     * matters most, since almost everything in `lib/` is generic. `unoptimized` is the same program
     * with `compiler/opt` switched off: an optimization may make a program faster and may not make
     * it different.
     *
     * They are separate builds rather than one build asserted three ways because there is no way to
     * un-optimize a resolved program - `lowerProgram` rewrites it in place - and no way to
     * un-specialize one either.
     */
    auto pass = runBuild(path, source, { "amd64"_v });
    pass = runBuild(path, source, { "generic"_v, true, true, false }) && pass;
    pass = runBuild(path, source, { "unoptimized"_v, false, false, false }) && pass;

    /*
     * And JavaScript, where the fixture opts in.
     *
     * Opted into by a marker file - `<fixture>.yana.js`, empty - rather than by anything in the
     * source, on the same terms every mode in the resolve driver is. Most of this corpus runs on
     * both targets and that is half of what it is for: `Show` on JS is the host's `toExponential`
     * and natively it is Ryu over a table of powers of five, so the two are different programs that
     * have to produce the same characters. The ones that do not opt in are the ones written over
     * `Native` - a JavaScript build has no descriptors and no bump allocator to count.
     *
     * The unoptimized JS build is run too, for the reason the native one is: every fold in
     * `compiler/opt` that declines a case declines it because the two targets would otherwise
     * disagree, and this is what notices if one of those judgements was wrong.
     */
    if(fileExists(path + String(".js")) && nodeAvailable()) {
        pass = runBuild(path, source, { "js"_v, false, true, true }) && pass;
        pass = runBuild(path, source, { "js-unoptimized"_v, false, false, true }) && pass;
    }

    println("Running test \"%@\"... %@", path, pass ? "Pass."_v : "Fail."_v);
    return pass;
}

int main(int argc, const char** argv) {
    U32 shard = 0;
    U32 shards = 1;

    // An argument that is not a shard spec names the fixtures to run, by prefix - the same
    // convention every driver here uses, and for the same reason: when the corpus cannot be got
    // through, "does *this* fixture pass" has to stay answerable.
    String only;

    for(int i = 1; i < argc; i++) {
        auto arg = String(argv[i]);
        if(parseShard(arg, shard, shards)) continue;

        // `generate` is accepted and ignored, so that a whole-suite regenerate can pass the same
        // arguments to every driver. There is nothing here to generate, and that is the design:
        // an expectation this driver could rewrite into agreeing with itself is one it is not
        // asserting.
        if(arg == "generate") continue;
        only = arg;
    }

    // `lib/` here has to be the fixture corpus and not the standard library of the same name - see
    // corpus.h, and the empty-directory guard below, which cannot catch that case.
    if(reportWrongDirectory()) return 1;

    Array<String> tests;

    listDirectory("lib", [&](const String& name, bool directory) {
        if(directory) return;
        if(auto dot = findLastChar(stringView(name), '.')) {
            if(String(dot + 1, name.text() + name.size() - dot - 1) == "yana") {
                if(only.size() && !stringView(name).startsWith(stringView(only))) return;
                tests.push(String("lib/") + name);
            }
        }
    });

    // Empty is a failure and not a quiet pass: a driver that verified nothing must not be mistaken
    // for one that verified everything, and running from anywhere but `test/` looks exactly like
    // this. See the note in test/README.md about the twelve x64 goldens.
    if(tests.isEmpty()) {
        println("no library tests found");
        return 1;
    }

    // See shard.h. Applied after the listing rather than inside it, so that which fixture lands in
    // which shard depends only on the corpus and not on what else was filtered out first.
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
        pass = runTest(test, { source.get(), size }) && pass;
    }

#if defined(__unix__) || defined(__APPLE__)
    // Closing the pipe is what ends the harness' read loop. Waited for rather than left to the
    // process exit, so that a driver run does not outlive its own child.
    nodeHarness().stop();
#endif

    return pass ? 0 : 1;
}
