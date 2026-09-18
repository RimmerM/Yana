#include "build.h"
#include "../entry.h"
#include "../raw/raw.h"

#include <llvm/ADT/APInt.h>
#include <llvm/BinaryFormat/ELF.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>

#include <cstdlib>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <string>
#include <vector>

/*
 * Getting the module out.
 *
 * Everything above this file works from the lower IR alone. Everything here needs a target machine
 * and a file system, which is why it is separated: a fixture that asserts what the backend generates
 * has no business initializing a code generator, and `-mode llvm` has no business needing one either.
 */
namespace llvmgen {

static llvm::StringRef toRef(const String& string) {
    return { string.text(), string.size() };
}

static llvm::StringRef toRef(StringView view) {
    return { view.ptr, view.length };
}

static StringView diagnosticText(const std::string& text) {
    return StringView { text.data(), Size(text.size()) };
}

/*
 * The command line, into the globals the library declares for it - Design-Test.md §11.2's F4.
 *
 * `main`'s three parameters, in the order compiler/intrinsic/supplied.def states the roles in, which is
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

    for(Size i = 0; i < intrinsic::kCommandLineGlobals; i++) {
        auto declared = lowered.builtin(base, intrinsic::Builtin(i));
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
        if(intrinsic::Builtin(i) == intrinsic::Builtin::commandLineCount) {
            value = builder.CreateSExt(value, llvm::Type::getInt64Ty(module.getContext()));
        }

        builder.CreateStore(value, global);
    }
}

/*
 * Whether this target starts itself.
 *
 * A Linux x86-64 ELF or raw image does: the entry sequence below is eleven instructions of the
 * architecture's own assembly, and everything under it - the allocator, the file system, the clock
 * - is already written over raw system calls. Everything else keeps the C handshake, which is a real
 * fallback rather than a hypothetical one: a Mach-O or PE object is linked by a toolchain whose
 * startup code is the platform's, and this backend has no `_start` for either.
 */
static bool startsItself(const CompileSettings& settings) {
    return settings.target == TargetType::Linux && settings.arch == TargetArch::X64 &&
           (settings.format == ExecutableFormat::ELF || settings.format == ExecutableFormat::Raw);
}

/*
 * The process entry, as assembly in the module - and why an LLVM-compiled program had a C runtime
 * inside it until it existed.
 *
 * A Yana program's runtime is Yana: `mapMemory` is `mmap`, `File` is `openat` and `write`, the clock
 * is `clock_gettime`, and the local backend produces an executable with no libraries in it at all.
 * The LLVM path was linking `cc -no-pie`, which is `crt1.o` plus `libc.so.6` plus the dynamic
 * loader, for exactly two things: somebody to call `main`, and `memcpy`. The program's own code
 * needed none of it - `nm -u` over the whole `test/resolve` corpus, compiled through this backend,
 * named one undefined symbol and it was `memcpy`.
 *
 * That is worth removing on its own terms - a 16096-byte hello world became 9512, statically linked,
 * with no interpreter and no relocation to apply at startup - but the reason it is *here* is the
 * entry point: the two backends now write the same sequence, from the same constants in
 * codegen/entry.h, instead of one of them writing it and the other asking a C runtime to.
 *
 * What the assembly is:
 *
 *   xor ebp, ebp                the outermost frame has no caller - a stack walk stops here
 *   mov rdi, [rsp]              argc, while the stack is still the one the kernel handed over
 *   lea rsi, [rsp+8]            argv
 *   lea rdx, [rsp+rdi*8+16]     envp - past the vector and its null terminator
 *   and rsp, -64                aligned to a vector; see kEntryStackAlignment
 *   call main                   the wrapper below, which fills the globals and runs the program
 *   mov edi, eax                the status it answered
 *   mov eax, 231                __NR_exit_group
 *   syscall
 *   hlt                         unreachable; a fault is better than running into what follows
 *
 * The three registers are the C convention's first three arguments, so the wrapper this calls is
 * exactly the `main` the platform's startup code would have called - which is what keeps this a
 * change of *who calls it* rather than a second entry sequence with its own rules about the roles.
 *
 * `memcpy`, `memmove` and `memset` are here because they are the three libcalls an LLVM memory
 * intrinsic may become, and nothing else in the program is going to define them. `rep movsb` is the
 * same instruction the local backend emits for `blockCopy` and is what the library's own ladder
 * calls above `width * 48` bytes; below that the ladder never reaches a libcall at all, because
 * `copyMemory` is written in Yana. A `memcmp` or `bcmp` - which LLVM can form out of a comparison
 * loop, and did not once in the corpus - is deliberately absent: a link error naming it is a better
 * outcome than a slow one nobody measured.
 */
static void addProcessEntry(llvm::Module& module, bool raw) {
    char text[4096];
    auto length = format(toBuffer(text), String(
        "%@"
        "\t.globl _start\n"
        "\t.type _start,@function\n"
        "_start:\n"
        "\txorl %ebp, %ebp\n"
        "\tmovq (%rsp), %rdi\n"
        "\tleaq 8(%rsp), %rsi\n"
        "\tleaq 16(%rsp,%rdi,8), %rdx\n"
        "\tandq $-%@, %rsp\n"
        "\tcall main\n"
        "\tmovl %eax, %edi\n"
        "\tmovl $%@, %eax\n"
        "\tsyscall\n"
        "\thlt\n"

        "\t.globl memcpy\n"
        "\t.type memcpy,@function\n"
        "memcpy:\n"
        "\tmovq %rdi, %rax\n"
        "\tmovq %rdx, %rcx\n"
        "\trep movsb\n"
        "\tret\n"

        "\t.globl memmove\n"
        "\t.type memmove,@function\n"
        "memmove:\n"
        "\tmovq %rdi, %rax\n"
        "\tmovq %rdx, %rcx\n"
        "\tcmpq %rsi, %rdi\n"
        "\tjbe 1f\n"
        "\tleaq (%rsi,%rdx), %r8\n"
        "\tcmpq %r8, %rdi\n"
        "\tjae 1f\n"
        "\tleaq -1(%rdi,%rdx), %rdi\n"
        "\tleaq -1(%rsi,%rdx), %rsi\n"
        "\tstd\n"
        "\trep movsb\n"
        "\tcld\n"
        "\tret\n"
        "1:\n"
        "\trep movsb\n"
        "\tret\n"

        "\t.globl memset\n"
        "\t.type memset,@function\n"
        "memset:\n"
        "\tmovq %rdi, %r8\n"
        "\tmovl %esi, %eax\n"
        "\tmovq %rdx, %rcx\n"
        "\trep stosb\n"
        "\tmovq %r8, %rax\n"
        "\tret\n"

        // Without it the linker marks the stack executable and says so, because an object with
        // assembly in it and no such note is one it cannot prove otherwise about.
        "\t.section .note.GNU-stack,\"\",@progbits\n"
    ), raw ? String("\t.section .text.entry,\"ax\",@progbits\n") : String("\t.text\n"),
       kEntryStackAlignment, kSysExitGroup);

    module.appendModuleInlineAsm(llvm::StringRef(text, length));
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

    // A bare target deliberately has no process ABI. Its entry is an ordinary Yana function: do
    // not manufacture argc/argv, rename it behind a C wrapper, or impose a return type. Keeping the
    // symbol external gives a future container writer (and a debugger) a stable entry symbol; the
    // raw writer still places its bytes first because a flat image has no symbol table.
    if(context.settings.target == TargetType::Bare) {
        // A raw image has no entry field. Give the ordinary entry its own first text section so the
        // in-process object flattener can place it at byte zero even if LLVM changes the surrounding
        // function order. ELF/PE writers may ignore this section name; the symbol remains the entry
        // point for those containers.
        if(context.settings.format == ExecutableFormat::Raw) entry->setSection(".text.entry");

        for(auto& function: module) {
            if(function.isDeclaration() || &function == entry) continue;
            function.setLinkage(llvm::GlobalValue::InternalLinkage);
        }

        for(auto& global: module.globals()) {
            if(global.isDeclaration()) continue;
            global.setLinkage(llvm::GlobalValue::InternalLinkage);
        }

        return true;
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
     * anything to read. One parameter per command-line role in compiler/intrinsic/supplied.def, in that
     * order - which is the correspondence storeCommandLine indexes by, and the assertion below is it
     * said out loud.
     */
    auto pointer = llvm::PointerType::getUnqual(llvm);
    llvm::Type* mainArgs[] = { word, pointer, pointer };

    static_assert(sizeof(mainArgs) / sizeof(*mainArgs) == intrinsic::kCommandLineGlobals,
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

    // And `main` stops being the C runtime's business, where this target has no C runtime. The
    // wrapper above is unchanged and still external - what changes is who calls it.
    if(startsItself(context.settings)) {
        addProcessEntry(module, context.settings.format == ExecutableFormat::Raw);
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
 * The level and flags the settings name, as the feature list LLVM spells it with.
 *
 * Read from the one level table in compiler/settings.cpp (`llvmAttributesOf`) rather than written out
 * here, which is Analysis-Avx512.md §0.4's rule: a second list is how the v3 description came to be
 * written from memory and wrong once. Getting this wrong is not a missed optimization - `llc` told
 * less than the local backend claims produces a slower program the benchmark then reads as a
 * difference of code generation, which is precisely what §37.2 had to correct once already.
 *
 * The psABI level names are LLVM feature names in recent versions, but not in every version this is
 * built against - and this compiler's levels are not exactly them anyway - so the members are listed.
 * A member LLVM does not know is ignored with a warning rather than refused, which is the failure
 * mode to prefer here.
 */
static std::string featuresOf(const CompileSettings& settings) {
    // SSE2, which every x86-64 part has and no level lists.
    std::string features = "+sse2";

    for(auto name: llvmAttributesOf(settings.extensions)) {
        features += ",+";
        features += name;
    }

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

    // The spelling, for the two messages below - a module's triple is a `Triple` since LLVM 21 and
    // was the string itself before.
#if LLVM_VERSION_MAJOR >= 21
    const std::string& tripleText = triple.str();
#else
    const std::string& tripleText = triple;
#endif

    if(!target) {
        if(required) {
            context.diagnostics.error("llvm: no code generator for %@: %@"_v, nullptr,
                                      StringView { tripleText.data(), tripleText.size() },
                                      StringView { error.data(), error.size() });
        }

        return nullptr;
    }

    /*
     * An executable is placed at a known address and a shared library is not, and the difference is
     * not only about how code addresses itself: a compiler-built table holds the address of a
     * function, and a closure header holds one in front of the entry point as prefix data (see
     * ClosureHeaderLayout). A slot is a 32-bit offset from the image anchor. ELF uses zero as that
     * anchor, while raw output uses its selected image base (see anchorIsAbsolute), so the object
     * file carries one ordinary absolute relocation rather than a difference across sections. In a
     * position-independent executable that is a relocation the loader applies at run time, and for
     * the one in prefix data it is a write into a page of code, and so a text relocation.
     *
     * So an executable is built the way its own tables are written: statically placed, with every
     * address resolved by the linker. A shared library has no such option and will need the tables
     * to be relative rather than absolute, which is a change to the lowering rather than to this.
     */
    auto shared = context.settings.mode == CompileMode::Shared;

    llvm::TargetOptions options;
    Ptr<llvm::TargetMachine> machine {
        target->createTargetMachine(triple, cpuOf(context.settings.arch), featuresOf(context.settings), options,
                                    shared ? llvm::Reloc::PIC_ : llvm::Reloc::Static)
    };

    auto stated = module.getDataLayout().getStringRepresentation();
    auto actual = machine->createDataLayout().getStringRepresentation();

    if(stated != actual) {
        context.diagnostics.warning("llvm: the data layout stated for %@ is %@, but its code generator uses %@"_v,
                                    nullptr, StringView { tripleText.data(), tripleText.size() },
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
#if LLVM_VERSION_MAJOR >= 18
    auto fileType = llvm::CodeGenFileType::ObjectFile;
#else
    auto fileType = llvm::CGFT_ObjectFile;
#endif

    if(machine->addPassesToEmitFile(passes, file, nullptr, fileType)) {
        context.diagnostics.error("llvm: this target cannot emit an object file"_v, nullptr);
        return false;
    }

    passes.run(module);
    file.flush();
    return true;
}

/*
 * LLVM has an object-file emitter, not a flat-binary emitter. A raw image is small enough that the
 * missing link step can be explicit and deterministic here: copy allocatable sections with the raw
 * entry section first, then the remaining text and data, and apply the x86-64 static relocations
 * LLVM left for us. No undefined symbol can be meaningful in a freestanding image, and refusing
 * one is preferable to writing a binary whose loader will discover it much later.
 */
bool writeRawObjectFile(Context& context, const String& objectPath, const String& outputPath,
                        U64 imageBase) {
    auto loaded = llvm::MemoryBuffer::getFile(toRef(objectPath));
    if(!loaded) {
        context.diagnostics.error("llvm: cannot read %@: %@"_v, nullptr, objectPath,
                                  StringView { loaded.getError().message().data(),
                                               loaded.getError().message().size() });
        return false;
    }

    auto buffer = std::move(*loaded);
    auto binary = llvm::object::createBinary(buffer->getMemBufferRef());
    if(!binary) {
        auto message = llvm::toString(binary.takeError());
        context.diagnostics.error("llvm: cannot read raw object %@: %@"_v, nullptr, objectPath,
                                  diagnosticText(message));
        return false;
    }

    auto* object = llvm::dyn_cast<llvm::object::ObjectFile>(binary->get());
    if(!object || !llvm::dyn_cast<llvm::object::ELFObjectFileBase>(object)) {
        context.diagnostics.error("llvm: raw output currently requires an x86-64 ELF object"_v,
                                  nullptr);
        return false;
    }

    struct Section {
        llvm::object::SectionRef source;
        U64 offset = 0;
    };

    std::vector<Section> sections;
    std::unordered_map<U64, U64> sectionOffsets;
    std::vector<Byte> image;

    auto alignUp = [](U64 value, U64 alignment, U64& result) {
        if(alignment <= 1) {
            result = value;
            return true;
        }

        auto padding = alignment - 1;
        if(value > std::numeric_limits<U64>::max() - padding) return false;
        result = (value + padding) & ~padding;
        return true;
    };

    // Text and read-only data come first, followed by writable data and BSS. This is a flat-image
    // policy rather than a claim that raw output has ELF-like protections; it keeps the usual code
    // and data separation while making the resulting bytes independent of section-header order.
    for(U32 pass = 0; pass < 3; pass++) {
        for(auto source: object->sections()) {
            auto text = source.isBerkeleyText();
            auto data = source.isBerkeleyData() || source.isBSS();
            auto name = source.getName();
            if(!name) {
                auto message = llvm::toString(name.takeError());
                context.diagnostics.error("llvm: cannot inspect raw section: %@"_v, nullptr,
                                          diagnosticText(message));
                return false;
            }

            auto entryText = text && *name == ".text.entry";
            if((pass == 0 && !entryText) ||
               (pass == 1 && (!text || entryText)) ||
               (pass == 2 && (!data || text))) continue;

            auto alignment = U64(source.getAlignment().value());
            if(alignment == 0) alignment = 1;

            U64 offset = 0;
            if(!alignUp(U64(image.size()), alignment, offset) ||
               offset > std::numeric_limits<size_t>::max() ||
               source.getSize() > std::numeric_limits<size_t>::max() - size_t(offset)) {
                context.diagnostics.error("llvm: raw image is too large"_v, nullptr);
                return false;
            }

            image.resize(size_t(offset) + size_t(source.getSize()), 0);
            sections.push_back(Section { source, offset });
            sectionOffsets.emplace(source.getIndex(), offset);

            if(source.isBSS() || source.getSize() == 0) continue;
            auto contents = source.getContents();
            if(!contents) {
                auto message = llvm::toString(contents.takeError());
                context.diagnostics.error("llvm: cannot read raw section: %@"_v, nullptr,
                                          diagnosticText(message));
                return false;
            }

            std::memcpy(image.data() + offset, contents->data(), contents->size());
        }
    }

    if(imageBase > std::numeric_limits<U64>::max() - U64(image.size())) {
        context.diagnostics.error("llvm: raw image does not fit at the selected image base"_v, nullptr);
        return false;
    }

    auto writeLittle = [&](U64 site, U64 value, U32 width) -> bool {
        if(site > image.size() || width > image.size() - size_t(site)) return false;
        for(U32 i = 0; i < width; i++) image[size_t(site) + i] = Byte(value >> (i * 8));
        return true;
    };

    for(auto& section: sections) {
        for(auto relocation: section.source.relocations()) {
            auto type = relocation.getType();
            U32 width = 0;
            bool pcRelative = false;
            bool signed32 = false;

            switch(type) {
                case llvm::ELF::R_X86_64_NONE:
                    continue;
                case llvm::ELF::R_X86_64_64:
                    width = 8;
                    break;
                case llvm::ELF::R_X86_64_PC32:
                case llvm::ELF::R_X86_64_PLT32:
                    width = 4;
                    pcRelative = true;
                    signed32 = true;
                    break;
                case llvm::ELF::R_X86_64_32:
                    width = 4;
                    break;
                case llvm::ELF::R_X86_64_32S:
                    width = 4;
                    signed32 = true;
                    break;
                case llvm::ELF::R_X86_64_PC64:
                    width = 8;
                    pcRelative = true;
                    break;
                default: {
                    llvm::SmallVector<char, 32> typeName;
                    relocation.getTypeName(typeName);
                    context.diagnostics.error("llvm: raw output cannot apply x86 relocation %@"_v,
                                              nullptr, StringView { typeName.data(), typeName.size() });
                    return false;
                }
            }

            auto relocationOffset = relocation.getOffset();
            if(relocationOffset > section.source.getSize() ||
               width > section.source.getSize() - size_t(relocationOffset)) {
                context.diagnostics.error("llvm: raw relocation is outside its section"_v, nullptr);
                return false;
            }

            auto site = section.offset + relocationOffset;
            if(site > image.size() || width > image.size() - size_t(site)) {
                context.diagnostics.error("llvm: raw relocation is outside the image"_v, nullptr);
                return false;
            }

            auto symbolIterator = relocation.getSymbol();
            auto symbol = *symbolIterator;
            auto symbolName = symbol.getName();
            if(!symbolName) {
                auto message = llvm::toString(symbolName.takeError());
                context.diagnostics.error("llvm: raw relocation has no symbol: %@"_v, nullptr,
                                          diagnosticText(message));
                return false;
            }

            auto flags = symbol.getFlags();
            if(!flags) {
                auto message = llvm::toString(flags.takeError());
                context.diagnostics.error("llvm: cannot inspect raw relocation symbol %@: %@"_v,
                                          nullptr, StringView { symbolName->data(), symbolName->size() },
                                          diagnosticText(message));
                return false;
            }

            auto value = symbol.getValue();
            if(!value) {
                auto message = llvm::toString(value.takeError());
                context.diagnostics.error("llvm: cannot read raw relocation symbol %@: %@"_v,
                                          nullptr, StringView { symbolName->data(), symbolName->size() },
                                          diagnosticText(message));
                return false;
            }

            U64 target = 0;
            auto targetSection = symbol.getSection();
            if(!targetSection) {
                auto message = llvm::toString(targetSection.takeError());
                context.diagnostics.error("llvm: cannot locate raw relocation symbol %@: %@"_v,
                                          nullptr, StringView { symbolName->data(), symbolName->size() },
                                          diagnosticText(message));
                return false;
            }

            if((*flags) & llvm::object::SymbolRef::SF_Undefined) {
                context.diagnostics.error("llvm: raw image has undefined symbol %@"_v, nullptr,
                                          StringView { symbolName->data(), symbolName->size() });
                return false;
            } else if((*flags) & llvm::object::SymbolRef::SF_Absolute) {
                target = *value;
            } else if(*targetSection != object->section_end()) {
                auto found = sectionOffsets.find((*targetSection)->getIndex());
                if(found == sectionOffsets.end()) {
                    context.diagnostics.error("llvm: raw relocation symbol %@ is in a non-loadable section"_v,
                                              nullptr, StringView { symbolName->data(), symbolName->size() });
                    return false;
                }

                target = imageBase + found->second + *value;
            } else {
                context.diagnostics.error("llvm: raw relocation symbol %@ has no definition"_v,
                                          nullptr, StringView { symbolName->data(), symbolName->size() });
                return false;
            }

            auto addend = llvm::object::ELFRelocationRef(relocation).getAddend();
            if(!addend) {
                auto message = llvm::toString(addend.takeError());
                context.diagnostics.error("llvm: cannot read raw relocation addend: %@"_v, nullptr,
                                          diagnosticText(message));
                return false;
            }

            constexpr unsigned kRelocationBits = 66;
            auto result = llvm::APInt(kRelocationBits, target);
            auto addendValue = llvm::APInt(64, U64(*addend));
            result += *addend < 0 ? addendValue.sext(kRelocationBits)
                                  : addendValue.zext(kRelocationBits);
            if(pcRelative) result -= llvm::APInt(kRelocationBits, imageBase + site);

            if(signed32 && !result.isSignedIntN(32)) {
                context.diagnostics.error("llvm: raw relocation for %@ does not fit a signed 32-bit field"_v,
                                          nullptr, StringView { symbolName->data(), symbolName->size() });
                return false;
            }

            if(width == 4 && !signed32 && !result.isIntN(32)) {
                context.diagnostics.error("llvm: raw relocation for %@ does not fit a 32-bit field"_v,
                                          nullptr, StringView { symbolName->data(), symbolName->size() });
                return false;
            }

            if(!writeLittle(site, result.trunc(width * 8).getZExtValue(), width)) {
                context.diagnostics.error("llvm: cannot write raw relocation for %@"_v, nullptr,
                                          StringView { symbolName->data(), symbolName->size() });
                return false;
            }
        }
    }

    return writeRawImage(context, image.data(), image.size(), outputPath);
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
    /*
     * Not a position-independent executable, because the code was generated as one that is not - see
     * createMachine. The two have to agree: absolute addresses in a PIE are relocations the loader
     * has to apply, and the ones inside prefix data are relocations against code.
     *
     * And nothing of the C runtime where the module brought its own `_start` - see addProcessEntry.
     * `-nostdlib` drops `crt1.o` and `libc`, `-static` drops the interpreter with them, and what is
     * left is a file the kernel maps and jumps into, which is what the other backend produces
     * directly. The link is still `cc` rather than `ld` because the driver is what knows where the
     * linker is.
     */
    auto freestanding = startsItself(context.settings);

    char command[4096];
    auto length = format(toBuffer(command),
                         freestanding ? String("cc -nostdlib -static -no-pie -o \"%@\" \"%@\"")
                                      : String("cc -no-pie -o \"%@\" \"%@\""),
                         outputPath, objectPath);

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
