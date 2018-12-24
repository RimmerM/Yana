#include <fstream>
#include "../resolve/module.h"
#include "../parse/parser.h"
#include "../parse/ast_print.h"
#include "../resolve/print.h"
#include "../codegen/llvm/gen.h"
#include "../codegen/js/gen.h"
#include "settings.h"
#include "source.h"
#include <File.h>

static ast::Module* parseFile(Context& context, const String& path, const Identifier& id) {
    auto result = File::open(path, readAccess());
    if(result.isErr()) {
        context.diagnostics.error("cannot open file %@: error %@"_buffer, nullptr, path, (U32)result.unwrapErr());
        return nullptr;
    }

    auto file = result.moveUnwrapOk();
    auto size = file.size();
    auto text = (char*)hAlloc(size + 1);

    auto read = file.read({text, size});
    if(read.isErr()) {
        context.diagnostics.error("cannot read file %@: error %@"_buffer, nullptr, path, (U32)read.unwrapErr());
        return nullptr;
    }

    text[size] = 0;

    auto ast = new ast::Module(context.addIdentifier(id));
    Parser parser(context, *ast, text);
    parser.parseModule();

    return ast;
}

static Module* compileEntry(Context& context, ModuleProvider& provider, SourceEntry& entry) {
    if(entry.ir) return entry.ir;

    auto ast = entry.ast;
    if(!ast) {
        ast = parseFile(context, entry.path, entry.id);
        entry.ast = ast;
    }

    if(!ast || ast->errorCount > 0) return nullptr;

    auto module = resolveModule(&context, &provider, ast);
    entry.ir = module;
    return module;
}

static String generatePath(StringBuffer root, const Identifier& id, StringBuffer extension) {
    StringBuilder path(root.length + id.textLength + 1 + extension.length);
    path.append(root.ptr, root.length);

    for(Size i = 0; i < id.segmentCount; i++) {
        auto start = id.getSegmentOffset(i);
        path.append(id.text + start, id.getSegmentOffset(i + 1) - start);
        path.append("/");
    }

    path.append(".");
    path.append(extension.ptr, extension.length);

    return path.string();
}

static String replaceExtension(const String& path, const String& extension) {
    auto p = findLastChar(stringBuffer(path), '.');
    if(!p) return path + extension;

    p++;
    auto extensionLength = path.size() - (p - path.text());
    auto oldExtension = StringBuffer{p, extensionLength};
    if(findChar(oldExtension, '/')) return path + extension;

    auto length = path.size() - extensionLength + extension.size();
    auto buffer = (char*)hAlloc(length);
    copy(path.text(), buffer, path.size() - extensionLength);
    copy(extension.text(), buffer + path.size() - extensionLength, extension.size());

    return String(buffer, length);
}

static void astToFile(Context& context, ast::Module& module, const SourceEntry& entry) {
    auto path = replaceExtension(entry.path, "ast");
    auto file = std::ofstream();
    file.open(std::string(path.text(), path.size()));

    printModule(file, context, module);
}

static void irToFile(Context& context, Module& module, const SourceEntry& entry) {
    auto path = replaceExtension(entry.path, "ir");
    auto file = std::ofstream();
    file.open(std::string(path.text(), path.size()));

    printModule(file, context, &module);
}

int main(int argc, const char** argv) {
    // Parse the provided arguments into a settings structure.
    auto result = parseCommandLine(argv, argc);
    if(result.isErr()) {
        print("Argument error: ");
        println(stringBuffer(result.unwrapErr()));
        return 1;
    }

    auto settings = result.moveUnwrapOk();

    // Walk the input directory tree to create a map with each module we will compile.
    ModuleMap moduleMap;
    auto sourceResult = buildModuleMap(moduleMap, settings);
    if(sourceResult.isErr()) {
        print("File error: ");
        println(stringBuffer(sourceResult.unwrapErr()));
        return 1;
    }

    // Print a module listing if needed.
    if(settings.printModules) {
        for(auto& source: moduleMap.entries) {
            println("Found module %@ at location %@", String{source.id.text, source.id.textLength}, source.path);
        }
    }

    // Match any root modules to the source map.
    Array<SourceEntry*> roots(settings.rootObjects.size());
    for(auto& root: settings.rootObjects) {
        auto found = false;
        for(auto& entry: moduleMap.entries) {
            if(String(entry.id.text, entry.id.textLength) == root) {
                roots.push(&entry);
                found = true;
                break;
            }
        }

        if(!found) {
            println("Error: Cannot find root module %@", root);
        }
    }

    if(roots.size() != settings.rootObjects.size()) {
        return 1;
    }

    if(moduleMap.entries.size() == 0) {
        println("Error: No modules to compile found");
        return 1;
    }

    // Create compilation context.
    FileProvider provider(moduleMap);
    PrintDiagnostics diagnostics(provider);
    Context context(diagnostics);
    provider.context = &context;

    // Add standard library to the compilation set.
    Array<Module*> compiledModules;
    compiledModules.push(provider.core);

    auto onEntry = [&](SourceEntry& entry) {
        auto module = compileEntry(context, provider, entry);

        if(entry.ast && settings.printAst) {
            astToFile(context, *entry.ast, entry);
        }

        if(module && settings.printIr) {
            irToFile(context, *module, entry);
        }

        if(module) {
            compiledModules.push(module);
        }
    };

    // If any roots were provided, we initially parse only those. Any other modules are parsed on-demand.
    // Otherwise, simply parse every entry in the source map.
    if(roots.size() > 0) {
        for(auto root: roots) onEntry(*root);
    } else {
        for(auto& entry: moduleMap.entries) onEntry(entry);
    }

    // Stop compiling if any parse errors occurred.
    if(diagnostics.errorCount() > 0) {
        return 1;
    }

    auto outputDir = std::string(settings.outputDir.text(), settings.outputDir.size());
    auto dirResult = createDirectory(outputDir.c_str());
    if(!dirResult) {
        println("Cannot create directory: %@ to due error: ", outputDir.c_str(), (U32)dirResult.unwrapErr());
    }

    switch(settings.mode) {
        case CompileMode::Library: {
            diagnostics.error("Library generation is not implemented yet."_buffer, nullptr);
            break;
        }
        case CompileMode::NativeExecutable: {
            llvm::LLVMContext llvmContext;
            Array<llvm::Module*> llvmModules;

            for(auto module: compiledModules) {
                llvmModules.push(genModule(&llvmContext, &context, module));
            }

            auto result = linkModules(&llvmContext, &context, llvmModules.pointer(), llvmModules.size());

            diagnostics.error("Native executable generation is not implemented yet."_buffer, nullptr);
            break;
        }
        case CompileMode::NativeShared: {
            diagnostics.error("Native shared library generation is not implemented yet."_buffer, nullptr);
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
            diagnostics.error("JS library generation is not implemented yet."_buffer, nullptr);
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