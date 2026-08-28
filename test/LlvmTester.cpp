// Standalone LLVM-codegen test driver.
//
// Parses a `.lower` file - the same self-contained lower-IR text format LowerTester.cpp and
// X64Tester.cpp read - builds an LLVM module out of it, runs LLVM's own verifier over the result,
// and compares the printed module against a golden `<name>.expect` file. `generate` rewrites the
// goldens, exactly as the other two drivers do.
//
// It exists for the same reason X64Tester.cpp does: a `.lower` file is a first-class way to write
// down IR that no source program produces yet. Every vector shape below stage 9 of
// Implementation-Vector.md is one of those, and this is where the LLVM half of them is asserted -
// `test/resolve`'s own `.llvm.expect` fixtures cover what a source program can reach and cannot
// reach a vector at all until the library lands.
//
// The verifier is not a formality here. Almost every mistake this backend can make about a vector -
// a lane type that does not match its vector, a mask where a full-width vector was wanted, a shuffle
// whose pattern is the wrong length - is a well-formedness error rather than a wrong answer, so a
// module that verifies is most of what there is to check.
#include <Core.h>
#include <File.h>
#include "shard.h"
#include "../compiler/lower/lower_parser.h"
#include "../compiler/lower/lower_print.h"
#include "../compiler/codegen/llvm/gen.h"
#include "Net/Stream.h"
#include "Net/File.h"
#include "directives.h"

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

// One fixture, up to the point where there is something to compare. Answers the printed module, or
// nothing if the backend or the verifier rejected it - which is a failure however the driver was
// invoked, since a golden regenerated from a module LLVM will not accept asserts nothing.
static bool buildModule(const String& path, StringView content, Net::Writer& writer) {
    TestProvider provider;
    provider.source = content;

    PrintDiagnostics diagnostics(provider);
    Context context(diagnostics);

    // The same `# extensions:` line X64Tester.cpp reads, and for a reason this backend has as well:
    // the level decides what a target *has*, and one operation here is written two entirely
    // different ways because of it - `pext` and `pdep` are an x86 intrinsic at v3 and a helper
    // function with a loop below it (see `hasBitPermute` in codegen/llvm/inst.cpp). A fixture that
    // could not say which level it wanted could only ever assert the second.
    applyFixtureDirectives(context.settings, content);

    LowerModule module(1024 * 1024);
    LowerLexer lexer(context, diagnostics, content);
    LowerParser parser(context, module, lexer);

    if(!parser.parseModule()) {
        println("Failed to parse test file \"%@\".", path);
        return false;
    }

    llvm::LLVMContext llvm;
    auto built = llvmgen::genModule(llvm, context, module);

    if(diagnostics.errorCount() > 0) {
        println("Fail (%@): the LLVM backend produced %@ diagnostics.", path, diagnostics.errorCount());
        return false;
    }

    if(!llvmgen::verifyGenModule(context, *built)) {
        println("Fail (%@): the LLVM backend produced a module the verifier rejects.", path);
        return false;
    }

    llvmgen::printModule(writer, *built);
    return true;
}

static bool compareAgainst(const String& comparePath, ByteBuffer string) {
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

// One fixture. Answers whether it passed, which is what the driver's exit code is made of - a run
// that reports every failure in its output and then exits 0 is one nothing above it can act on.
static bool llvmTest(const String& path, StringView content, bool generate) {
    if(generate) logInfo("Generating expect file for test \"%@\"", path);
    else print("Running test \"%@\"...", path);

    Net::Writer writer(16384);
    if(!buildModule(path, content, writer)) return false;

    auto expectPath = path + String(".expect");
    auto text = writer.getBuffered();

    if(generate) {
        try {
            Net::FileStream file;
            file.open(expectPath, writeAccess(), File::CreateAlways);

            Net::Writer out(Net::WriteStream(file), 16384);
            out.writeBytes(text.ptr, text.length);
            out.flush();
        } catch(const Net::Exception& e) {
            logError("Cannot create expect file \"%@\": %@", expectPath, e.description);
            return false;
        }

        println("Created expect file \"%@\".", expectPath);
        return true;
    }

    auto pass = compareAgainst(expectPath, text);
    println(pass ? "Pass."_v : "Fail."_v);
    return pass;
}

static bool testLlvm(bool generate) {
    auto passed = true;
    Array<String> tests;

    listDirectory("llvm", [&](const String& name, bool isDirectory) {
        if(!isDirectory && name != ".." && name != ".") {
            if(auto p = findLastChar(stringView(name), '.')) {
                String extension(p + 1, name.text() + name.size() - p - 1);
                if(extension == "lower") {
                    tests.push(String("llvm/") + name);
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

        if(!llvmTest(test, { buffer.get(), size }, generate)) passed = false;
    }

    return passed;
}

int main(int argc, const char** argv) {
    bool generateExpects = false;

    for(int i = 1; i < argc; i++) {
        auto arg = String(argv[i]);
        if(arg == "generate") generateExpects = true;
        else if(rejectFlagArgument(arg)) return 1;
    }

    return testLlvm(generateExpects) ? 0 : 1;
}
