#include <Core.h>
#include <File.h>
#include "../compiler/parse/parser.h"
#include "../compiler/parse/ast_print.h"
#include "../compiler/resolve/module.h"
#include "../compiler/resolve/print.h"
#include "../compiler/resolve/builtins.h"
#include "Net/Stream.h"
#include "Net/File.h"
#include "../compiler/codegen/x64/gen.h"

using namespace Tritium;

struct TestProvider: ModuleProvider, SourceProvider {
    StringView source;
    Context* context;
    Module* core = nullptr;
    Module* native = nullptr;

    Module* getModule(Module* from, StringId name) override {
        if(name == core->id) {
            return getCore();
        } else if(name == native->id) {
            return getNative();
        } else {
            return nullptr;
        }
    }

    StringView getSource(StringId module) override {
        return source;
    }

    const Location* getNode(LocationId id) override {
        return context->getLocation(id);
    }

    Module* getCore() {
        if(!core) core = coreModule(context);
        return core;
    }

    Module* getNative() {
        if(!native) native = nativeModule(context, getCore());
        return native;
    }
};

void parserTest(const String& path, StringView content) {
    print("Running test \"%@\"...", path);

    TestProvider provider;
    provider.source = content;

    PrintDiagnostics diagnostics(provider);
    Context context(diagnostics);
    provider.context = &context;

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
    provider.context = &context;

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

void resolverTest(const String& path, StringView content) {
    print("Running test \"%@\"...", path);

    TestProvider provider;
    provider.source = content;

    PrintDiagnostics diagnostics(provider);
    Context context(diagnostics);
    provider.context = &context;

    Lexer lexer(context, context.diagnostics, content);
    Parser parser(context, lexer, context.addUnqualifiedName("no_name", 7));

    auto ast = parser.parseModule();
    auto module = resolveModule(&context, &provider, &ast);

    Net::Writer writer(16384);
    printModule(writer, context, *module);
    auto string = writer.getBuffered();

    auto expectPath = path + String(".expect");
    auto result = File::openFile(expectPath, readAccess());
    if(result.isErr()) {
        println("cannot open file %@: error %@", expectPath, (U32)result.unwrapErr());
        return;
    }

    auto file = result.moveUnwrapOk();
    auto size = file.size();
    Ptr<char> buffer { (char*)hAlloc(size) };
    file.read({ (Byte*)buffer.get(), size });

    auto equal = size == string.length && compareMem(buffer.get(), string.ptr, size) == 0;
    if(equal) {
        println("Pass.");
    } else {
        println("Fail. Got:");
        print(StringView { (const char*)string.ptr, string.length });
        println("\n\n\nExpected:");
        print(StringView { buffer.get(), size });
        print("\n\n\n");
    }
}

void generateResolverTest(const String& path, StringView content) {
    println("Generating expect file for test \"%@\"", path);

    TestProvider provider;
    provider.source = content;

    PrintDiagnostics diagnostics(provider);
    Context context(diagnostics);
    provider.context = &context;

    Lexer lexer(context, context.diagnostics, content);
    Parser parser(context, lexer, context.addUnqualifiedName("no_name", 7));

    auto ast = parser.parseModule();
    auto module = resolveModule(&context, &provider, &ast);

    try {
        Net::FileStream file;
        file.open(path + ".expect", writeAccess(), File::CreateAlways);

        Net::Writer writer(Net::WriteStream(file), 16384);
        printModule(writer, context, *module);
    } catch(const Net::Exception& e) {
        logError("Cannot create expect file for \"%@\": %@", path, e.description);
    }
}

void testResolver(bool generate) {
    Array<String> tests;

    listDirectory("resolver", [&](const String& name, bool isDirectory) {
        if(!isDirectory && name != ".." && name != ".") {
            if(auto p = findLastChar(stringView(name), '.')) {
                String extension(p + 1, name.text() + name.size() - p - 1);
                if(extension == "yana") {
                    tests.push(String("resolver/") + name);
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
            generateResolverTest(test, { buffer.get(), size });
        } else {
            resolverTest(test, { buffer.get(), size });
        }
    }
}

void testLlvm(bool generate) {

}

void testJs(bool generate) {

}

int main(int argc, const char** argv) {
    bool generateExpects = false;
    bool parserTests = false;
    bool resolverTests = false;
    bool llvmTests = false;
    bool jsTests = false;

    for(int i = 1; i < argc; i++) {
        auto arg = String(argv[i]);
        if(arg == "generate") generateExpects = true;
        else if(arg == "parser") parserTests = true;
        else if(arg == "resolver") resolverTests = true;
        else if(arg == "llvm") llvmTests = true;
        else if(arg == "js") jsTests = true;
    }

    if(parserTests) testParser(generateExpects);
    if(resolverTests) testResolver(generateExpects);
    if(llvmTests) testLlvm(generateExpects);
    if(jsTests) testJs(generateExpects);
}