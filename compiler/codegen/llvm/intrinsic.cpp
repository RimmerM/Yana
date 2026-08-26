#include "build.h"

#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IntrinsicsX86.h>

#include <string>

/*
 * The operations named directly rather than described.
 *
 * An intrinsic means "emit this machine operation", so the question here is only whether LLVM has a
 * name for the same operation. Where it does - `ctpop`, the fences, `prefetch` - the
 * intrinsic is used, because LLVM knows what those do and can move, fold and select them. Where it
 * does not, the operation is inline assembly with the operands pinned to the registers the encoding
 * demands, which is the same statement the x64 registry makes as fixed-register constraints (see
 * codegen/x64/intrinsic.cpp) - written in the only vocabulary LLVM has for it.
 *
 * Every one of them is marked as having side effects, including the ones that only read: an
 * intrinsic is in the program because the program asked for it there, and an `rdtsc` that LLVM
 * hoisted out of the loop being timed would be a measurement of nothing.
 */
namespace llvmgen {

static llvm::Value* argOf(FunGen& f, LowerInstIntrinsic& inst, Size index) {
    auto used = inst.used();
    if(auto value = f.use(used[index])) return value;

    f.context.diagnostics.error("llvm: an intrinsic uses a value that was never defined"_v, inst.source);
    return llvm::PoisonValue::get(typeOf(f.gen, f.base[used[index]]->type));
}

// The result types the intrinsic states, as the anonymous struct several outputs come back in.
static llvm::FunctionType* asmTypeOf(FunGen& f, LowerInstIntrinsic& inst, llvm::ArrayRef<llvm::Value*> args) {
    llvm::SmallVector<llvm::Type*, 4> results;
    llvm::SmallVector<llvm::Type*, 4> argTypes;

    for(auto& created: inst.created()) results.push_back(typeOf(f.gen, created.type));
    for(auto arg: args) argTypes.push_back(arg->getType());

    return llvm::FunctionType::get(resultTypeOf(f.gen, results), argTypes, false);
}

static void genAsm(FunGen& f, LowerInstIntrinsic& inst, const char* text, const char* constraints,
                   llvm::ArrayRef<llvm::Value*> args)
{
    auto assembly = llvm::InlineAsm::get(asmTypeOf(f, inst, args), text, constraints, true);
    defineResults(f, inst, f.builder.CreateCall(assembly, args));
}

// The four-byte forms every port and register intrinsic works at - see integer32Rule in the x64
// registry. Stated here as well because inline assembly picks the register *view* from the operand
// type: an i32 in `{ax}` is eax, and an i16 in the same place is ax.
static llvm::Value* asInt32(FunGen& f, llvm::Value* value) {
    return f.builder.CreateZExtOrTrunc(value, llvm::Type::getInt32Ty(f.gen.llvm));
}

static llvm::Value* asInt16(FunGen& f, llvm::Value* value) {
    return f.builder.CreateZExtOrTrunc(value, llvm::Type::getInt16Ty(f.gen.llvm));
}

// An operation with one operand and no result: an address to act on, or a value to write.
static void genSingleOperand(FunGen& f, LowerInstIntrinsic& inst, const char* text, const char* constraints) {
    llvm::Value* args[] = { argOf(f, inst, 0) };
    genAsm(f, inst, text, constraints, args);
}

// One of the two halves the machine hands a 64-bit register back in, joined.
static llvm::Value* joinHalves(FunGen& f, llvm::Value* pair, llvm::Type* type) {
    auto low = f.builder.CreateZExt(f.builder.CreateExtractValue(pair, 0), type);
    auto high = f.builder.CreateZExt(f.builder.CreateExtractValue(pair, 1), type);

    return f.builder.CreateOr(low, f.builder.CreateShl(high, llvm::ConstantInt::get(type, 32)));
}

bool genIntrinsic(FunGen& f, LowerInstIntrinsic& inst) {
    auto created = inst.created();

    switch(inst.getIntrinsic()) {
        case LowerIntrinsic::Popcnt:
            defineResults(f, inst, f.builder.CreateUnaryIntrinsic(llvm::Intrinsic::ctpop, argOf(f, inst, 0)));
            return true;

        /*
         * `llvm.cttz` takes a second argument saying whether a zero operand is poison, and the two
         * kinds here are exactly that argument: `Cttz` promises nothing at zero and selects the bare
         * bit scan, `CttzWidth` promises the operand's width and selects `tzcnt` where the target has
         * it and the scan plus a correction where it does not.
         *
         * Which one a caller wants is not this backend's guess to make, which is the argument for
         * two kinds rather than one and a flag read off the target.
         */
        case LowerIntrinsic::Cttz:
        case LowerIntrinsic::CttzWidth: {
            auto poisonZero = inst.getIntrinsic() == LowerIntrinsic::Cttz;
            auto operand = argOf(f, inst, 0);
            llvm::Value* args[] = { operand, poisonZero ? f.builder.getTrue() : f.builder.getFalse() };

            defineResults(f, inst, f.builder.CreateIntrinsic(llvm::Intrinsic::cttz,
                                                             { operand->getType() }, args));
            return true;
        }

        /*
         * The leading count, which is `cttz`'s case at the other end of the word and takes the same
         * flag: the language's `leadingZeros` answers the operand's width at zero, so the zero
         * operand is not poison and `llvm.ctlz` is told so. What the target does with that - one
         * `lzcnt`, or a `bsr` and a correction - is its own lowering, which is exactly the division
         * of labour the x64 backend has to make for itself in `expandBitScans`.
         */
        case LowerIntrinsic::ClzWidth: {
            auto operand = argOf(f, inst, 0);
            llvm::Value* args[] = { operand, f.builder.getFalse() };

            defineResults(f, inst, f.builder.CreateIntrinsic(llvm::Intrinsic::ctlz,
                                                             { operand->getType() }, args));
            return true;
        }

        /*
         * Written by the x64 backend for itself and never by anything above one - see the note on
         * the kind, and `LowerReduce::Bits`, which is here for the same reason and refused the same
         * way. Expanding it would mean choosing what an out-of-range index does, and the whole point
         * of the kind is that x86's answer to that is the one being spent.
         *
         * `Bsr` joins them for the same reason at a different operation: it answers an *index*, is
         * undefined at zero, and exists so that `expandBitScans` has a name for the instruction the
         * x64 baseline has. Nothing above a backend writes one, so nothing here has to lower one.
         */
        case LowerIntrinsic::Bsr:
        case LowerIntrinsic::Bzhi:
        case LowerIntrinsic::Pext:
        case LowerIntrinsic::Pdep:
            f.context.diagnostics.error("llvm: a target-lowered intrinsic reached the LLVM backend"_v,
                                        inst.source);
            return true;

        case LowerIntrinsic::Cpuid: {
            llvm::Value* args[] = { asInt32(f, argOf(f, inst, 0)), asInt32(f, argOf(f, inst, 1)) };

            // ebx is the one the compiler cannot simply be told about: it is the PIC base register
            // in 32-bit code, which is why hand-written cpuid sequences exchange it. On AMD64 it is
            // an ordinary callee-saved register and naming it as an output is enough.
            genAsm(f, inst, "cpuid", "={ax},={bx},={cx},={dx},{ax},{cx}", args);
            return true;
        }

        case LowerIntrinsic::Rdtscp:
            genAsm(f, inst, "rdtscp", "={ax},={dx},~{rcx}", {});
            return true;

        case LowerIntrinsic::Rdtsc: {
            // The machine answers in edx:eax whichever of the two the IR asked for, so the single
            // 64-bit form is the pair plus the arithmetic that joins it - which is exactly what the
            // x64 backend's pseudo does with the same two registers.
            auto type = typeOf(f.gen, created[0].type);
            auto pair = llvm::StructType::get(f.builder.getInt32Ty(), f.builder.getInt32Ty());
            auto assembly = llvm::InlineAsm::get(llvm::FunctionType::get(pair, false), "rdtsc", "={ax},={dx}", true);
            auto counter = f.builder.CreateCall(assembly);

            f.define(created[0], joinHalves(f, counter, type));
            return true;
        }

        case LowerIntrinsic::MFence:
            f.builder.CreateIntrinsic(llvm::Intrinsic::x86_sse2_mfence, {}, {});
            return true;
        case LowerIntrinsic::LFence:
            f.builder.CreateIntrinsic(llvm::Intrinsic::x86_sse2_lfence, {}, {});
            return true;
        case LowerIntrinsic::SFence:
            f.builder.CreateIntrinsic(llvm::Intrinsic::x86_sse_sfence, {}, {});
            return true;
        case LowerIntrinsic::Pause:
            f.builder.CreateIntrinsic(llvm::Intrinsic::x86_sse2_pause, {}, {});
            return true;

        case LowerIntrinsic::Prefetch:
        case LowerIntrinsic::PrefetchNta: {
            auto nta = inst.getIntrinsic() == LowerIntrinsic::PrefetchNta;
            llvm::Value* args[] = {
                argOf(f, inst, 0),
                f.builder.getInt32(0),          // read rather than write
                f.builder.getInt32(nta ? 0 : 3), // locality: none, or all levels
                f.builder.getInt32(1),          // the data cache rather than the instruction cache
            };

            f.builder.CreateIntrinsic(llvm::Intrinsic::prefetch, { args[0]->getType() }, args);
            return true;
        }

        case LowerIntrinsic::Clflush:
            f.builder.CreateIntrinsic(llvm::Intrinsic::x86_sse2_clflush, {}, { argOf(f, inst, 0) });
            return true;

        case LowerIntrinsic::Invlpg:
            genSingleOperand(f, inst, "invlpg ($0)", "r,~{memory}");
            return true;

        case LowerIntrinsic::Hlt:
            genAsm(f, inst, "hlt", "~{memory}", {});
            return true;
        case LowerIntrinsic::Cli:
            genAsm(f, inst, "cli", "~{memory}", {});
            return true;
        case LowerIntrinsic::Sti:
            genAsm(f, inst, "sti", "~{memory}", {});
            return true;
        case LowerIntrinsic::Swapgs:
            genAsm(f, inst, "swapgs", "~{memory}", {});
            return true;

        case LowerIntrinsic::Rdmsr: {
            llvm::Value* args[] = { asInt32(f, argOf(f, inst, 0)) };
            genAsm(f, inst, "rdmsr", "={ax},={dx},{cx}", args);
            return true;
        }

        case LowerIntrinsic::Wrmsr: {
            llvm::Value* args[] = {
                asInt32(f, argOf(f, inst, 0)), asInt32(f, argOf(f, inst, 1)), asInt32(f, argOf(f, inst, 2)),
            };

            genAsm(f, inst, "wrmsr", "{cx},{ax},{dx},~{memory}", args);
            return true;
        }

        case LowerIntrinsic::Xgetbv: {
            llvm::Value* args[] = { asInt32(f, argOf(f, inst, 0)) };
            genAsm(f, inst, "xgetbv", "={ax},={dx},{cx}", args);
            return true;
        }

        case LowerIntrinsic::In8: {
            // `in al, dx` writes only al and leaves the rest of the register alone, so the byte is
            // taken as a byte and widened - which is what the x64 backend's PortIn8 pseudo does.
            llvm::Value* args[] = { asInt16(f, argOf(f, inst, 0)) };
            auto type = llvm::FunctionType::get(f.builder.getInt8Ty(), { args[0]->getType() }, false);
            auto assembly = llvm::InlineAsm::get(type, "inb %dx, %al", "={ax},{dx}", true);

            f.define(created[0], f.builder.CreateZExt(f.builder.CreateCall(assembly, args),
                                                     typeOf(f.gen, created[0].type)));
            return true;
        }

        case LowerIntrinsic::In32: {
            llvm::Value* args[] = { asInt16(f, argOf(f, inst, 0)) };
            genAsm(f, inst, "inl %dx, %eax", "={ax},{dx}", args);
            return true;
        }

        case LowerIntrinsic::Out8: {
            // The port first and the value second, as the IR states them.
            llvm::Value* args[] = {
                asInt16(f, argOf(f, inst, 0)),
                f.builder.CreateZExtOrTrunc(argOf(f, inst, 1), f.builder.getInt8Ty()),
            };

            genAsm(f, inst, "outb %al, %dx", "{dx},{ax}", args);
            return true;
        }

        case LowerIntrinsic::Out32: {
            llvm::Value* args[] = { asInt16(f, argOf(f, inst, 0)), asInt32(f, argOf(f, inst, 1)) };
            genAsm(f, inst, "outl %eax, %dx", "{dx},{ax}", args);
            return true;
        }

        /*
         * Control registers. One intrinsic per register because the number is part of the encoding,
         * which is also why each of these is its own assembly string.
         */

        case LowerIntrinsic::ReadCr0:
            genAsm(f, inst, "mov %cr0, $0", "=r", {});
            return true;
        case LowerIntrinsic::ReadCr2:
            genAsm(f, inst, "mov %cr2, $0", "=r", {});
            return true;
        case LowerIntrinsic::ReadCr3:
            genAsm(f, inst, "mov %cr3, $0", "=r", {});
            return true;
        case LowerIntrinsic::ReadCr4:
            genAsm(f, inst, "mov %cr4, $0", "=r", {});
            return true;

        case LowerIntrinsic::WriteCr0:
            genSingleOperand(f, inst, "mov $0, %cr0", "r,~{memory}");
            return true;
        case LowerIntrinsic::WriteCr3:
            genSingleOperand(f, inst, "mov $0, %cr3", "r,~{memory}");
            return true;
        case LowerIntrinsic::WriteCr4:
            genSingleOperand(f, inst, "mov $0, %cr4", "r,~{memory}");
            return true;

        /*
         * Starting a thread - the same sequence the x64 backend encodes by hand, written as inline
         * assembly for the reason the system call above it is: the callee is the kernel, and what
         * comes back is two threads rather than a value.
         *
         * It has to be one asm block and not a call around a smaller one, because the whole point is
         * that no compiler-generated instruction may stand between the system call and the child's
         * first `pop` - see PseudoKind::CloneThread. An asm block is opaque to LLVM, so nothing is
         * scheduled into the middle of this one.
         *
         * The operands are named by register rather than left to the allocator, because three of
         * them are the kernel's own positions and the other two are read back off the child's stack
         * in an order this text fixes.
         */
        case LowerIntrinsic::CloneThread: {
            auto used = inst.used();
            auto word = llvm::Type::getInt64Ty(f.gen.llvm);

            llvm::SmallVector<llvm::Value*, 6> args;
            llvm::SmallVector<llvm::Type*, 6> argTypes;

            // In the order the constraint string names them: flags, stack top, code, argument,
            // thread id, environment.
            static const char* kConstraints =
                "={rax},{rdi},{rsi},{rdx},{r8},{r10},{r9}"
                ",~{rcx},~{r11},~{memory},~{dirflag},~{fpsr},~{flags}";

            for(Size i = 0; i < 6; i++) {
                auto value = f.use(used[i]);
                if(value->getType()->isPointerTy()) value = f.builder.CreatePtrToInt(value, word);
                else if(value->getType() != word) value = f.builder.CreateIntCast(value, word, true);

                args.push_back(value);
                argTypes.push_back(word);
            }

            auto text =
                "movq %r9, -24(%rsi)\n\t"
                "movq %r8, -16(%rsi)\n\t"
                "movq %rdx, -8(%rsi)\n\t"
                "leaq -24(%rsi), %rsi\n\t"
                "movq %r10, %rdx\n\t"
                "xorl %r8d, %r8d\n\t"
                "movl $$56, %eax\n\t"
                "syscall\n\t"
                "testq %rax, %rax\n\t"
                "jnz 1f\n\t"
                "popq %rdi\n\t"
                "popq %rsi\n\t"
                "popq %rax\n\t"
                "call *%rax\n\t"
                "xorl %edi, %edi\n\t"
                "movl $$60, %eax\n\t"
                "syscall\n\t"
                "1:";

            auto assembly = llvm::InlineAsm::get(llvm::FunctionType::get(word, argTypes, false),
                                                 text, kConstraints, true);

            llvm::Value* result = f.builder.CreateCall(assembly, args);
            auto created = inst.created();

            if(created.length == 1 && created[0].type != LowerType::Int64) {
                result = f.builder.CreateIntCast(result, typeOf(f.gen, created[0].type), true);
            }

            defineResults(f, inst, result);
            return true;
        }
    }

    f.context.diagnostics.error("llvm: unknown intrinsic"_v, inst.source);
    return false;
}

/*
 * System calls.
 *
 * Inline assembly for the same reason the x64 backend gives it a form of its own: the callee is the
 * kernel, its convention is not one LLVM can be told about as a calling convention, and the number
 * is an operand rather than an address. The clobbers are the ABI's - rcx and r11 hold the return
 * address and flags across `syscall` - plus memory, since what a system call does to the program's
 * memory is not something the compiler can see.
 */
llvm::Value* genSyscall(FunGen& f, LowerInstCall& inst) {
    static const char* kArgumentRegisters[] = { "{rdi}", "{rsi}", "{rdx}", "{r10}", "{r8}", "{r9}" };
    static constexpr Size kMaxArguments = sizeof(kArgumentRegisters) / sizeof(const char*);

    auto used = inst.used();
    auto created = inst.created();
    auto argCount = used.length - 1;
    auto word = llvm::Type::getInt64Ty(f.gen.llvm);

    if(argCount > kMaxArguments) {
        f.context.diagnostics.error("llvm: a system call takes at most %@ arguments"_v, inst.source, kMaxArguments);
        return llvm::PoisonValue::get(word);
    }

    // Every operand is passed as a full register: the kernel reads all 64 bits of each, and a
    // narrower value would leave the rest of the register holding whatever it held. Signed, because
    // a negative argument is an ordinary one - the -1 file descriptor an anonymous mapping passes.
    auto widen = [&](LowerPtr<LowerValue> value) -> llvm::Value* {
        auto built = f.use(value);
        if(!built) {
            f.context.diagnostics.error("llvm: a system call uses a value that was never defined"_v, inst.source);
            return llvm::PoisonValue::get(word);
        }

        if(built->getType()->isPointerTy()) return f.builder.CreatePtrToInt(built, word);
        return f.builder.CreateSExtOrTrunc(built, word);
    };

    std::string constraints;
    llvm::SmallVector<llvm::Value*, 8> args;
    llvm::SmallVector<llvm::Type*, 8> argTypes;

    if(created.length > 0) constraints += "={rax},";
    constraints += "{rax}";
    args.push_back(widen(used[0]));

    for(Size i = 0; i < argCount; i++) {
        constraints += ",";
        constraints += kArgumentRegisters[i];
        args.push_back(widen(used[i + 1]));
    }

    constraints += ",~{rcx},~{r11},~{memory},~{dirflag},~{fpsr},~{flags}";
    for(auto arg: args) argTypes.push_back(arg->getType());

    auto resultType = created.length > 0 ? word : llvm::Type::getVoidTy(f.gen.llvm);
    auto assembly = llvm::InlineAsm::get(llvm::FunctionType::get(resultType, argTypes, false),
                                         "syscall", constraints, true);
    llvm::Value* result = f.builder.CreateCall(assembly, args);

    // The kernel answers in rax, and the IR may have asked for that as something narrower or as an
    // address - `mmap` returns a pointer here and a signed error code there.
    if(created.length == 1 && created[0].type != LowerType::Int64) {
        result = isPtr(created[0].type)
            ? f.builder.CreateIntToPtr(result, typeOf(f.gen, created[0].type))
            : f.builder.CreateIntCast(result, typeOf(f.gen, created[0].type), true);
    }

    return result;
}

} // namespace llvmgen
