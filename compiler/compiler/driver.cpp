#include "../parse/parser.h"
#include "../parse/ast_print.h"
#include "../resolve/module.h"
#include "../resolve/print.h"
#include "../resolve/explain.h"
#include "../resolve/test.h"
#include "../resolve/lower.h"
#include "../resolve/analyze.h"
#include "../lower/lower_print.h"
#include "../lower/lower_validate.h"
#include "../codegen/llvm/gen.h"
#include "../codegen/js/gen.h"
#include "../codegen/x64/emit.h"
#include "../opt/opt.h"
#include "../repr/repr.h"
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
 * `-mode ast` - what the parser made of every file, and nothing after parsing.
 *
 * Into the output directory, named by each file's own dotted identifier. These used to be written
 * beside the source they came from, under `-print-ast`, which made a dump of a source tree an edit
 * to that tree - and pointing it at `lib/` scattered `.ast` files through the standard library. A
 * mode writes what it produces where every other mode writes what it produces.
 *
 * One file per *source file* rather than per module, because that is what an AST is: grouping files
 * into modules is the step after this one, and a dump of the parse should not have taken it.
 */
static bool printAsts(Context& context, ModuleMap& map) {
    auto written = true;

    for(auto& entry: map.entries) {
        if(!entry.ast) continue;

        auto name = StringView { entry.id.text, entry.id.textLength };
        written &= writeText(context, joinPath(context.settings.outputDir, name, ".ast"_v),
                             [&](Net::Writer& writer) {
            printModule(writer, context, *context.parseRegion, *entry.ast);
        });
    }

    return written;
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

/*
 * `-mode ownership` - what the ownership passes concluded.
 *
 * Where each local is live and where its drop went, which is the one thing the compiler works out
 * that neither the resolve IR nor the lowered form shows on its face. It needs the live ranges,
 * which a compilation does not otherwise build - see CompileSettings::ownershipRanges - so the mode
 * is what turns them on, and `parseCommandLine` sets the flag when it reads this mode rather than
 * leaving a switch that could be forgotten.
 *
 * Before the optimizer, deliberately: these are the conclusions the *ownership passes* reached, and
 * running the optimizer first would show them after inlining had moved the code they are about.
 */
static bool compileOwnership(Context& context, Program& program, const String& outputDir, StringView name) {
    return writeText(context, joinPath(outputDir, name, ".own"_v), [&](Net::Writer& writer) {
        printOwnership(writer, context, program);
    });
}

/*
 * `-mode ir` - the resolved program, as the backends read it.
 *
 * Optimized, because that is the program that is actually compiled: the optimizer rewrites the
 * resolve IR in place and both backends call it on the way in, so the unoptimized form is a program
 * no backend ever sees. `-no-opt` is what asks for that one, and it is the same switch that makes
 * every other stage skip the optimizer - so the two forms this used to write as two files are now
 * one file and a flag that composes with everything else.
 *
 * The target matters even here: `optimizeProgram` takes a ReprTarget, and a JS build and a native
 * build are two resolved programs optimized against two layouts. This is the only way to read the
 * JavaScript one, which is why the mode is available on both platforms while `-mode lower` is not.
 */
static bool compileIr(Context& context, Program& program, const String& outputDir, StringView name) {
    {
        StageScope stage(CompileStage::Optimize);
        optimizeProgram(context, program,
                        isJsMode(context.settings) ? jsReprTarget() : nativeReprTarget());
    }

    if(context.diagnostics.errorCount()) return false;

    return writeText(context, joinPath(outputDir, name, ".ir"_v), [&](Net::Writer& writer) {
        printProgram(writer, context, program);
    });
}

/*
 * `-mode lower` - the form both native backends are generated from.
 *
 * Native only, and refused for a JavaScript build by `checkSettings` rather than here: the JS
 * backend generates straight from the resolve IR, so there is no lowered form of a JS program to
 * write. What comes out has been through `validateLowerModule` like every other native build, which
 * makes this mode also the shortest way to ask whether lowering a program produces valid IR.
 */
static bool compileLower(Context& context, Program& program, const String& outputDir, StringView name) {
    auto lowered = lowerNative(context, program);
    if(!lowered) return false;

    return writeText(context, joinPath(outputDir, name, ".lower"_v), [&](Net::Writer& writer) {
        printModule(writer, context, *lowered->arena, *lowered);
    });
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

// The output directory, made where it does not exist. Two callers now: every mode that writes a
// program, and `-mode ast`, which stops before the rest of them and still has files to write.
static bool ensureOutputDirectory(const String& outputDir) {
    auto result = createDirectory(outputDir);
    if(!result && result.unwrapErr() != FileError::Exists) {
        printlnError("Cannot create output directory %@: error %@", outputDir, (U32)result.unwrapErr());
        return false;
    }

    return true;
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
            println("Error: no function named %@ was found. Write it as <Module>.%@ if it is in a module this build did not compile.",
                    settings.explainName, settings.explainName);
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
        printError("Argument error: ");
        printlnError(stringView(result.unwrapErr()));
        return 1;
    }

    auto settings = result.moveUnwrapOk();

    if(settings.help) {
        print(helpText());
        return 0;
    }

    // What the positional arguments were - a project, or sources. Asked before the project file is
    // looked for, because naming one is one of the two answers.
    auto inputResult = resolveInputs(settings);
    if(inputResult.isErr()) {
        printError("Argument error: ");
        printlnError(stringView(inputResult.unwrapErr()));
        return 1;
    }

    // The project file, if there is one, fills in what the flags did not say. It is read before
    // anything is looked for on disk because it is what says where to look - and it is the same
    // reader the language server uses, so an editor and a build cannot disagree about which files
    // are in the program. Implementation-Tooling.md §5.2.
    auto named = settings.projectFile != "";
    if(auto projectPath = locateProjectFile(settings)) {
        auto project = readProjectFile(projectPath.unwrap());
        if(project.isErr()) {
            printError("Project error: ");
            printlnError(stringView(project.unwrapErr()));
            return 1;
        }

        applyProjectFile(settings, project.unwrapOk());
    } else if(named && !settings.noProject) {
        printlnError("Error: cannot find a project file at %@", settings.projectFile);
        return 1;
    } else if(settings.compileObjects.isEmpty() && !settings.noProject) {
        // Nothing named and no project here. The walk upwards happens inside this sentence and
        // nowhere else - see describeMissingProject.
        printError("Error: ");
        printlnError(stringView(describeMissingProject(settings)));
        return 1;
    }

    /*
     * Where the build goes, once everything that could have said has had its turn.
     *
     * The working directory, which is what every other compiler does with an unnamed output. This
     * used to default to `argv[0]` - the compiler's own executable path - so a build that named no
     * `-to` resolved, optimized, lowered and generated code before failing to write its result into
     * a path that is a file rather than a directory.
     */
    if(settings.outputDir == "") settings.outputDir = String(".");

    auto settingsResult = checkSettings(settings);
    if(settingsResult.isErr()) {
        printError("Argument error: ");
        printlnError(stringView(settingsResult.unwrapErr()));
        return 1;
    }

    /*
     * A `-opt` nothing will read.
     *
     * Only the LLVM backend has levels, and the local one is the default wherever it exists - so
     * `yana -opt 3` is a reasonable thing to write, does nothing, and said nothing about it. A note
     * rather than an error, because the build it was passed to is still the build that was asked
     * for: what is wrong is the belief about what was measured, and that is what this corrects.
     */
    if(settings.explicitOptimization &&
       (isJsMode(settings) || isTextMode(settings.mode) || settings.backend != NativeBackend::Llvm)) {
        printlnError("Note: -opt sets LLVM's optimization level, and this build does not run LLVM. "
                     "Add -backend llvm for it to mean anything; -no-ir-opt is the switch on the IR "
                     "optimizer, which runs either way.");
    }

    // Walk the input directory tree to create a map with each module we could compile.
    ModuleMap moduleMap;
    auto sourceResult = buildModuleMap(moduleMap, settings);
    if(sourceResult.isErr()) {
        printError("File error: ");
        printlnError(stringView(sourceResult.unwrapErr()));
        return 1;
    }

    if(moduleMap.entries.size() == 0) {
        printlnError("Error: no modules to compile were found");
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
        printError("Error: ");
        printlnError(stringView(rootError));
        return 1;
    }

    // Every file is parsed by `prepare`, which is what grouping them into modules needed - see
    // FileProvider::prepare. So a parse error anywhere in the tree stops the compile here.
    if(diagnostics.errorCount() > 0) return 1;

    /*
     * `-mode ast`, which is finished already: parsing is the stage it stops at.
     *
     * Before resolution rather than after it, because an AST is what the parser produced and nothing
     * downstream changes it - and because stopping here is what makes the mode useful on a program
     * that does not resolve. `-print-ast` ran after a successful resolve and so could not dump the
     * parse of the file you were trying to understand.
     */
    if(context.settings.mode == CompileMode::Ast) {
        if(!ensureOutputDirectory(context.settings.outputDir)) return 1;
        return printAsts(context, moduleMap) && diagnostics.errorCount() == 0 ? 0 : 1;
    }

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

    if(!program || diagnostics.errorCount() > 0) return 1;

    // Before the output directory is created, deliberately: a query produces no output, so it has
    // no business leaving a directory behind.
    if(context.settings.explaining()) return explainProgram(context, *program) ? 0 : 1;

    auto& outputDir = context.settings.outputDir;
    if(!ensureOutputDirectory(outputDir)) return 1;

    // What the artifact is called - `-o`, or the root module's name. The mode adds the extension,
    // which is why this is a bare name rather than a file name.
    auto& given = context.settings.outputName;
    auto name = given != "" ? StringView { given.text(), given.size() }
                            : StringView { root->text.text(), root->text.size() };
    auto built = false;

    /*
     * The mode decides what is produced and the platform decides what it is produced for, so the
     * dispatch reads both - see TargetPlatform. Only `Executable` genuinely branches on the
     * platform: the two text modes below it either work on both (`ir`) or were refused for one by
     * `checkSettings` (`lower`, `llvm`), which is where a combination that does not exist is
     * reported, rather than here where there would be nothing useful to say about it.
     */
    switch(context.settings.mode) {
        case CompileMode::Executable:
            if(isJsMode(context.settings)) {
                built = compileJs(context, *program, outputDir, name);
                break;
            }

            // The one output with two implementations - see NativeBackend. Which one this is came
            // from the target unless a flag overrode it, and a flag that named a target with no code
            // generator behind it was already refused by checkSettings, so what is left here is only
            // the choice.
            built = context.settings.backend == NativeBackend::Local
                ? compileNativeLocal(context, *program, outputDir, name)
                : compileNative(context, *program, outputDir, name);
            break;
        case CompileMode::Ownership:
            built = compileOwnership(context, *program, outputDir, name);
            break;
        case CompileMode::Ir:
            built = compileIr(context, *program, outputDir, name);
            break;
        case CompileMode::Lower:
            built = compileLower(context, *program, outputDir, name);
            break;
        case CompileMode::Llvm:
            built = compileLlvm(context, *program, outputDir, name);
            break;

        // Handled above, before there was a program to switch on.
        case CompileMode::Ast:
            break;

        // The two that have no code behind them yet. Each is a different question the answer above
        // does not contain: a library is what a program links *against*, which needs a format for
        // the resolved declarations rather than for the code; a shared library is that plus an
        // exported symbol table and position-independent placement.
        case CompileMode::Library:
            diagnostics.error(isJsMode(context.settings)
                ? "JS library generation is not implemented yet"_v
                : "library generation is not implemented yet"_v, nullptr);
            break;
        case CompileMode::Shared:
            diagnostics.error("shared library generation is not implemented yet"_v, nullptr);
            break;
    }

    return built && diagnostics.errorCount() == 0 ? 0 : 1;
}
