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
 * The entry point.
 *
 * A Yana `main` is compiled under the compiler's own calling convention like every other function,
 * and what starts a process is C startup code calling `int main(void)` under the platform's. So the
 * lowered entry keeps its convention and gets a wrapper, rather than being given the C convention
 * and made to differ from every call site that already exists in the module.
 */
bool addNativeEntry(Context& context, llvm::Module& module, StringView entryName) {
    auto entry = module.getFunction(toRef(entryName));

    if(!entry) {
        context.diagnostics.error("llvm: the program has no entry point %@"_v, nullptr, toString(entryName));
        return false;
    }

    if(entry->arg_size() != 0) {
        context.diagnostics.error("llvm: the entry point %@ cannot take arguments yet"_v, nullptr, toString(entryName));
        return false;
    }

    auto& llvm = module.getContext();
    auto word = llvm::Type::getInt32Ty(llvm);

    entry->setName("yana.entry");

    auto main = llvm::Function::Create(llvm::FunctionType::get(word, false),
                                       llvm::Function::ExternalLinkage, "main", module);
    main->setCallingConv(llvm::CallingConv::C);

    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(llvm, "", main));
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
    return true;
}

/*
 * The target machine.
 *
 * Created wherever one is needed rather than passed around, since building one is a table lookup:
 * what it costs is the same either way, and a caller that had to hold one would have to know what a
 * target machine is in order to ask for anything at all.
 */

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

// The instruction set extensions the settings enabled, in the form LLVM names them. What the
// command line says the target has is what the code generator is allowed to use.
static std::string featuresOf(const CompileSettings& settings) {
    std::string features;

    auto add = [&](const char* name) {
        if(!features.empty()) features += ",";
        features += "+";
        features += name;
    };

    switch(settings.extensions.sse) {
        case TargetExtensions::AVX512: add("avx512f"); [[fallthrough]];
        case TargetExtensions::AVX2:   add("avx2");    [[fallthrough]];
        case TargetExtensions::AVX:    add("avx");     [[fallthrough]];
        case TargetExtensions::SSE4_2: add("sse4.2");  [[fallthrough]];
        case TargetExtensions::SSE4_1: add("sse4.1");  [[fallthrough]];
        case TargetExtensions::SSSE3:  add("ssse3");   [[fallthrough]];
        case TargetExtensions::SSE3:   add("sse3");    [[fallthrough]];
        case TargetExtensions::SSE2:   add("sse2");    [[fallthrough]];
        case TargetExtensions::SSE:    add("sse");     [[fallthrough]];
        case TargetExtensions::NoSSE:  break;
    }

    if(settings.extensions.popcnt) add("popcnt");
    if(settings.extensions.lzcnt) add("lzcnt");
    if(settings.extensions.fma3) add("fma");

    return features;
}

// The machine the module says it is for, with the module's data layout reconciled against it. The
// two are stated independently - the IR is generated without a code generator being linked at all
// (see targetDescriptionOf) - and a disagreement between them is a compiler bug rather than
// anything a program can cause, so it is reported and then resolved in the machine's favour.
static Ptr<llvm::TargetMachine> createMachine(Context& context, llvm::Module& module) {
    initializeTargets();

    std::string error;
    auto triple = module.getTargetTriple();
    auto target = llvm::TargetRegistry::lookupTarget(triple, error);

    if(!target) {
        context.diagnostics.error("llvm: no code generator for %@: %@"_v, nullptr,
                                  StringView { triple.data(), triple.size() },
                                  StringView { error.data(), error.size() });
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
        target->createTargetMachine(triple, "x86-64", featuresOf(context.settings), options,
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
    // the machine the program is actually for rather than against generic costs.
    auto machine = createMachine(context, module);

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
    auto machine = createMachine(context, module);
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
