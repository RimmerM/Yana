#include <fstream>
#include <sstream>
#include <Core.h>
#include <File.h>
#include "../compiler/parse/parser.h"
#include "../compiler/parse/ast_print.h"
#include "../compiler/resolve/module.h"
#include "../compiler/resolve/print.h"
#include "../compiler/resolve/builtins.h"

using namespace Tritium;

struct TestProvider: ModuleProvider, SourceProvider {
    StringBuffer source;
    Context* context;
    Module* core = nullptr;
    Module* native = nullptr;

    Module* getModule(Module* from, Id name) override {
        if(name == core->id) {
            return getCore();
        } else if(name == native->id) {
            return getNative();
        } else {
            return nullptr;
        }
    }

    StringBuffer getSource(Id module) override {
        return ""_buffer;
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

void parserTest(const String& path, StringBuffer content) {
    print("Running test \"%@\"...", path);

    TestProvider provider;
    provider.source = content;

    PrintDiagnostics diagnostics(provider);
    Context context(diagnostics);
    provider.context = &context;

    auto ast = new ast::Module(context.addUnqualifiedName("no_name", 7));
    Parser parser(context, *ast, content.ptr);
    parser.parseModule();

    std::stringstream stream;
    printModule(stream, context, *ast);
    auto string = stream.str();

    auto expectPath = path + String(".expect");
    auto result = File::open(expectPath, readAccess());
    if(result.isErr()) {
        println("cannot open file %@: error %@", expectPath, (U32)result.unwrapErr());
        return;
    }

    auto file = result.moveUnwrapOk();
    auto size = file.size();
    auto buffer = (char*)hAlloc(size);
    file.read({buffer, size});

    auto equal = size == string.length() && compareMem(buffer, string.begin().base(), size) == 0;
    if(equal) {
        println("Pass.");
    } else {
        println("Fail. Got:");
        print(string.c_str());
        println("\n\n\nExpected:");
        print(StringBuffer{buffer, size});
        print("\n\n\n");
    }
}

void generateParserTest(const String& path, StringBuffer content) {
    println("Generating expect file for test \"%@\"", path);

    TestProvider provider;
    provider.source = content;

    PrintDiagnostics diagnostics(provider);
    Context context(diagnostics);
    provider.context = &context;

    auto ast = new ast::Module(context.addUnqualifiedName("no_name", 7));
    Parser parser(context, *ast, content.ptr);
    parser.parseModule();

    auto expectPath = path + String(".expect");
    std::ofstream stream(std::string(expectPath.text(), expectPath.size()), std::ofstream::out | std::ofstream::trunc);
    if(!stream) {
        println("Cannot create expect file \"%@\"", expectPath);
        return;
    }

    printModule(stream, context, *ast);
}

void testParser(bool generate) {
    Array<String> tests;

    listDirectory("parser", [&](const String& name, bool isDirectory) {
        if(!isDirectory && name != ".." && name != ".") {
            if(auto p = findLastChar(stringBuffer(name), '.')) {
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
        auto result = File::open(test, readAccess());
        if(result.isErr()) {
            println("cannot open file %@: error %@", test, (U32)result.unwrapErr());
            continue;
        }

        auto file = result.moveUnwrapOk();
        auto size = file.size() + 1;
        auto buffer = (char*)hAlloc(size);
        file.read({buffer, size});
        buffer[size - 1] = 0;

        if(generate) {
            generateParserTest(test, {buffer, size});
        } else {
            parserTest(test, {buffer, size});
        }
    }
}

void resolverTest(const String& path, StringBuffer content) {
    print("Running test \"%@\"...", path);

    TestProvider provider;
    provider.source = content;

    PrintDiagnostics diagnostics(provider);
    Context context(diagnostics);
    provider.context = &context;

    auto ast = new ast::Module(context.addUnqualifiedName("no_name", 7));
    Parser parser(context, *ast, content.ptr);
    parser.parseModule();

    auto module = resolveModule(&context, &provider, ast);

    std::stringstream stream;
    printModule(stream, context, module);
    auto string = stream.str();

    auto expectPath = path + String(".expect");
    auto result = File::open(expectPath, readAccess());
    if(result.isErr()) {
        println("cannot open file %@: error %@", expectPath, (U32)result.unwrapErr());
        return;
    }

    auto file = result.moveUnwrapOk();
    auto size = file.size();
    auto buffer = (char*)hAlloc(size);
    file.read({buffer, size});

    auto equal = size == string.length() && compareMem(buffer, string.begin().base(), size) == 0;
    if(equal) {
        println("Pass.");
    } else {
        println("Fail. Got:");
        print(string.c_str());
        println("\n\n\nExpected:");
        print(StringBuffer{buffer, size});
        print("\n\n\n");
    }
}

void generateResolverTest(const String& path, StringBuffer content) {
    println("Generating expect file for test \"%@\"", path);

    TestProvider provider;
    provider.source = content;

    PrintDiagnostics diagnostics(provider);
    Context context(diagnostics);
    provider.context = &context;

    auto ast = new ast::Module(context.addUnqualifiedName("no_name", 7));
    Parser parser(context, *ast, content.ptr);
    parser.parseModule();

    auto module = resolveModule(&context, &provider, ast);

    auto expectPath = path + String(".expect");
    std::ofstream stream(std::string(expectPath.text(), expectPath.size()), std::ofstream::out | std::ofstream::trunc);
    if(!stream) {
        println("Cannot create expect file \"%@\"", expectPath);
        return;
    }

    printModule(stream, context, module);
}

void testResolver(bool generate) {
    Array<String> tests;

    listDirectory("resolver", [&](const String& name, bool isDirectory) {
        if(!isDirectory && name != ".." && name != ".") {
            if(auto p = findLastChar(stringBuffer(name), '.')) {
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
        auto result = File::open(test, readAccess());
        if(result.isErr()) {
            println("cannot open file %@: error %@", test, (U32)result.unwrapErr());
            continue;
        }

        auto file = result.moveUnwrapOk();
        auto size = file.size() + 1;
        auto buffer = (char*)hAlloc(size);
        file.read({buffer, size});
        buffer[size - 1] = 0;

        if(generate) {
            generateResolverTest(test, {buffer, size});
        } else {
            resolverTest(test, {buffer, size});
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