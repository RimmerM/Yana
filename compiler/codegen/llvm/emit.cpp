#include "build.h"

#include <llvm/IR/LegacyPassManager.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>

#include <cstdlib>
#include <string>

/*
 * Getting the module out.
 *
 * Everything above this file works from the lower IR alone. Everything here needs a target machine,
 * a file system and a linker, which is why it is separated: a fixture that asserts what the backend
 * generates has no business initializing a code generator, and `-mode llvm` has no business needing
 * one either.
 */
namespace llvmgen {

static llvm::StringRef toRef(const String& string) {
    return { string.text(), string.size() };
}

static llvm::StringRef toRef(StringView view) {
    return { view.ptr, view.length };
}

/*
 * The command line, into the globals the library declares for it - Design-Test.md §11.2's F4.
 *
 * `main`'s three parameters, in the order compiler/compiler/builtin.h states the roles in, which is
 * why this is a loop rather than three stores. Which global each one lands in is what the library
 * said with `@builtin` and not a name written here; the lowered module is what carries the answer,
 * and the name is read back off it only because an LLVM module is addressed by name.
 *
 * A role no declaration claimed, or one whose global nothing in the program reaches, is skipped: an
 * unreached global was never emitted, and there is nothing to store into. The amd64 backend reads
 * the same three facts off the initial stack pointer and fills the same three globals - see
 * genCommandLine there.
 */
static void storeCommandLine(Context& context, llvm::Module& module, LowerModule& lowered,
                             llvm::Function* main, llvm::IRBuilder<>& builder) {
    auto base = *lowered.arena;

    for(Size i = 0; i < kCommandLineGlobals; i++) {
        auto declared = lowered.builtin(base, Builtin(i));
        if(!declared) continue;

        auto global = module.getNamedGlobal(nameOf(context, declared->name));
        if(!global) continue;

        llvm::Value* value = main->getArg(unsigned(i));

        /*
         * `argc` is a C `int` and the global holds a `Size`, so the count widens - and it widens to
         * the *pointer* width rather than to the global's declared type, because a global's LLVM type
         * is the shape of its initial bytes rather than the scalar it holds. A `Size` initialized to
         * zero is eight zero bytes, which arrives here as `[8 x i8]`, and `sext i32 to [8 x i8]` is
         * what asking the wrong question looks like.
         */
        if(Builtin(i) == Builtin::commandLineCount) {
            value = builder.CreateSExt(value, llvm::Type::getInt64Ty(module.getContext()));
        }

        builder.CreateStore(value, global);
    }
}

/*
 * The entry point.
 *
 * A Yana `main` is compiled under the compiler's own calling convention like every other function,
 * and what starts a process is C startup code calling `int main(void)` under the platform's. So the
 * lowered entry keeps its convention and gets a wrapper, rather than being given the C convention
 * and made to differ from every call site that already exists in the module.
 */
bool addNativeEntry(Context& context, llvm::Module& module, LowerModule& lowered) {
    auto name = context.findName(lowered.entry);
    auto entry = module.getFunction(toRef(name));

    if(!entry) {
        context.diagnostics.error("llvm: the program has no entry point %@"_v, nullptr, name);
        return false;
    }

    if(entry->arg_size() != 0) {
        context.diagnostics.error("llvm: the entry point %@ cannot take arguments yet"_v, nullptr, name);
        return false;
    }

    auto& llvm = module.getContext();
    auto word = llvm::Type::getInt32Ty(llvm);

    entry->setName("yana.entry");

    /*
     * And the C name has to be free before it can be taken.
     *
     * `main` in Yana is an ordinary function compiled under the compiler's own convention - the name
     * says which function the program starts *at*, not what linkage it has - and where the entry is a
     * synthesized initializer that calls it, it is still in the module under that name. Left there,
     * LLVM renames the wrapper instead and C startup calls the wrong function: the program runs
     * `main` under a convention nobody agreed on, with its own initialization skipped.
     *
     * After the rename above, so that the ordinary case - where the entry *is* `main` - has already
     * vacated the name and finds nothing here.
     */
    if(auto occupied = module.getFunction("main")) occupied->setName("yana.main");

    /*
     * `int main(int argc, char** argv, char** envp)` - the three-argument form, for F4.
     *
     * POSIX's third parameter rather than `environ`, and taking all three whether or not this
     * program reads any of them: a C `main` may declare fewer than it is passed, so the arguments
     * cost nothing where they are unused, and declaring them here is what gives storeCommandLine
     * anything to read. One parameter per command-line role in compiler/compiler/builtin.h, in that
     * order - which is the correspondence storeCommandLine indexes by, and the assertion below is it
     * said out loud.
     */
    auto pointer = llvm::PointerType::getUnqual(llvm);
    llvm::Type* mainArgs[] = { word, pointer, pointer };

    static_assert(sizeof(mainArgs) / sizeof(*mainArgs) == kCommandLineGlobals,
                  "`main`'s parameters are what the command-line globals are stored from");

    auto main = llvm::Function::Create(llvm::FunctionType::get(word, mainArgs, false),
                                       llvm::Function::ExternalLinkage, "main", module);
    main->setCallingConv(llvm::CallingConv::C);

    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(llvm, "", main));

    // Before the entry is called, because the entry is where the program's own top level runs and a
    // global read from there has to already hold what it will hold for the rest of the run.
    storeCommandLine(context, module, lowered, main, builder);

    auto call = builder.CreateCall(entry, {});
    call->setCallingConv(entry->getCallingConv());

    // The exit status is what the program answered, narrowed to what a process can report. A `main`
    // that answers nothing exits zero, which is the same thing C says about falling off the end.
    llvm::Value* status;
    auto type = call->getType();

    if(type->isVoidTy()) {
        status = llvm::ConstantInt::get(word, 0);
    } else if(type->isIntegerTy()) {
        status = builder.CreateIntCast(call, word, true);
    } else if(type->isPointerTy()) {
        status = builder.CreateIntCast(builder.CreatePtrToInt(call, llvm::Type::getInt64Ty(llvm)), word, false);
    } else {
        context.diagnostics.error("llvm: the entry point must return an integer"_v, nullptr);
        return false;
    }

    builder.CreateRet(status);

    /*
     * And everything else stops being visible outside the module, which is what makes it a program.
     *
     * Every function this backend writes was given external linkage, and that is a statement with
     * consequences: `opt` may not delete a function once it has inlined it, may not specialise one
     * for the arguments its only caller passes, and may not narrow one's convention - because for
     * all it knows something outside is going to call it by name. Nothing is. Both LLVM modes go
     * through the whole program at once and both end here, so `main` is the one symbol a linker or a
     * C caller can name and the rest are this module's own.
     *
     * What it was worth, on the ten-program corpus: `growHeap` alone was paying for `mapMemory`
     * inline at two call sites *and* keeping the out-of-line copy, and the corpus as a whole was
     * 26922 bytes of functions against 22814 with this - a sixth of the module, present because of a
     * linkage keyword. See §58 of test/bench/findings.md.
     *
     * A declaration is left alone: `memcpy` and the handful of intrinsics LLVM introduces itself are
     * bodies that live somewhere else, and internal linkage on one is a definition that is not there.
     * Globals go the same way and for the same reason - a table nothing outside reads is a table
     * whose contents can be propagated into its readers.
     */
    for(auto& function: module) {
        if(function.isDeclaration() || &function == main) continue;
        function.setLinkage(llvm::GlobalValue::InternalLinkage);
    }

    for(auto& global: module.globals()) {
        if(global.isDeclaration()) continue;
        global.setLinkage(llvm::GlobalValue::InternalLinkage);
    }

    return true;
}

/*
 * The target machine.
 *
 * Created wherever one is needed rather than passed around, since building one is a table lookup:
 * what it costs is the same either way, and a caller that had to hold one would have to know what a
 * target machine is in order to ask for anything at all.
 */

/*
 * The architectures this build can actually generate code for.
 *
 * Stated here rather than left to the target registry to answer, because the registry answers about
 * the triple in the module and the triple is written by targetDescriptionOf whether or not a code
 * generator exists for it. `-arch arm64` produced a module carrying an aarch64 triple and then a
 * target machine built for CPU "x86-64", which is not a combination that has a meaning; the
 * registry lookup would fail eventually, but only after the whole program had been generated, and
 * with a message about a triple the author never wrote.
 *
 * The list is CMakeLists.txt's `x86codegen` and nothing else. Adding a backend is adding its
 * component there, its LLVMInitialize calls below, and its arch here - not just its triple.
 */
static bool hasCodeGenerator(TargetArch arch) {
    return arch == TargetArch::X64;
}

// The CPU a target machine is built for. Taken from the architecture rather than written once, so
// that a second backend cannot silently inherit AMD64's name for its own processors.
static const char* cpuOf(TargetArch arch) {
    switch(arch) {
        case TargetArch::X64:   return "x86-64";
        case TargetArch::X86:   return "i686";
        case TargetArch::ARM64: return "generic";
        case TargetArch::ARM:   return "generic";
    }

    return "generic";
}

// Registers the code generators this build was linked against. Only the AMD64 one, which is also
// the only architecture the lowering produces IR for - see targetDescriptionOf.
static void initializeTargets() {
    static bool initialized = false;
    if(initialized) return;

    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86AsmPrinter();
    LLVMInitializeX86AsmParser();

    initialized = true;
}

/*
 * The level the settings name, as the feature list LLVM spells it with.
 *
 * One list per level and not a ladder, because that is what a level *is*: the psABI defines v2 and
 * v3 as sets, LLVM has no name for either set, and writing them out is how the two sides come to be
 * compiling for one machine. Getting this wrong is not a missed optimization - `llc` told less than
 * the local backend claims produces a slower program the benchmark then reads as a difference of
 * code generation, which is precisely what §37.2 had to correct once already.
 *
 * `x86-64-v2` and `-v3` are LLVM feature names in recent versions, but not in every version this is
 * built against, so the members are listed instead. A member LLVM does not know is ignored with a
 * warning rather than refused, which is the failure mode to prefer here.
 */
static std::string featuresOf(const CompileSettings& settings) {
    std::string features;

    auto add = [&](const char* name) {
        if(!features.empty()) features += ",";
        features += "+";
        features += name;
    };

    // v2, which is the floor: everything this compiler emits assumes it.
    //
    // `crc32` is a feature of its own in LLVM even though it is part of SSE4.2 on every part that
    // has either - the two were split so that a target description could name the instruction
    // without the string comparisons. Without it `llvm.x86.sse42.crc32.*` fails instruction
    // selection outright rather than expanding into anything, which is how this was found.
    add("sse2"); add("sse3"); add("ssse3"); add("sse4.1"); add("sse4.2"); add("crc32");
    add("popcnt"); add("cx16");

    if(settings.extensions.level >= TargetExtensions::V3) {
        add("avx"); add("avx2"); add("bmi"); add("bmi2"); add("fma"); add("f16c");
        add("lzcnt"); add("movbe"); add("xsave");
    }

    if(settings.extensions.level >= TargetExtensions::V4) {
        add("avx512f"); add("avx512bw"); add("avx512cd"); add("avx512dq"); add("avx512vl");
    }

    // The extension that is in no level, named beside one - see TargetExtensions::sha.
    if(settings.extensions.sha) add("sha");

    return features;
}

/*
 * The machine the module says it is for, with the module's data layout reconciled against it. The
 * two are stated independently - the IR is generated without a code generator being linked at all
 * (see targetDescriptionOf) - and a disagreement between them is a compiler bug rather than
 * anything a program can cause, so it is reported and then resolved in the machine's favour.
 *
 * `required` is whether the caller can proceed without one. Writing an object file cannot, and says
 * so; the optimizer can, since a missing machine costs it target-specific cost models and nothing
 * else - and emitting textual IR for an architecture this build has no backend for is a supported
 * thing to ask for, so the optimizer must not report it as a failure.
 */
static Ptr<llvm::TargetMachine> createMachine(Context& context, llvm::Module& module, bool required) {
    initializeTargets();

    // Before the registry, so that an architecture this build has no backend for is named as the
    // architecture the author asked for rather than as the triple that was derived from it.
    if(!hasCodeGenerator(context.settings.arch)) {
        if(required) {
            context.diagnostics.error("llvm: this compiler has no code generator for %@ - it can write textual IR for that target, but not an object file or an executable"_v,
                                      nullptr, archName(context.settings.arch));
        }

        return nullptr;
    }

    std::string error;
    auto triple = module.getTargetTriple();
    auto target = llvm::TargetRegistry::lookupTarget(triple, error);

    if(!target) {
        if(required) {
            context.diagnostics.error("llvm: no code generator for %@: %@"_v, nullptr,
                                      StringView { triple.data(), triple.size() },
                                      StringView { error.data(), error.size() });
        }

        return nullptr;
    }

    /*
     * An executable is placed at a known address and a shared library is not, and the difference is
     * not only about how code addresses itself: a compiler-built table holds the address of a
     * function, and prefix data holds two of them in front of the entry point (see
     * ClosureHeaderLayout). Those are absolute addresses in the IR's model, and in a position-
     * independent executable each one becomes a relocation the loader applies at run time - which
     * for the ones in prefix data means writing into a page of code, and so a text relocation.
     *
     * So an executable is built the way its own tables are written: statically placed, with every
     * address resolved by the linker. A shared library has no such option and will need the tables
     * to be relative rather than absolute, which is a change to the lowering rather than to this.
     */
    auto shared = context.settings.mode == CompileMode::NativeShared;

    llvm::TargetOptions options;
    Ptr<llvm::TargetMachine> machine {
        target->createTargetMachine(triple, cpuOf(context.settings.arch), featuresOf(context.settings), options,
                                    shared ? llvm::Reloc::PIC_ : llvm::Reloc::Static)
    };

    auto stated = module.getDataLayout().getStringRepresentation();
    auto actual = machine->createDataLayout().getStringRepresentation();

    if(stated != actual) {
        context.diagnostics.warning("llvm: the data layout stated for %@ is %@, but its code generator uses %@"_v,
                                    nullptr, StringView { triple.data(), triple.size() },
                                    StringView { stated.data(), stated.size() },
                                    StringView { actual.data(), actual.size() });
    }

    module.setDataLayout(machine->createDataLayout());
    return machine;
}

/*
 * Optimization.
 */

void optimizeModule(Context& context, llvm::Module& module, U32 level) {
    if(level == 0) return;

    // Given to the pass builder so that inlining, unrolling and vectorization are decided against
    // the machine the program is actually for rather than against generic costs. Not required: a
    // target with no backend here still optimizes, against those generic costs.
    auto machine = createMachine(context, module, false);

    llvm::LoopAnalysisManager loops;
    llvm::FunctionAnalysisManager functions;
    llvm::CGSCCAnalysisManager callGraph;
    llvm::ModuleAnalysisManager modules;
    llvm::PassBuilder builder(machine.get());

    builder.registerModuleAnalyses(modules);
    builder.registerCGSCCAnalyses(callGraph);
    builder.registerFunctionAnalyses(functions);
    builder.registerLoopAnalyses(loops);
    builder.crossRegisterProxies(loops, functions, callGraph, modules);

    auto optimization = level >= 3 ? llvm::OptimizationLevel::O3
                      : level == 2 ? llvm::OptimizationLevel::O2
                                   : llvm::OptimizationLevel::O1;

    auto pipeline = builder.buildPerModuleDefaultPipeline(optimization);
    pipeline.run(module, modules);
}

/*
 * Files.
 */

bool writeIrFile(Context& context, llvm::Module& module, const String& path) {
    std::error_code error;
    llvm::raw_fd_ostream file(toRef(path), error, llvm::sys::fs::OF_Text);

    if(error) {
        context.diagnostics.error("llvm: cannot write %@: %@"_v, nullptr, path,
                                  StringView { error.message().data(), error.message().size() });
        return false;
    }

    module.print(file, nullptr);
    return true;
}

bool writeObjectFile(Context& context, llvm::Module& module, const String& path) {
    auto machine = createMachine(context, module, true);
    if(!machine) return false;

    std::error_code fileError;
    llvm::raw_fd_ostream file(toRef(path), fileError, llvm::sys::fs::OF_None);

    if(fileError) {
        context.diagnostics.error("llvm: cannot write %@: %@"_v, nullptr, path,
                                  StringView { fileError.message().data(), fileError.message().size() });
        return false;
    }

    llvm::legacy::PassManager passes;
    if(machine->addPassesToEmitFile(passes, file, nullptr, llvm::CGFT_ObjectFile)) {
        context.diagnostics.error("llvm: this target cannot emit an object file"_v, nullptr);
        return false;
    }

    passes.run(module);
    file.flush();
    return true;
}

/*
 * Linking.
 *
 * Through the platform's C toolchain driver rather than through a linker directly, because what
 * turns an object file into a program is not only the linker: it is the startup code that calls
 * `main`, the C library the runtime's own syscalls stand beside, and the dynamic loader's
 * arrangements. Every one of those is what `cc` knows and a bare `ld` invocation would have to
 * restate.
 */
bool linkExecutable(Context& context, const String& objectPath, const String& outputPath) {
    // Not a position-independent executable, because the code was generated as one that is not -
    // see createMachine. The two have to agree: absolute addresses in a PIE are relocations the
    // loader has to apply, and the ones inside prefix data are relocations against code.
    char command[4096];
    auto length = format(toBuffer(command), String("cc -no-pie -o \"%@\" \"%@\""), outputPath, objectPath);

    if(length >= sizeof(command) - 1) {
        context.diagnostics.error("llvm: the link command is too long"_v, nullptr);
        return false;
    }

    command[length] = 0;
    auto result = system(command);

    if(result != 0) {
        context.diagnostics.error("llvm: linking %@ failed with status %@"_v, nullptr, outputPath, result);
        return false;
    }

    return true;
}

} // namespace llvmgen
