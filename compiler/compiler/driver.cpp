#include "../parse/parser.h"
#include "../parse/ast_print.h"
#include "../resolve/module.h"
#include "../resolve/print.h"
#include "../resolve/explain.h"
#include "../resolve/lower.h"
#include "../lower/lower_print.h"
#include "../lower/lower_validate.h"
#include "../codegen/llvm/gen.h"
#include "../codegen/js/gen.h"
#include "settings.h"
#include "source.h"
#include "project.h"
#include "Net/File.h"
#include <File.h>

/*
 * The compiler driver.
 *
 * What a project is has moved down into the resolver since this was last written: `resolveProgram`
 * takes the root module and asks a ModuleProvider for everything it imports, so linking modules
 * together is name resolution rather than anything the driver arranges. What is left here is the
 * three things only the driver can do - find the files, decide which module is the root, and take
 * the finished program to the output the command line asked for.
 */

static String joinPath(const String& directory, StringView name, StringView extension) {
    StringBuilder path(directory.size() + name.length + extension.length + 2);
    path.append(directory.text(), directory.size());
    path.append("/");
    path.append(name.ptr, name.length);
    path.append(extension.ptr, extension.length);

    return path.string();
}

static String replaceExtension(const StringView& path, const String& extension) {
    auto p = findLastChar(path, '.');
    if(!p) return toString(path) + extension;

    p++;
    auto extensionLength = path.length - (p - path.ptr);
    auto oldExtension = StringView { p, extensionLength };
    if(findChar(oldExtension, '/')) return toString(path) + extension;

    auto length = path.size() - extensionLength + extension.size();
    auto buffer = (char*)hAlloc(length);
    copy(path.ptr, buffer, path.size() - extensionLength);
    copy(extension.text(), buffer + path.size() - extensionLength, extension.size());

    return { buffer, length, true };
}

template<class Print>
static bool writeText(Context& context, const String& path, Print&& printValue) {
    try {
        Net::FileStream file;
        file.open(path, writeAccess(), File::CreateAlways);
        Net::Writer writer(Net::WriteStream(file), 16384);
        printValue(writer);
        writer.flush();
        return true;
    } catch(const Net::Exception& error) {
        context.diagnostics.error("cannot write %@: %@"_v, nullptr, path, error.description);
        return false;
    }
}

/*
 * The debug outputs.
 *
 * Written beside the source they came from, since there is one per module and the module is what
 * names them. The lowered form is the exception: lowering produces one module for the whole
 * program, so it goes to the output directory with everything else that is per-program.
 */
static void printAsts(Context& context, ModuleMap& map) {
    for(auto& entry: map.entries) {
        if(!entry.ast) continue;

        writeText(context, replaceExtension(entry.path, "ast"), [&](Net::Writer& writer) {
            printModule(writer, context, *entry.ast->region, *entry.ast);
        });
    }
}

/*
 * The targets.
 */

static bool compileJs(Context& context, Program& program, const String& outputDir, StringView name) {
    auto file = js::genProgram(context, program);
    if(context.diagnostics.errorCount()) return false;

    return writeText(context, joinPath(outputDir, name, ".js"_v), [&](Net::Writer& writer) {
        js::formatFile(writer, context, *file, false);
    });
}

// Everything the two native modes share: the resolved program as an LLVM module, entry point and
// all. `-mode llvm` stops at the text of it, which is why the wrapper and the optimization happen
// here rather than in the executable path - the IR that mode writes is the IR that mode would
// compile, and one that had been through neither would be a different program.
static Ptr<llvm::Module> genNative(llvm::LLVMContext& llvm, Context& context, Program& program) {
    auto lowered = lowerProgram(context, program);
    if(context.diagnostics.errorCount()) return nullptr;

    if(!validateLowerModule(&context.diagnostics, lowered.get())) {
        context.diagnostics.error("lowering produced invalid IR"_v, nullptr);
        return nullptr;
    }

    if(context.settings.printIr) {
        writeText(context, joinPath(context.settings.outputDir, "program"_v, ".lower"_v), [&](Net::Writer& writer) {
            printModule(writer, context, *lowered->arena, *lowered);
        });
    }

    auto module = llvmgen::genModule(llvm, context, *lowered);
    if(context.diagnostics.errorCount()) return nullptr;

    if(!llvmgen::addNativeEntry(context, *module, "main"_v)) return nullptr;
    if(!llvmgen::verifyGenModule(context, *module)) return nullptr;

    llvmgen::optimizeModule(context, *module, context.settings.optimization);
    return module;
}

static bool compileLlvm(Context& context, Program& program, const String& outputDir, StringView name) {
    llvm::LLVMContext llvm;
    auto module = genNative(llvm, context, program);
    if(!module) return false;

    return llvmgen::writeIrFile(context, *module, joinPath(outputDir, name, ".ll"_v));
}

static bool compileNative(Context& context, Program& program, const String& outputDir, StringView name) {
    llvm::LLVMContext llvm;
    auto module = genNative(llvm, context, program);
    if(!module) return false;

    auto objectPath = joinPath(outputDir, name, ".o"_v);
    if(!llvmgen::writeObjectFile(context, *module, objectPath)) return false;

    return llvmgen::linkExecutable(context, objectPath, joinPath(outputDir, name, ""_v));
}

/*
 * The `explain` query - Analysis-Ambient.md §7.3.
 *
 * Nothing is emitted and nothing is written: the answer goes to stdout, because this is a question
 * asked at a terminal rather than a build step. Everything it reads was produced by resolution and
 * the ownership passes, both of which have already run by the time this is called.
 *
 * The call-site index is built once for the whole query even when one function is being explained,
 * since "specialized at 3 of 11 call sites" is a fact about the program rather than about the
 * function - see CallSiteIndex.
 */
static bool explainProgram(Context& context, Program& program) {
    auto& settings = context.settings;

    CallSiteIndex calls;
    calls.build(program);

    if(settings.explainAll) {
        Net::Writer writer(16384);
        printExplanations(writer, context, program);
        auto buffered = writer.getBuffered();
        print(StringView { (const char*)buffered.ptr, buffered.length });
        return true;
    }

    auto moduleName = settings.explainModule == ""
        ? StringId(0)
        : context.addQualifiedName(settings.explainModule.text(), settings.explainModule.size());
    auto name = context.addQualifiedName(settings.explainName.text(), settings.explainName.size());

    Array<Function*> targets;
    findExplainTargets(program, moduleName, name, targets);

    if(targets.isEmpty()) {
        if(settings.explainModule == "") {
            println("Error: no function named %@ was found. Name the module with --module=<M> if it was not compiled.",
                    settings.explainName);
        } else {
            println("Error: module %@ declares no function named %@.",
                    settings.explainModule, settings.explainName);
        }

        return false;
    }

    // Every match rather than the first one, because a name is not a function: overloads and class
    // instances share one, and picking one of them silently would answer a question nobody asked.
    // Where there is more than one, each is said to come from somewhere - two identical signatures
    // in two modules is exactly the case the query is being asked about.
    StringBuilder text;
    for(auto function: targets) {
        if(text.size()) text.append("\n"_v);

        if(targets.size() > 1 && function->module) {
            text.append("-- in "_v);
            text.append(context.findName(function->module->name));
            text.append("\n"_v);
        }

        printExplanation(text, context, program, explainFunction(program, *function, &calls));
    }

    print(stringView(text));
    return true;
}

int main(int argc, const char** argv) {
    // Parse the provided arguments into a settings structure.
    auto result = parseCommandLine(argv, argc);
    if(result.isErr()) {
        print("Argument error: ");
        println(stringView(result.unwrapErr()));
        return 1;
    }

    auto settings = result.moveUnwrapOk();

    // The project file, if there is one, fills in what the flags did not say. It is read before
    // anything is looked for on disk because it is what says where to look - and it is the same
    // reader the language server uses, so an editor and a build cannot disagree about which files
    // are in the program. Implementation-Tooling.md §5.2.
    if(auto projectPath = locateProjectFile(settings)) {
        auto project = readProjectFile(projectPath.unwrap());
        if(project.isErr()) {
            print("Project error: ");
            println(stringView(project.unwrapErr()));
            return 1;
        }

        applyProjectFile(settings, project.unwrapOk());
    } else if(settings.projectFile != "" && !settings.noProject) {
        println("Error: cannot find a project file at %@", settings.projectFile);
        return 1;
    }

    auto settingsResult = checkSettings(settings);
    if(settingsResult.isErr()) {
        print("Argument error: ");
        println(stringView(settingsResult.unwrapErr()));
        return 1;
    }

    // Walk the input directory tree to create a map with each module we could compile.
    ModuleMap moduleMap;
    auto sourceResult = buildModuleMap(moduleMap, settings);
    if(sourceResult.isErr()) {
        print("File error: ");
        println(stringView(sourceResult.unwrapErr()));
        return 1;
    }

    if(moduleMap.entries.size() == 0) {
        println("Error: no modules to compile were found");
        return 1;
    }

    if(settings.printModules) {
        for(auto& source: moduleMap.entries) {
            println("Found module %@ at location %@", String { source.id.text, source.id.textLength }, source.path);
        }
    }

    // Create the compilation context. The provider is what both the resolver and the diagnostics
    // read source through, so it is built first and given the context once there is one.
    FileProvider provider(moduleMap);
    PrintDiagnostics diagnostics(provider);
    Context context(diagnostics);
    context.settings = ::move(settings);
    provider.prepare(context);

    String rootError;
    auto root = findRootModule(moduleMap, context.settings, rootError);
    if(!root) {
        print("Error: ");
        println(stringView(rootError));
        return 1;
    }

    auto rootAst = provider.parse(*root);
    if(!rootAst || diagnostics.errorCount() > 0) return 1;

    // Everything the root imports is parsed and resolved from here, through the provider. The
    // compilation mode is already in the settings, which matters: `@platform` selects which
    // declarations exist at all, so a JS build and a native build do not share a resolved program.
    auto program = resolveProgram(context, *rootAst, &provider);

    if(context.settings.printAst) printAsts(context, moduleMap);

    if(program && context.settings.printIr) {
        writeText(context, joinPath(context.settings.outputDir, "program"_v, ".ir"_v), [&](Net::Writer& writer) {
            printProgram(writer, context, *program);
        });
    }

    if(!program || diagnostics.errorCount() > 0) return 1;

    // Before the output directory is created, deliberately: a query produces no output, so it has
    // no business leaving a directory behind.
    if(context.settings.explaining()) return explainProgram(context, *program) ? 0 : 1;

    auto& outputDir = context.settings.outputDir;
    auto directoryResult = createDirectory(outputDir);
    if(!directoryResult && directoryResult.unwrapErr() != FileError::Exists) {
        println("Cannot create output directory %@: error %@", outputDir, (U32)directoryResult.unwrapErr());
        return 1;
    }

    auto name = StringView { root->id.text, root->id.textLength };
    auto built = false;

    switch(context.settings.mode) {
        case CompileMode::NativeExecutable:
            built = compileNative(context, *program, outputDir, name);
            break;
        case CompileMode::Llvm:
            built = compileLlvm(context, *program, outputDir, name);
            break;
        case CompileMode::JsExecutable:
            built = compileJs(context, *program, outputDir, name);
            break;

        // The three that have no code behind them yet. Each is a different question the answer
        // above does not contain: a library is what a program links *against*, which needs a
        // format for the resolved declarations rather than for the code; a shared library is that
        // plus an exported symbol table and position-independent placement.
        case CompileMode::Library:
            diagnostics.error("library generation is not implemented yet"_v, nullptr);
            break;
        case CompileMode::NativeShared:
            diagnostics.error("shared library generation is not implemented yet"_v, nullptr);
            break;
        case CompileMode::JsLibrary:
            diagnostics.error("JS library generation is not implemented yet"_v, nullptr);
            break;
    }

    return built && diagnostics.errorCount() == 0 ? 0 : 1;
}
