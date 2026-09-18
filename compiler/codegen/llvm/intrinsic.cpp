#include "build.h"

#include <llvm/Config/llvm-config.h>
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

/*
 * The two routines a stack switch is made of - Analysis-External-Iterators.md §4.1.
 *
 * **Out of line rather than inline assembly**, which is the one place this backend has to state the
 * operation differently from the x64 one. The x64 sequence moves `rsp` in the middle of a function,
 * and that backend can afford to because its allocator is told what the move costs through the
 * convention. An asm block that did the same here would be invisible to every LLVM pass that needs
 * to know: the frame the caller spills into is not the frame the next instruction runs on. A call is
 * the shape LLVM already understands for "control leaves and comes back with the registers gone",
 * so the switch is one, to a routine that takes nothing from the caller's frame.
 *
 *     __yana_stack_switch:           entered by a call, rsp at the return address
 *       push rbp                     the one register the convention keeps
 *       mov  [r13], rsp              the saved stack pointer, where the operand says
 *       mov  rsp, r12                the target's
 *       mov  rax, r14                the transfer word, in the result register
 *       pop  rbp                     the target's frame pointer
 *       pop  rcx                     the address it left from
 *       jmp  rcx                     which is a jump, not a `ret` - see below
 *
 * **`preserve_none` is what makes it correct, not merely fast.** It is the one LLVM convention
 * whose callee keeps nothing but `rsp` and `rbp`, which is exactly what `LowerCallType::StackSwitch`
 * says: the other context runs inside this call, so a register the caller expected back would come
 * back holding the other context's value. Its arguments are r12, r13 and r14 in that order, which
 * is why the routine reads them there rather than from the System V registers. So LLVM spills what
 * is live across the switch, and nothing else - the same bargain the x64 allocator makes, where
 * `Tritium_suspendFiber` saves and restores all seven callee-saved registers on every switch.
 *
 * A saved stack pointer names a saved frame pointer with a resume address above it, which is the
 * same shape the x64 sequence leaves - there the address is pushed, and here the call pushed it.
 * That is what lets `StackInit` lay out a fresh stack the ordinary switch enters: its resume address
 * is the entry routine, which pops what the layout put there and calls it.
 *
 *     __yana_stack_entry:            rsp at top - 24
 *       pop  rdi                     the environment
 *       pop  rsi                     the argument
 *       pop  r11                     the code
 *       call r11                     rsp was at top, so System V's alignment holds at the entry
 *       ud2                          a fiber body ends by switching away, not by returning
 *
 * The entry is called under the internal convention, whose first two arguments `fastcc` places in
 * rdi and rsi - the same assumption `CloneThread`'s child sequence above makes.
 *
 * **An indirect `jmp` rather than `ret`**, as the x64 sequence ends. This used to be a `ret`, on the
 * reasoning that it keeps the return-address predictor's depth right and "mispredicts only the
 * switch itself, which the indirect `jmp` would have done anyway". The last clause was wrong, and it
 * was the whole cost.
 *
 * - **A `ret` here always mispredicts.** It returns to an address in the *other* context, and the
 *   predictor answers with this context's caller.
 * - **An indirect `jmp` is predicted per site**, and the target of a ping-pong between two contexts is
 *   the same every time.
 *
 * Measured 2026-09-15 on a Zen 5, 48M `Iter.next` steps: 62.6 cycles per step with two mispredicts,
 * against 21.7 with none (Analysis-External-Iterators.md's switch-cost note).
 *
 * What the `jmp` gives up is the call's predictor entry, which nothing pops. A real `ret` in the
 * resumed context - returning from a function that was active when that context last switched - can
 * mispredict against it. A loop with everything between two switches inlined makes no such return,
 * and measured no mispredicts. `rcx` carries the address because the convention keeps nothing, and
 * `r11` is the entry routine's.
 *
 * Module assembly rather than a naked function, because the routine's arguments are registers and
 * nothing else: a definition LLVM could see is one whose unused arguments dead-argument elimination
 * and interprocedural propagation would be entitled to rewrite. Local symbols, so a program linked
 * from one module has no second definition to collide with.
 */
static bool stackRuntimeOf(Gen& gen, LocationId source) {
    if(gen.stackSwitch) return true;

    if(gen.context.settings.arch != TargetArch::X64) {
        gen.context.diagnostics.error("llvm: stack switching is implemented for x86-64 only"_v, source);
        return false;
    }

#if LLVM_VERSION_MAJOR < 19
    gen.context.diagnostics.error("llvm: stack switching needs the preserve_none convention of LLVM 19 or later"_v,
                                  source);
    return false;
#else
    auto word = llvm::Type::getInt64Ty(gen.llvm);
    auto pointer = llvm::PointerType::get(gen.llvm, 0);

    gen.stackSwitch = llvm::Function::Create(
        llvm::FunctionType::get(word, { pointer, pointer, word }, false),
        llvm::Function::ExternalLinkage, "__yana_stack_switch", gen.module);
    gen.stackSwitch->setCallingConv(llvm::CallingConv::PreserveNone);

    gen.stackEntry = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(gen.llvm), false),
        llvm::Function::ExternalLinkage, "__yana_stack_entry", gen.module);

    // The symbols as the assembler sees them: Mach-O prefixes an underscore, and the data layout is
    // what says so. `.type` and `.size` only mean something to ELF.
    auto prefix = gen.module.getDataLayout().getGlobalPrefix();
    // Raw output is flattened from an ELF object as well, so its module assembly still needs the
    // ELF-only type and size directives. The output format is raw; the intermediate object format
    // is what matters to the assembler here.
    auto elf = gen.context.settings.format == ExecutableFormat::ELF ||
               gen.context.settings.format == ExecutableFormat::Raw;

    auto routine = [&](const char* name, const char* body) {
        std::string symbol;
        if(prefix) symbol += prefix;
        symbol += name;

        std::string text = "\t.text\n\t.p2align 4\n";
        if(elf) text += "\t.type " + symbol + ", @function\n";
        text += symbol + ":\n";
        text += body;
        if(elf) text += "\t.size " + symbol + ", . - " + symbol + "\n";

        gen.module.appendModuleInlineAsm(text);
    };

    routine("__yana_stack_switch",
            "\tpushq %rbp\n"
            "\tmovq %rsp, (%r13)\n"
            "\tmovq %r12, %rsp\n"
            "\tmovq %r14, %rax\n"
            "\tpopq %rbp\n"
            "\tpopq %rcx\n"
            "\tjmpq *%rcx\n");

    routine("__yana_stack_entry",
            "\tpopq %rdi\n"
            "\tpopq %rsi\n"
            "\tpopq %r11\n"
            "\tcallq *%r11\n"
            "\tud2\n");

    return true;
#endif
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
        case LowerIntrinsic::Nop:
            genAsm(f, inst, "nop", "~{memory}", {});
            return true;

        case LowerIntrinsic::Lretq:
            {
                // The native emitter uses MOVZX EAX, r/m16. Keep the input 16-bit here too, so
                // LLVM names the source register's low word in the inline assembly and the
                // zero-extension is explicit rather than relying on the source type's lowering.
                auto selector = asInt16(f, argOf(f, inst, 0));
                llvm::Value* args[] = { selector };
                genAsm(f, inst,
                       "pushq %rax\n\t"
                       "movzwl $0, %eax\n\t"
                       "pushq %rax\n\t"
                       "leaq 3f(%rip), %rax\n\t"
                       "pushq %rax\n\t"
                       "lretq\n"
                       "3:\n\t"
                       "popq %rax",
                       "r,~{memory}", args);
            }
            return true;

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

        // The two lists spelled out, because `{}` for both is ambiguous since LLVM 20 grew an
        // overload taking the result type first.
        case LowerIntrinsic::MFence:
            f.builder.CreateIntrinsic(llvm::Intrinsic::x86_sse2_mfence, llvm::ArrayRef<llvm::Type*>(),
                                      llvm::ArrayRef<llvm::Value*>());
            return true;
        case LowerIntrinsic::LFence:
            f.builder.CreateIntrinsic(llvm::Intrinsic::x86_sse2_lfence, llvm::ArrayRef<llvm::Type*>(),
                                      llvm::ArrayRef<llvm::Value*>());
            return true;
        case LowerIntrinsic::SFence:
            f.builder.CreateIntrinsic(llvm::Intrinsic::x86_sse_sfence, llvm::ArrayRef<llvm::Type*>(),
                                      llvm::ArrayRef<llvm::Value*>());
            return true;
        case LowerIntrinsic::Pause:
            f.builder.CreateIntrinsic(llvm::Intrinsic::x86_sse2_pause, llvm::ArrayRef<llvm::Type*>(),
                                      llvm::ArrayRef<llvm::Value*>());
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

        case LowerIntrinsic::In16: {
            llvm::Value* args[] = { asInt16(f, argOf(f, inst, 0)) };
            auto type = llvm::FunctionType::get(f.builder.getInt16Ty(), { args[0]->getType() }, false);
            auto assembly = llvm::InlineAsm::get(type, "inw %dx, %ax", "={ax},{dx}", true);
            f.define(created[0], f.builder.CreateZExt(f.builder.CreateCall(assembly, args),
                                                      typeOf(f.gen, created[0].type)));
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

        case LowerIntrinsic::Out16: {
            llvm::Value* args[] = { asInt16(f, argOf(f, inst, 0)), asInt16(f, argOf(f, inst, 1)) };
            genAsm(f, inst, "outw %ax, %dx", "{dx},{ax}", args);
            return true;
        }

        case LowerIntrinsic::Ins32: {
            llvm::Value* args[] = {
                asInt32(f, argOf(f, inst, 0)), argOf(f, inst, 1), asInt32(f, argOf(f, inst, 2)),
            };
            genAsm(f, inst, "rep insl", "{edx},{rdi},{ecx},~{rdi},~{rcx},~{memory},~{dirflag}", args);
            return true;
        }

        case LowerIntrinsic::Outs32: {
            llvm::Value* args[] = {
                asInt32(f, argOf(f, inst, 0)), argOf(f, inst, 1), asInt32(f, argOf(f, inst, 2)),
            };
            genAsm(f, inst, "rep outsl", "{edx},{rsi},{ecx},~{rsi},~{rcx},~{memory},~{dirflag}", args);
            return true;
        }

        case LowerIntrinsic::Interrupt: {
            llvm::Value* args[] = { argOf(f, inst, 0) };
            genAsm(f, inst, "int $0", "i", args);
            return true;
        }

        case LowerIntrinsic::Lidt:
            genSingleOperand(f, inst, "lidt ($0)", "r,~{memory}");
            return true;
        case LowerIntrinsic::Lgdt:
            genSingleOperand(f, inst, "lgdt ($0)", "r,~{memory}");
            return true;

        case LowerIntrinsic::MovDs:
            {
                llvm::Value* args[] = { asInt16(f, argOf(f, inst, 0)) };
                genAsm(f, inst, "mov $0, %ds", "r,~{memory}", args);
            }
            return true;
        case LowerIntrinsic::MovEs:
            {
                llvm::Value* args[] = { asInt16(f, argOf(f, inst, 0)) };
                genAsm(f, inst, "mov $0, %es", "r,~{memory}", args);
            }
            return true;
        case LowerIntrinsic::MovFs:
            {
                llvm::Value* args[] = { asInt16(f, argOf(f, inst, 0)) };
                genAsm(f, inst, "mov $0, %fs", "r,~{memory}", args);
            }
            return true;
        case LowerIntrinsic::MovGs:
            {
                llvm::Value* args[] = { asInt16(f, argOf(f, inst, 0)) };
                genAsm(f, inst, "mov $0, %gs", "r,~{memory}", args);
            }
            return true;
        case LowerIntrinsic::MovSs:
            {
                llvm::Value* args[] = { asInt16(f, argOf(f, inst, 0)) };
                genAsm(f, inst, "mov $0, %ss", "r,~{memory}", args);
            }
            return true;

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

        /*
         * Preparing a stack for its first switch - see PseudoKind::StackInit for the layout, which
         * is the same one here word for word.
         *
         * Five stores and an address rather than inline assembly, which is the one way this differs
         * from the x64 sequence. What the x64 backend has to branch over inline is the entry stub,
         * and here the stub is a routine of its own beside the switch (`stackRuntimeOf`), so what
         * is left is only writes through a pointer - which LLVM can see, and has no reason to think
         * are dead: the address they are below is the result, and it goes on to be a switch target.
         *
         * The word at `top - 32` is the stub's address and the one at `top - 40` a zero frame
         * pointer, exactly what the switch routine leaves when it saves a stack: its `ret` takes the
         * first as a resume address, so a context's first resume is an ordinary one here as well.
         */
        case LowerIntrinsic::StackInit: {
            if(!stackRuntimeOf(f.gen, inst.source)) return false;

            auto word = llvm::Type::getInt64Ty(f.gen.llvm);
            auto pointer = llvm::PointerType::get(f.gen.llvm, 0);

            auto asAddress = [&](llvm::Value* value) -> llvm::Value* {
                return value->getType()->isPointerTy() ? value : f.builder.CreateIntToPtr(value, pointer);
            };

            // A word is written whole whatever the operand's width, so that nothing above a
            // narrower value is left holding what the mapping held before.
            auto asWord = [&](llvm::Value* value) -> llvm::Value* {
                return value->getType()->isPointerTy() ? value : f.builder.CreateZExtOrTrunc(value, word);
            };

            auto top = asAddress(argOf(f, inst, 0));
            auto below = [&](I64 bytes) {
                return f.builder.CreateGEP(f.builder.getInt8Ty(), top, llvm::ConstantInt::getSigned(word, -bytes));
            };

            // `(stackTop, code, environment, argument)`, laid out as the stub pops them: the
            // environment first, then the argument, then the code.
            f.builder.CreateAlignedStore(asWord(argOf(f, inst, 1)), below(8), llvm::Align(8));
            f.builder.CreateAlignedStore(asWord(argOf(f, inst, 3)), below(16), llvm::Align(8));
            f.builder.CreateAlignedStore(asWord(argOf(f, inst, 2)), below(24), llvm::Align(8));
            f.builder.CreateAlignedStore(f.gen.stackEntry, below(32), llvm::Align(8));
            f.builder.CreateAlignedStore(llvm::ConstantInt::get(word, 0), below(40), llvm::Align(8));

            llvm::Value* saved = below(40);
            if(created.length == 1 && !isPtr(created[0].type)) {
                saved = f.builder.CreatePtrToInt(saved, typeOf(f.gen, created[0].type));
            }

            defineResults(f, inst, saved);
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

/*
 * The stack switch - `(target, save, transfer) -> transfer`, a call to the routine `stackRuntimeOf`
 * describes. The operands are coerced to the routine's three words, since the IR may state an
 * address as an integer where the library did arithmetic on it.
 */
llvm::Value* genStackSwitch(FunGen& f, LowerInstCall& inst) {
    auto used = inst.used();
    auto created = inst.created();
    auto word = llvm::Type::getInt64Ty(f.gen.llvm);
    auto pointer = llvm::PointerType::get(f.gen.llvm, 0);

    if(used.length != 3) {
        f.context.diagnostics.error("llvm: a stack switch takes a target, a save slot and a transfer word"_v,
                                    inst.source);
        return llvm::PoisonValue::get(word);
    }

    if(!stackRuntimeOf(f.gen, inst.source)) return llvm::PoisonValue::get(word);

    llvm::Value* args[3];
    for(Size i = 0; i < 3; i++) {
        auto value = f.use(used[i]);
        if(!value) {
            f.context.diagnostics.error("llvm: a stack switch uses a value that was never defined"_v, inst.source);
            return llvm::PoisonValue::get(word);
        }

        auto wanted = i < 2 ? (llvm::Type*)pointer : word;
        if(value->getType() == wanted) {
            args[i] = value;
        } else if(value->getType()->isPointerTy()) {
            args[i] = f.builder.CreatePtrToInt(value, word);
        } else if(wanted->isPointerTy()) {
            args[i] = f.builder.CreateIntToPtr(value, pointer);
        } else {
            args[i] = f.builder.CreateZExtOrTrunc(value, word);
        }
    }

    auto call = f.builder.CreateCall(f.gen.stackSwitch, args);
    call->setCallingConv(llvm::CallingConv::PreserveNone);

    llvm::Value* result = call;
    if(created.length == 1 && created[0].type != LowerType::Int64) {
        result = isPtr(created[0].type)
            ? f.builder.CreateIntToPtr(result, typeOf(f.gen, created[0].type))
            : f.builder.CreateIntCast(result, typeOf(f.gen, created[0].type), false);
    }

    return result;
}

} // namespace llvmgen
