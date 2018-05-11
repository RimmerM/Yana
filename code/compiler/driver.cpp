#define __STDC_CONSTANT_MACROS
#define __STDC_FORMAT_MACROS
#define __STDC_LIMIT_MACROS

#include <fstream>
#include "../resolve/module.h"
#include "../parse/parser.h"
#include "../resolve/print.h"
#include "../codegen/llvm/gen.h"
#include "../codegen/js/gen.h"
#include "../resolve/builtins.h"
#include <File.h>

struct FileHandler: ModuleHandler {
    Module* core;
    Module* native;

    FileHandler(Context* context) {
        core = coreModule(context);
        native = nativeModule(context, core);
    }

    Module* require(Context* context, Module* from, Id name) override {
        if(name == core->id) {
            return core;
        } else if(name == native->id) {
            return native;
        } else {
            return nullptr;
        }
    }
};

bool isDirectory(const String& path) {
    auto info = File::info(path);
    return info && info.unwrap().isDirectory;
}

Module* compileFile(Context& context, ModuleHandler& handler, const String& path, Size rootOffset) {
    char nameBuffer[2048];
    Size length = 0;
    Size segments = 1;
    auto name = path.text() + rootOffset;

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

    auto result = File::open(path, readAccess());
    if(result.isErr()) {
        context.diagnostics.error("cannot open file %@: error %@"_buffer, nullptr, noSource, path, (U32)result.unwrapErr());
        return nullptr;
    }

    auto file = result.moveUnwrapOk();
    auto size = file.size();
    auto text = (char*)hAlloc(size + 1);
    file.read({text, size});
    text[size] = 0;

    Parser parser(context, ast, text);
    parser.parseModule();

    return resolveModule(&context, &handler, &ast);
}

int main(int argc, const char** argv) {
    if(argc < 4) {
        println("Usage: compile <mode> <library root> <output file/directory>");
        return 1;
    }

    String mode(argv[1]);
    String root(argv[2]);
    String output(argv[3]);

    PrintDiagnostics diagnostics;
    Context context(diagnostics);
    FileHandler handler(&context);
    Array<Module*> compiledModules;

    compiledModules.push(handler.core);

    bool directory = isDirectory(root);
    if(directory) {
        // TODO:
    } else {
        auto module = compileFile(context, handler, root, root.size());
        if(module) {
            compiledModules.push(module);
        }
    }

    if(mode == "exe") {
        llvm::LLVMContext llvmContext;
        Array<llvm::Module*> llvmModules;

        for(auto module: compiledModules) {
            llvmModules.push(genModule(&llvmContext, &context, module));
        }

        auto result = linkModules(&llvmContext, &context, llvmModules.pointer(), llvmModules.size());
    } else if(mode == "js") {
        std::ofstream jsFile(std::string(output.text(), output.size()), std::ios_base::out);
        for(auto module: compiledModules) {
            auto ast = js::genModule(&context, module);
            js::formatFile(context, jsFile, ast, false);
        }
    } else if(mode == "ir") {
        std::ofstream irFile(std::string(output.text(), output.size()), std::ios_base::out);

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
    } else if(mode == "lib") {
        // TODO: Generate library.
    } else if(mode == "ll") {
        std::ofstream llvmFile(std::string(output.text(), output.size()), std::ios_base::out);
        llvm::LLVMContext llvmContext;

        Array<llvm::Module*> llvmModules;
        for(auto module: compiledModules) {
            llvmModules.push(genModule(&llvmContext, &context, module));
        }

        auto result = linkModules(&llvmContext, &context, llvmModules.pointer(), llvmModules.size());
        llvm::raw_os_ostream stream{llvmFile};
        result->print(stream, nullptr);
    } else {
        println("Unknown compilation mode '%@'. Valid modes are exe, js, ir, lib, ll.", mode);
    }
}