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
#include "settings.h"
#include "source.h"
#include <File.h>

Module* compileFile(Context& context, ModuleHandler& handler, const String& path, const Identifier& id) {
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

    ast::Module ast(context.addIdentifier(id));
    Parser parser(context, ast, text);
    parser.parseModule();

    return resolveModule(&context, &handler, &ast);
}

int main(int argc, const char** argv) {
    auto result = parseCommandLine(argv, argc);
    if(result.isErr()) {
        print("Argument error: ");
        println(stringBuffer(result.unwrapErr()));
        return 1;
    }

    auto settings = result.moveUnwrapOk();

    SourceMap sourceMap;
    auto sourceResult = buildSourceMap(sourceMap, settings);
    if(sourceResult.isErr()) {
        print("File error: ");
        println(stringBuffer(sourceResult.unwrapErr()));
        return 1;
    }

    for(auto& source: sourceMap.entries) {
        println("Found module %@ at location %@", String{source.id.text, source.id.textLength}, source.path);
    }

    PrintDiagnostics diagnostics;
    Context context(diagnostics);
    FileHandler handler(&context);

    Array<Module*> compiledModules;
    compiledModules.push(handler.core);

    for(auto& source: sourceMap.entries) {
        auto module = compileFile(context, handler, source.path, source.id);
        if(module) {
            compiledModules.push(module);
        }
    }

    auto outputDir = std::string(settings.outputDir.text(), settings.outputDir.size());

    switch(settings.mode) {
        case CompileMode::Library: {
            diagnostics.error("Library generation is not implemented yet."_buffer, nullptr, noSource);
            break;
        }
        case CompileMode::NativeExecutable: {
            llvm::LLVMContext llvmContext;
            Array<llvm::Module*> llvmModules;

            for(auto module: compiledModules) {
                llvmModules.push(genModule(&llvmContext, &context, module));
            }

            auto result = linkModules(&llvmContext, &context, llvmModules.pointer(), llvmModules.size());

            diagnostics.error("Native executable generation is not implemented yet."_buffer, nullptr, noSource);
            break;
        }
        case CompileMode::NativeShared: {
            diagnostics.error("Native shared library generation is not implemented yet."_buffer, nullptr, noSource);
            break;
        }
        case CompileMode::JsExecutable: {
            std::ofstream jsFile(outputDir + "/module.js", std::ios_base::out);
            for(auto module: compiledModules) {
                auto ast = js::genModule(&context, module);
                js::formatFile(context, jsFile, ast, false);
            }
            break;
        }
        case CompileMode::JsLibrary: {
            diagnostics.error("JS library generation is not implemented yet."_buffer, nullptr, noSource);
            break;
        }
        case CompileMode::Ir: {
            std::ofstream irFile(outputDir + "/module.ir", std::ios_base::out);

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

            break;
        }
        case CompileMode::Llvm: {
            std::ofstream llvmFile(outputDir + "/module.ll", std::ios_base::out);
            llvm::LLVMContext llvmContext;

            Array<llvm::Module*> llvmModules;
            for(auto module: compiledModules) {
                llvmModules.push(genModule(&llvmContext, &context, module));
            }

            auto result = linkModules(&llvmContext, &context, llvmModules.pointer(), llvmModules.size());
            llvm::raw_os_ostream stream{llvmFile};
            result->print(stream, nullptr);

            break;
        }
    }
}