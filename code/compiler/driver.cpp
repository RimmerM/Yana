#define __STDC_CONSTANT_MACROS
#define __STDC_FORMAT_MACROS
#define __STDC_LIMIT_MACROS

#include <cstdio>
#include <sys/stat.h>
#include <cstring>
#include <fstream>
#include <llvm/IR/LLVMContext.h>
#include "../util/types.h"
#include "../resolve/module.h"
#include "../parse/parser.h"
#include "../resolve/print.h"
#include "../codegen/llvm/gen.h"
#include "../codegen/js/gen.h"
#include "../resolve/builtins.h"
#include <llvm/Support/raw_os_ostream.h>

struct FileHandler: ModuleHandler {
    Module* core;
    Module* native;

    FileHandler(Context* context) {
        core = coreModule(context);
        native = nativeModule(context, core);
    }

    virtual Module* require(Context* context, Module* from, Id name) override {
        if(name == core->id) {
            return core;
        } else if(name == native->id) {
            return native;
        } else {
            return nullptr;
        }
    }
};

bool isDirectory(const char* path) {
    struct stat type;
    stat(path, &type);
    return S_ISDIR(type.st_mode);
}

Module* compileFile(Context& context, ModuleHandler& handler, const char* path, U32 rootPath) {
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
    fread(text, size, 1, file);
    text[size] = 0;

    Parser parser(context, ast, text);
    parser.parseModule();

    return resolveModule(&context, &handler, &ast);
}

int main(int argc, const char** argv) {
    if(argc < 4) {
        printf("Usage: compile <mode> <library root> <output file/directory>\n");
        return 1;
    }

    const char* mode = argv[1];
    const char* root = argv[2];
    const char* output = argv[3];

    PrintDiagnostics diagnostics;
    Context context(diagnostics);
    FileHandler handler(&context);
    Array<Module*> compiledModules;

    compiledModules.push(handler.core);

    bool directory = isDirectory(root);
    if(directory) {
        // TODO:
    } else {
        auto module = compileFile(context, handler, root, strlen(root));
        if(module) {
            compiledModules.push(module);
        }
    }

    if(strcmp(mode, "exe") == 0) {
        llvm::LLVMContext llvmContext;
        Array<llvm::Module*> llvmModules;

        for(auto module: compiledModules) {
            llvmModules.push(genModule(&llvmContext, &context, module));
        }

        auto result = linkModules(&llvmContext, &context, llvmModules.pointer(), llvmModules.size());
    } else if(strcmp(mode, "js") == 0) {
        std::ofstream jsFile(output, std::ios_base::out);
        for(auto module: compiledModules) {
            auto ast = js::genModule(&context, module);
            js::formatFile(context, jsFile, ast, false);
        }
    } else if(strcmp(mode, "ir") == 0) {
        std::ofstream irFile(output, std::ios_base::out);

        for(auto module: compiledModules) {
            irFile << "module ";
            auto name = &context.find(module->id);

            if(name->textLength > 0) {
                irFile.write(name->text, name->textLength);
            } else {
                irFile << "<unnamed>";
            }

            irFile << "\n\n";
            printModule(irFile, context, module);
        }
    } else if(strcmp(mode, "lib") == 0) {
        // TODO: Generate library.
    } else if(strcmp(mode, "ll") == 0) {
        std::ofstream llvmFile(output, std::ios_base::out);
        llvm::LLVMContext llvmContext;

        Array<llvm::Module*> llvmModules;
        for(auto module: compiledModules) {
            llvmModules.push(genModule(&llvmContext, &context, module));
        }

        auto result = linkModules(&llvmContext, &context, llvmModules.pointer(), llvmModules.size());
        llvm::raw_os_ostream stream{llvmFile};
        result->print(stream, nullptr);
    } else {
        printf("Unknown compilation mode '%s'. Valid modes are exe, js, ir, lib, ll.", mode);
    }
}