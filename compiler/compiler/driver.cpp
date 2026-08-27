#include "../parse/parser.h"
#include "../parse/ast_print.h"
#include "../resolve/module.h"
#include "../resolve/print.h"
#include "../resolve/explain.h"
#include "../resolve/test.h"
#include "../resolve/lower.h"
#include "../lower/lower_print.h"
#include "../lower/lower_validate.h"
#include "../codegen/llvm/gen.h"
#include "../codegen/js/gen.h"
#include "../codegen/x64/emit.h"
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
            printModule(writer, context, *context.parseRegion, *entry.ast);
        });
    }
}

/*
 * The targets.
 */

static bool compileJs(Context& context, Program& program, const String& outputDir, StringView name) {
    auto file = js::genProgram(context, program);
    if(context.diagnostics.errorCount()) return false;

    // The same dump `lowerNative` writes, for the same reason and after the same step: `genProgram`
    // runs the optimizer over `program` in place, so this is the resolve IR the emitter below read.
    // The two targets resolve and optimize separately, so a question about a pass in `opt/` has a
    // different answer on each and this is the only place the JS one can be read.
    if(context.settings.printIr) {
        writeText(context, joinPath(context.settings.outputDir, "program"_v, ".opt.ir"_v), [&](Net::Writer& writer) {
            printProgram(writer, context, program);
        });
    }

    return writeText(context, joinPath(outputDir, name, ".js"_v), [&](Net::Writer& writer) {
        js::formatFile(writer, context, *file, false);
    });
}

/*
 * Everything every native build shares, whichever backend then consumes it: the lowered form of the
 * program, checked, written out if it was asked for, and known to have somewhere to start.
 *
 * Above the fork rather than inside either arm, because it is the same program either way. The two
 * backends are two code generators over one IR, and the useful thing about being able to switch
 * between them with a flag is that everything before the switch is held fixed - a difference between
 * their output is then a difference between them.
 */
static Ptr<LowerModule> lowerNative(Context& context, Program& program) {
    auto lowered = lowerProgram(context, program);
    if(context.diagnostics.errorCount()) return nullptr;

    if(!validateLowerModule(&context.diagnostics, lowered.get())) {
        context.diagnostics.error("lowering produced invalid IR"_v, nullptr);
        return nullptr;
    }

    if(context.settings.printIr) {
        // `lowerProgram` runs the optimizer over `program` in place, so this is the resolve IR the
        // lowering below it actually read - which is a different program from the `.ir` written at
        // the end of resolution, and the one to read when a question is about a pass in `opt/`.
        writeText(context, joinPath(context.settings.outputDir, "program"_v, ".opt.ir"_v), [&](Net::Writer& writer) {
            printProgram(writer, context, program);
        });

        writeText(context, joinPath(context.settings.outputDir, "program"_v, ".lower"_v), [&](Net::Writer& writer) {
            printModule(writer, context, *lowered->arena, *lowered);
        });
    }

    // The program's start rather than `main` by name - Analysis-Initialization.md stage B. Where the
    // root module has top-level statements, `main` is what the synthesized entry calls last, and
    // starting at `main` itself would start the program after its own initialization had been
    // skipped. Asked here, so that a program with nowhere to start is reported once and by the same
    // sentence however it was going to be compiled.
    if(!lowered->entry) {
        context.diagnostics.error("the program has no entry point - the module being compiled declares neither `main` nor any top-level statement"_v,
                                  nullptr);
        return nullptr;
    }

    return lowered;
}

// Everything the two LLVM modes share: the resolved program as an LLVM module, entry point and all.
// `-mode llvm` stops at the text of it, which is why the wrapper and the optimization happen here
// rather than in the executable path - the IR that mode writes is the IR that mode would compile,
// and one that had been through neither would be a different program.
static Ptr<llvm::Module> genNative(llvm::LLVMContext& llvm, Context& context, Program& program) {
    auto lowered = lowerNative(context, program);
    if(!lowered) return nullptr;

    auto module = llvmgen::genModule(llvm, context, *lowered);
    if(context.diagnostics.errorCount()) return nullptr;

    if(!llvmgen::addNativeEntry(context, *module, *lowered)) return nullptr;
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
 * The same mode through the compiler's own backend.
 *
 * No object file and no link step, because there is nothing to link: the whole program is in one
 * lowered module, and everything it calls is in it. What comes out is the executable itself, at the
 * same path `-backend llvm` would have left one - the two differ in how the file was produced and in
 * nothing a caller has to know about.
 */
static bool compileNativeLocal(Context& context, Program& program, const String& outputDir, StringView name) {
    auto lowered = lowerNative(context, program);
    if(!lowered) return false;

    return genX64Executable(context, *lowered, joinPath(outputDir, name, ""_v));
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

    // Every file is parsed by `prepare`, which is what grouping them into modules needed - see
    // FileProvider::prepare. So a parse error anywhere in the tree stops the compile here.
    if(diagnostics.errorCount() > 0) return 1;

    // Everything the root imports is parsed and resolved from here, through the provider. The
    // compilation mode is already in the settings, which matters: `@platform` selects which
    // declarations exist at all, so a JS build and a native build do not share a resolved program.

    /*
     * The modules that hold tests, which nothing imports - see Design-Test.md §3.4.
     *
     * A test module is not reached from the program it tests, since the dependency runs the other
     * way, so a walk from the root alone would compile the program and none of its tests. The list
     * is the modules that actually declare a `@test` and not every mapped group: a module with none
     * contributes nothing to a suite, and one that a test module *uses* is reached from it by an
     * ordinary import like anything else.
     */
    Array<ast::ModuleGroup*> testRoots;

    if(context.settings.test) {
        for(auto& group: moduleMap.groups) {
            if(moduleDeclaresTests(context, group.parsed)) testRoots.push(&group.parsed);
        }
    }

    auto specialization = context.settings.forceGeneric ? Program::Specialization::Generic
                                                        : Program::Specialization::Always;

    auto program = resolveProgram(context, root->parsed, &provider, specialization,
                                  toBuffer(testRoots));

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

    auto name = StringView { root->text.text(), root->text.size() };
    auto built = false;

    switch(context.settings.mode) {
        // The one mode with two implementations - see NativeBackend. Which one this is came from the
        // target unless a flag overrode it, and a flag that named a target with no code generator
        // behind it was already refused by checkSettings, so what is left here is only the choice.
        case CompileMode::NativeExecutable:
            built = context.settings.backend == NativeBackend::Local
                ? compileNativeLocal(context, *program, outputDir, name)
                : compileNative(context, *program, outputDir, name);
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
