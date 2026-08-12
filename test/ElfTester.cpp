// The source-to-executable test driver.
//
// Everything else that runs a fixture's `main` runs it *in* the test process: ResolveTester.cpp maps
// the assembled buffer and calls into it, which answers "does the code the backend generated compute
// the right thing" and nothing about the file the compiler writes. This driver answers the other
// half - it compiles each fixture through the same path `yana -mode exe -backend local` takes, writes
// a real ELF executable, hands it to the kernel, and compares what the process reported.
//
// The two are complementary and deliberately share their goldens: a fixture's `.run.expect` is what
// `main` answers, and it is the same number whichever of them asks. What this one can see that the
// other cannot is everything between the last byte generated and the first instruction executed -
// the segment layout, the entry point, the page the data landed on, the addresses written into
// constant data. Every one of those fails as a crash rather than as a wrong answer, which is why a
// signal is reported here as a distinct outcome rather than as a mismatched status.
//
// **The exit status is eight bits.** A process can report 0-255, and most fixtures answer something
// larger, so what is compared is the low byte of the expected value. That is a weaker assertion than
// the in-process driver's and is not meant to replace it: this one is about the file, and the corpus
// is large enough that a layout mistake shows up across it rather than in one fixture's low byte.
//
//   ./YanaElfTest                 # every fixture with a .run.expect
//   ./YanaElfTest Format          # the fixtures whose names start with Format
//   ./YanaElfTest shard:0/8       # one process' share of the corpus
#include <Core.h>
#include <File.h>
#include <cstdlib>
#include <cstring>
#include "../compiler/parse/parser.h"
#include "../compiler/resolve/lower.h"
#include "../compiler/lower/lower_validate.h"
#include "../compiler/codegen/x64/emit.h"
#include "shard.h"
#include "directives.h"

#if defined(__linux__) && defined(__x86_64__)
#include <sys/wait.h>
#include <unistd.h>
#include <spawn.h>
#include <fcntl.h>
#include <cerrno>
#include <csignal>

extern char** environ;
#define YANA_CAN_RUN_ELF 1
#endif

using namespace Tritium;

// Supplies both the source text a diagnostic quotes and the modules an `import` names, out of the
// same directory ResolveTester.cpp reads them from - the fixtures are shared, so the way their
// imports are found has to be too.
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
 * Where the executables go.
 *
 * Not the fixture directory: a driver that leaves binaries beside the sources makes `git status`
 * unreadable and makes two shards fight over one name. One directory per process, named by the pid,
 * and each fixture's executable inside it under its own name - so a failing run can be reproduced by
 * hand, and the name in a debugger is the fixture's.
 */
static String outputDirectory() {
    auto base = getenv("TMPDIR");
    auto root = String(base && *base ? base : "/tmp");

    char suffix[64];
#ifdef YANA_CAN_RUN_ELF
    auto length = format(toBuffer(suffix), String("/yana-elf-%@"), U64(getpid()));
#else
    auto length = format(toBuffer(suffix), String("/yana-elf-%@"), U64(0));
#endif

    return root + String(suffix, length);
}

static String fixtureName(const String& path) {
    auto slash = findLastChar(stringView(path), '/');
    auto start = slash ? slash + 1 : path.text();
    auto length = path.size() - (start - path.text());

    // Without the extension, so that `Format.yana` produces `Format` rather than a file whose name
    // says it is source.
    auto view = StringView { start, length };
    if(auto dot = findLastChar(view, '.')) length = dot - start - 1;

    return String(start, length);
}

#ifdef YANA_CAN_RUN_ELF
/*
 * What the process reported, or nothing if it did not get that far.
 *
 * `wasSignal` separates the two ways a run can fail. A wrong status is a wrong program; a signal is
 * a wrong *image* - a jump into padding, a write to a page that was mapped read-only, an entry point
 * that returned to nowhere - and those are the mistakes only this driver is positioned to catch, so
 * they are reported as themselves rather than folded into "returned the wrong number".
 *
 * Standard output goes to /dev/null: several fixtures print, and their text is not what is being
 * asserted here.
 */
static Maybe<int> runExecutable(const String& path, bool& wasSignal, int& signalNumber) {
    // Copied out and terminated, because a String is a counted pointer and everything below reads
    // until a zero byte.
    char terminated[4096];
    if(path.size() >= sizeof(terminated)) return Nothing();
    copy(path.text(), terminated, path.size());
    terminated[path.size()] = 0;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);

    const char* argv[] = { terminated, nullptr };

    pid_t child = -1;
    auto spawned = posix_spawn(&child, terminated, &actions, nullptr, (char* const*)argv, environ);
    posix_spawn_file_actions_destroy(&actions);

    if(spawned != 0) return Nothing();

    int status = 0;
    while(waitpid(child, &status, 0) < 0 && errno == EINTR) {}

    if(WIFSIGNALED(status)) {
        wasSignal = true;
        signalNumber = WTERMSIG(status);
        return Nothing();
    }

    if(!WIFEXITED(status)) return Nothing();
    return Just(WEXITSTATUS(status));
}
#endif

static bool runTest(const String& path, StringView source, const String& outputDir) {
    auto expected = readExpectedRun(path + String(".run.expect"));

    // A fixture with no expected result is not a failure: most of the corpus asserts IR rather than
    // behaviour, and this driver has nothing to say about those.
    if(!expected) return true;

    TestProvider provider;
    provider.source = source;
    PrintDiagnostics diagnostics(provider);
    Context context(diagnostics);
    applyExtensionDirective(context.settings, source);
    provider.context = &context;

    // The same settings a `-mode exe -backend local` build runs under. Stated rather than left at
    // the defaults, because `@platform` reads the mode during resolution: a fixture resolved for
    // one target and emitted for another is a different program.
    context.settings.mode = CompileMode::NativeExecutable;
    context.settings.backend = NativeBackend::Local;
    context.settings.format = ExecutableFormat::ELF;
    context.settings.target = TargetType::Linux;
    context.settings.arch = TargetArch::X64;

    auto name = context.addUnqualifiedName("ElfTest", 7);
    Lexer lexer(context, diagnostics, source, name);
    Parser parser(context, lexer, name);
    auto ast = parser.parseModule();

    auto program = resolveProgram(context, ast, &provider);
    if(!program || diagnostics.errorCount()) {
        println("Fail (%@): the fixture did not resolve.", path);
        return false;
    }

    auto lowered = lowerProgram(context, *program);
    if(diagnostics.errorCount() || !validateLowerModule(&diagnostics, lowered.get())) {
        println("Fail (%@): the fixture did not lower.", path);
        return false;
    }

    auto executable = outputDir + String("/") + fixtureName(path);
    if(!genX64Executable(context, *lowered, executable)) {
        println("Fail (%@): no executable was generated.", path);
        return false;
    }

#ifdef YANA_CAN_RUN_ELF
    // The low byte, because that is all a process can report - see the header comment.
    auto want = int(U64(expected.unwrap()) & 0xff);

    auto wasSignal = false;
    auto signalNumber = 0;
    auto actual = runExecutable(executable, wasSignal, signalNumber);

    if(wasSignal) {
        println("Fail (%@): %@ was killed by signal %@.", path, executable, signalNumber);
        return false;
    }

    if(!actual) {
        println("Fail (%@): %@ could not be run.", path, executable);
        return false;
    }

    if(actual.unwrap() != want) {
        println("Fail (%@): %@ exited with %@, expected %@ (the low byte of %@).",
                path, executable, actual.unwrap(), want, expected.unwrap());
        return false;
    }

    // Kept only when it failed, which is when there is something to look at: a passing run leaves
    // nothing behind, and every return above this one leaves the executable that produced the
    // failure where the message said it was.
    File::remove(executable);
#endif

    println("Running test \"%@\"... Pass.", path);
    return true;
}

int main(int argc, const char** argv) {
    U32 shard = 0;
    U32 shards = 1;

    // An argument that is not a shard spec names the fixtures to run, by prefix - the same
    // convention ResolveTester.cpp uses, and for the same reason: when the corpus cannot be got
    // through, "does *this* fixture pass" has to stay answerable.
    String only;

    for(int i = 1; i < argc; i++) {
        auto arg = String(argv[i]);
        if(parseShard(arg, shard, shards)) continue;

        // `generate` is accepted and ignored, so that a whole-suite regenerate can pass the same
        // arguments to every driver. There is nothing here to generate: the goldens this reads are
        // the resolve driver's.
        if(arg == "generate") continue;
        only = arg;
    }

#if !defined(__linux__) || !defined(__x86_64__)
    // Not a skip of the assertions but of the whole driver: the executables it writes are amd64
    // Linux ones, and there is nothing on another host that could run them.
    println("elf tests need amd64 Linux - nothing to run here");
    return 0;
#endif

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
        println("no elf tests found");
        return 1;
    }

    // See shard.h. Applied after the listing rather than inside it, so that which fixture lands in
    // which shard depends only on the corpus.
    if(shards > 1) {
        Array<String> mine;
        for(U32 i = 0; i < tests.size(); i++) {
            if(i % shards == shard) mine.push(tests[i]);
        }

        tests = ::move(mine);
    }

    auto outputDir = outputDirectory();
    auto created = createDirectory(outputDir);
    if(!created && created.unwrapErr() != FileError::Exists) {
        println("cannot create %@: error %@", outputDir, (U32)created.unwrapErr());
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
        pass = runTest(test, { source.get(), size }, outputDir) && pass;
    }

    return pass ? 0 : 1;
}
