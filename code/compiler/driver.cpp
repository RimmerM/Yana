
#include <cstdio>
#include <sys/stat.h>
#include <cstring>
#include <fstream>
#include "../util/types.h"
#include "../resolve/module.h"
#include "../parse/parser.h"
#include "../resolve/print.h"

bool isDirectory(const char* path) {
    struct stat type;
    stat(path, &type);
    return S_ISDIR(type.st_mode);
}

Module* compileFile(Context& context, const char* path, U32 rootPath) {
    char nameBuffer[2048];
    Size length = 0;
    Size segments = 1;
    auto name = path + rootPath;
    while(*name) {
        if(*name == '/' || *name == '\\') {
            segments++;
            nameBuffer[length] = '.';
        } else {
            nameBuffer[length] = *name;
        }

        length++;
        name++;
    }

    ast::Module ast(context.addQualifiedName(nameBuffer, length, segments));

    auto file = fopen(path, "r");
    if(!file) {
        context.diagnostics.error("cannot open file %s", nullptr, nullptr, path);
        return nullptr;
    }

    fseek(file, 0, SEEK_END);
    auto size = ftell(file);
    rewind(file);
    auto text = (char*)malloc(size + 1);
    fread(text, size, size, file);
    text[size] = 0;

    Parser parser(context, ast, text);
    parser.parseModule();

    return resolveModule(&context, nullptr, &ast);
}

int main(int argc, const char** argv) {
    if(argc < 3) {
        printf("Usage: compile <library root> <output file>\n");
        return 1;
    }

    const char* root = argv[1];
    const char* output = argv[2];

    PrintDiagnostics diagnostics;
    Context context(diagnostics);
    Array<Module*> compiledModules;

    if(isDirectory(root)) {
        // TODO:
    } else {
        auto module = compileFile(context, root, strlen(root));
        if(module) {
            compiledModules.push(module);
        }
    }

    std::ofstream irFile(output, std::ios_base::out);
    for(auto module: compiledModules) {
        irFile << "\nmodule ";
        irFile.write(module->name->text, module->name->textLength);
        irFile << '\n';
        printModule(irFile, context, module);
    }
}