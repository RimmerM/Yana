// Standalone lower-IR test driver.
//
// The `lower` intermediate language is self-contained (parsing/printing/resolving its own
// text format) and doesn't depend on the parse/resolve compiler stages or on codegen/LLVM.
// This driver only exercises that stage - see compiler/lower/*.* and the YanaLower CMake
// target for why this is split out from the full compiler, the same way ParserTester.cpp
// is split from the full compiler for the parser stage.
#include <Core.h>
#include <File.h>
#include "../compiler/lower/lower_parser.h"
#include "../compiler/lower/lower_print.h"
#include "Net/Stream.h"
#include "Net/File.h"

using namespace Tritium;

struct TestProvider: SourceProvider {
    StringView source;

    StringView getSource(StringId module) override {
        return source;
    }

    const Location* getNode(LocationId id) override {
        return nullptr;
    }
};

// Prints the module and compares it against the file at comparePath.
// Returns false (and prints a diff) if the comparison fails or the file cannot be opened.
static bool compareAgainst(Context& context, LowerModule& module, const String& comparePath, PrintAnnotations annotations) {
    Net::Writer writer(16384);
    printModule(writer, context, *module.arena, module, annotations);
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

static void writeExpect(Context& context, LowerModule& module, const String& path, PrintAnnotations annotations) {
    try {
        Net::FileStream file;
        file.open(path, writeAccess(), File::CreateAlways);

        Net::Writer writer(Net::WriteStream(file), 16384);
        printModule(writer, context, *module.arena, module, annotations);
        writer.flush();
    } catch(const Net::Exception& e) {
        logError("Cannot create expect file \"%@\": %@", path, e.description);
    }
}

// One fixture. Answers whether it passed, which is what the driver's exit code is made of - a run
// that reports every failure in its output and then exits 0 is one nothing above it can act on.
bool lowerTest(const String& path, StringView content) {
    print("Running test \"%@\"...", path);

    TestProvider provider;
    provider.source = content;

    PrintDiagnostics diagnostics(provider);
    Context context(diagnostics);

    LowerModule module(1024 * 1024);
    LowerLexer lexer(context, diagnostics, content);
    LowerParser parser(context, module, lexer);

    if(!parser.parseModule()) {
        println("Failed to parse test file.");
        return false;
    }

    // Every test compares its plain (unannotated) printout against `<name>.expect` -
    // this is the baseline that exercises parsing, resolving and printing.
    auto expectPath = path + String(".expect");
    auto pass = compareAgainst(context, module, expectPath, PrintAnnotations {});

    // If a `<name>.live.expect` file also exists, additionally compare the liveness-annotated
    // printout against it. This exercises LowerFunction::buildLiveness() (see lower_analyze.cpp),
    // which the plain printout above never triggers.
    auto livePath = path + String(".live.expect");
    if(File::exists(livePath)) {
        pass = compareAgainst(context, module, livePath, PrintAnnotations { .liveness = true }) && pass;
    }

    // Likewise for `<name>.freq.expect` and LowerFunction::buildFrequencies(): the block
    // frequencies, which are what everything downstream weighs one part of a function by.
    auto freqPath = path + String(".freq.expect");
    if(File::exists(freqPath)) {
        pass = compareAgainst(context, module, freqPath, PrintAnnotations { .frequency = true }) && pass;
    }

    println(pass ? "Pass."_v : "Fail."_v);
    return pass;
}

bool generateLowerTest(const String& path, StringView content) {
    logInfo("Generating expect file for test \"%@\"", path);

    TestProvider provider;
    provider.source = content;

    PrintDiagnostics diagnostics(provider);
    Context context(diagnostics);

    LowerModule module(1024 * 1024);
    LowerLexer lexer(context, diagnostics, content);
    LowerParser parser(context, module, lexer);

    if(!parser.parseModule()) {
        println("Failed to parse test file.");
        return false;
    }

    auto expectPath = path + String(".expect");
    writeExpect(context, module, expectPath, PrintAnnotations {});
    println("Created expect file \"%@\".", expectPath);

    // Only (re-)generate an annotated expect file if one already exists for this test - opting a
    // test into liveness or frequency coverage is done by hand-creating the file once.
    auto livePath = path + String(".live.expect");
    if(File::exists(livePath)) {
        writeExpect(context, module, livePath, PrintAnnotations { .liveness = true });
        println("Created expect file \"%@\".", livePath);
    }

    auto freqPath = path + String(".freq.expect");
    if(File::exists(freqPath)) {
        writeExpect(context, module, freqPath, PrintAnnotations { .frequency = true });
        println("Created expect file \"%@\".", freqPath);
    }

    return true;
}

bool testLower(bool generate) {
    auto passed = true;
    Array<String> tests;

    listDirectory("lower", [&](const String& name, bool isDirectory) {
        if(!isDirectory && name != ".." && name != ".") {
            if(auto p = findLastChar(stringView(name), '.')) {
                String extension(p + 1, name.text() + name.size() - p - 1);
                if(extension == "lower") {
                    tests.push(String("lower/") + name);
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
            if(!generateLowerTest(test, { buffer.get(), size })) passed = false;
        } else {
            if(!lowerTest(test, { buffer.get(), size })) passed = false;
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

    return testLower(generateExpects) ? 0 : 1;
}
