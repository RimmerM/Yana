// Standalone parser test driver.
//
// The full Yana compiler (resolve/lower/codegen) is mid-migration and does not build
// end-to-end today; this driver only exercises the parser stage (lexer -> parser ->
// AST printer), which is self-contained and independently testable. See
// compiler/parse/{lexer,parser,ast,ast_print}.* and the YanaParse CMake target.
#include <Core.h>
#include <File.h>
#include "../compiler/parse/parser.h"
#include "../compiler/parse/ast_print.h"
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

void parserTest(const String& path, StringView content) {
    print("Running test \"%@\"...", path);

    TestProvider provider;
    provider.source = content;

    PrintDiagnostics diagnostics(provider);
    Context context(diagnostics);

    Lexer lexer(context, context.diagnostics, content);
    Parser parser(context, lexer, context.addUnqualifiedName("no_name", 7));
    auto ast = parser.parseModule();

    Net::Writer writer(16384);
    printModule(writer, context, *ast.region, ast);

    auto expectPath = path + String(".expect");
    auto file = tryResultOr(File::openFile(expectPath, readAccess()), {
        logError("cannot open file %@: error %@", expectPath, it.unwrapErr());
        return;
    });

    auto size = file.size();
    Ptr<char> buffer { (char*)hAlloc(size) };
    file.read({ (Byte*)buffer.get(), size });

    auto string = writer.getBuffered();
    auto equal = size == string.length && compareMem(buffer.get(), string.ptr, size) == 0;
    if(equal) {
        println("Pass.");
    } else {
        println("Fail. Got:");
        print(StringView { (char*)string.ptr, string.length });
        println("\n\n\nExpected:");
        print(StringView { buffer.get(), size });
        print("\n\n\n");
    }
}

void generateParserTest(const String& path, StringView content) {
    logInfo("Generating expect file for test \"%@\"", path);

    TestProvider provider;
    provider.source = content;

    PrintDiagnostics diagnostics(provider);
    Context context(diagnostics);

    Lexer lexer(context, context.diagnostics, content);
    Parser parser(context, lexer, context.addUnqualifiedName("no_name", 7));
    auto ast = parser.parseModule();

    try {
        Net::FileStream file;
        file.open(path + ".expect", writeAccess(), File::CreateAlways);

        Net::Writer writer(Net::WriteStream(file), 16384);
        printModule(writer, context, *ast.region, ast);
    } catch(const Net::Exception& e) {
        logError("Cannot create expect file for \"%@\": %@", path, e.description);
    }
}

void testParser(bool generate) {
    Array<String> tests;

    listDirectory("parser", [&](const String& name, bool isDirectory) {
        if(!isDirectory && name != ".." && name != ".") {
            if(auto p = findLastChar(stringView(name), '.')) {
                String extension(p + 1, name.text() + name.size() - p - 1);
                if(extension == "yana") {
                    tests.push(String("parser/") + name);
                }
            }
        }
    });

    if(tests.size() == 0) {
        println("no tests found");
    }

    for(auto& test: tests) {
        auto result = File::openFile(test, readAccess());
        if(result.isErr()) {
            println("cannot open file %@: error %@", test, (U32)result.unwrapErr());
            continue;
        }

        auto file = result.moveUnwrapOk();
        auto size = file.size();

        Ptr<char> buffer { (char*)hAlloc(size) };
        file.read({ (Byte*)buffer.get(), size });

        if(generate) {
            generateParserTest(test, { buffer.get(), size });
        } else {
            parserTest(test, { buffer.get(), size });
        }
    }
}

int main(int argc, const char** argv) {
    bool generateExpects = false;

    for(int i = 1; i < argc; i++) {
        auto arg = String(argv[i]);
        if(arg == "generate") generateExpects = true;
    }

    testParser(generateExpects);
}
