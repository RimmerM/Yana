#include "machine.h"
#include "x64_util.h"

/*
 * The intrinsic registry.
 *
 * An intrinsic is a machine operation the program named rather than one the lowering derived, and
 * this is the whole of what the backend knows about each one: a machine form like any other
 * instruction's, the features its encoding needs, what its operands and results have to be, and what
 * it does beyond them.
 *
 * What is worth reading here is what is *not* anywhere else. Each shape below demands something
 * different of the allocator, and none of them appears by name in placement, in legalization or in
 * the encoder:
 *
 *   popcnt   reads one operand out of a frame slot where the allocator left it there, exactly as an
 *            `add` does, because it says so with the same constraint.
 *   cpuid    forces two operands into fixed registers and produces four results in fixed registers,
 *            one of which is callee-saved and so drags a save into the prologue.
 *   rdtscp   writes a register it does not name as a result at all, which is a clobber like a call's.
 *   mfence   has no operands and no results, and exists entirely for its effect on everything else.
 *   prefetch takes an *address*, which the address folding rewrites into a folded addressing mode
 *            exactly as it does a load's, because the form says the operand is one.
 *   readcr3  puts the number of the register it reads in the encoding's opcode extension, which is
 *            why there is one intrinsic per control register rather than one taking a number.
 *   rdtsc    is three instructions rather than one, and the only thing that makes expanding it
 *            after allocation safe is that the form already declared the register and the flags it
 *            uses on the way (§15.2 of the plan).
 *
 * Adding another is a block below and a name in the IR's own table (lower.cpp). If it ever needs
 * more than that, the thing to change is the descriptor, not the pass that noticed it was missing.
 */

static IntrinsicOperandRule integerRule() {
    return IntrinsicOperandRule { IntrinsicOperandClass::Integer };
}

static IntrinsicOperandRule integer32Rule() {
    return IntrinsicOperandRule { IntrinsicOperandClass::Integer32 };
}

static IntrinsicOperandRule integer64Rule() {
    return IntrinsicOperandRule { IntrinsicOperandClass::Integer64 };
}

static IntrinsicOperandRule pointerRule() {
    return IntrinsicOperandRule { IntrinsicOperandClass::Pointer };
}

// One intrinsic being added: its form and its descriptor, which are written together because they
// are two halves of one row.
struct IntrinsicBuilder {
    MachineForm& form;
    IntrinsicDescriptor& desc;
};

void addIntrinsics(MachineTarget& target) {
    auto add = [&](LowerIntrinsic id, StringView formName, FeatureSet features) {
        auto& ir = lowerIntrinsicDesc(id);
        auto opcode = opcodeForIntrinsic(id);

        // An intrinsic's machine opcode is named after the intrinsic, so that a form table dump and
        // a verifier message say the thing the program wrote.
        target.opcodes[opcode].id = opcode;
        target.opcodes[opcode].name = ir.name;

        target.forms.push(MachineForm {});

        auto& form = target.forms[target.forms.size() - 1];
        form.id = MachineFormId(target.forms.size() - 1);
        form.opcode = opcode;
        form.name = formName;
        form.requiredFeatures = features;

        auto& desc = target.intrinsics[Size(id)];
        desc.id = id;
        desc.opcode = opcode;
        desc.form = form.id;
        desc.requiredFeatures = features;
        desc.defined = true;

        return IntrinsicBuilder { form, desc };
    };

    {
        // POPCNT r, r/m (f3 0f b8) counts the set bits of its operand into a register that need not
        // be the operand's. The r/m side is the ordinary memory alternative: a source the allocator
        // left in the frame is read there, with no reload, because this row says so in the same
        // words `add` does - and nothing in placement or legalization had to learn the name.
        auto b = add(LowerIntrinsic::Popcnt, "popcnt r, r/m"_v, kFeatureBaseline);
        b.form.uses.push(regOrMem(MemoryAccessKind::Read));
        b.form.defs.push(def());
        b.form.flagsEffect = FlagsEffect::Def;
        b.form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = 0xb8, .escape = 0x0f, .prefix = 0xf3,
            .regField = defRef(0), .rmField = useRef(0),
            .width = OperationWidth::FromUse0,
        };

        b.desc.operands.push(integerRule());
        b.desc.results.push(integerRule());
    }

    {
        /*
         * BSF r, r/m (0f bc) answers the index of the lowest set bit of its operand, in a register
         * that need not be the operand's - the same shape POPCNT has above, and the memory
         * alternative is there for the same reason.
         *
         * **Baseline, and undefined at zero.** TZCNT is this instruction with an `f3` prefix and a
         * defined answer for zero (the operand's width), but it is BMI1 and this target describes no
         * such level; a processor without it decodes the prefix as BSF and silently leaves the
         * destination alone, which is the one way for a feature to be wrong that no diagnostic
         * catches. So the row is BSF, the IR kind says the zero case is undefined, and every emitter
         * here hands it an operand that cannot be zero.
         *
         * ZF is set exactly when the operand was zero, which nothing reads: the flags effect is
         * declared so that the window a comparison's flags survive in knows this writes them.
         */
        auto b = add(LowerIntrinsic::Cttz, "bsf r, r/m"_v, kFeatureBaseline);
        b.form.uses.push(regOrMem(MemoryAccessKind::Read));
        b.form.defs.push(def());
        b.form.flagsEffect = FlagsEffect::Def;
        b.form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = 0xbc, .escape = 0x0f,
            .regField = defRef(0), .rmField = useRef(0),
            .width = OperationWidth::FromUse0,
        };

        b.desc.operands.push(integerRule());
        b.desc.results.push(integerRule());
    }

    {
        /*
         * TZCNT r, r/m (f3 0f bc) is the same scan with the zero case defined: it answers the
         * operand's width rather than leaving the destination alone, which is the whole reason it is
         * a second row rather than a cheaper encoding of the one above.
         *
         * **A prefix on BSF is exactly what makes the feature matter.** A processor without BMI1
         * ignores the `f3` and runs the bit scan, so a target that claimed this wrongly would not
         * fault - it would answer whatever the destination held. `kFeatureBmi1` is claimed from
         * AVX2 rather than detected for that reason; see the note on it in target.h.
         */
        auto b = add(LowerIntrinsic::CttzWidth, "tzcnt r, r/m"_v, kFeatureBmi1);
        b.form.uses.push(regOrMem(MemoryAccessKind::Read));
        b.form.defs.push(def());
        b.form.flagsEffect = FlagsEffect::Def;
        b.form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = 0xbc, .escape = 0x0f, .prefix = 0xf3,
            .regField = defRef(0), .rmField = useRef(0),
            .width = OperationWidth::FromUse0,
        };

        b.desc.operands.push(integerRule());
        b.desc.results.push(integerRule());
    }

    {
        /*
         * BSR r, r/m (0f bd) answers the index of the *highest* set bit - the same shape BSF has
         * above, at the other end of the word, and with the same undefined destination for a zero
         * operand.
         *
         * **Not a leading-zero count**, which is the whole reason both this and LZCNT below exist as
         * separate rows rather than one: `bsr` of 1 is 0 and `lzcnt` of 1 is 31, and turning one
         * into the other is a subtraction from `width - 1`. `expandBitScans` is what pays it, on a
         * target with no LZCNT, and this is the row it selects.
         *
         * Baseline, and written by that expansion alone - nothing above the backend produces one.
         */
        auto b = add(LowerIntrinsic::Bsr, "bsr r, r/m"_v, kFeatureBaseline);
        b.form.uses.push(regOrMem(MemoryAccessKind::Read));
        b.form.defs.push(def());
        b.form.flagsEffect = FlagsEffect::Def;
        b.form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = 0xbd, .escape = 0x0f,
            .regField = defRef(0), .rmField = useRef(0),
            .width = OperationWidth::FromUse0,
        };

        b.desc.operands.push(integerRule());
        b.desc.results.push(integerRule());
    }

    {
        /*
         * LZCNT r, r/m (f3 0f bd) is the count rather than the index, with the zero case defined at
         * the operand's width - which is `TZCNT`'s relationship to `BSF` said at the other end, and
         * carries `TZCNT`'s hazard with it: a processor without the feature ignores the `f3` and
         * runs `bsr`, so a wrong claim answers an index where a count was wanted and faults nowhere.
         * `kFeatureLzcnt` is claimed from a level for that reason; see the note on it in target.h.
         */
        auto b = add(LowerIntrinsic::ClzWidth, "lzcnt r, r/m"_v, kFeatureLzcnt);
        b.form.uses.push(regOrMem(MemoryAccessKind::Read));
        b.form.defs.push(def());
        b.form.flagsEffect = FlagsEffect::Def;
        b.form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = 0xbd, .escape = 0x0f, .prefix = 0xf3,
            .regField = defRef(0), .rmField = useRef(0),
            .width = OperationWidth::FromUse0,
        };

        b.desc.operands.push(integerRule());
        b.desc.results.push(integerRule());
    }

    {
        /*
         * BZHI r32a, r/m32, r32b (VEX.LZ.0F38.W0 F5 /r) copies the low `r32b` bits of its source and
         * clears everything above them - BMI2, and the first VEX-prefixed *general-register*
         * encoding in this table.
         *
         * The operand shape is the one VEX exists for: three registers, none of them tied. The
         * destination is ModRM.reg, the value is r/m - so it may be read out of a frame slot in
         * place, like every other `regOrMem` here - and the *index* is VEX.vvvv, which is why this
         * needs the prefix at all. Nothing about it is two-address, so no copy is emitted in front
         * of it and the value it reads stays live afterwards if something else wants it.
         *
         * `LZ` is a vector length of zero, which is what `vectorLength = 0` already says. The width
         * is the result's, which is REX.W in the prefix - this was `Fixed32` while the mask
         * reductions were its only caller and every one of those works at 32 bits; a 64-bit
         * `bitsUpTo` is what made the difference visible, and a form fixed at 32 would have cleared
         * the top half of a `U64` rather than the bits it was asked about.
         *
         * The flags are written (ZF and SF from the result, CF when the index was out of range) and
         * nothing reads them; the effect is declared so the window a comparison's flags survive in
         * knows that.
         */
        auto b = add(LowerIntrinsic::Bzhi, "bzhi r, r/m, r"_v, kFeatureBmi2);
        b.form.uses.push(regOrMem(MemoryAccessKind::Read));
        b.form.uses.push(anyReg());
        b.form.defs.push(def());
        b.form.flagsEffect = FlagsEffect::Def;
        b.form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = 0xf5,
            .regField = defRef(0), .rmField = useRef(0), .vvvvField = useRef(1),
            .prefixEncoding = PrefixEncoding::Vex,
            .opcodeMap = kOpcodeMap0F38,
        };

        b.desc.operands.push(integerRule());
        b.desc.operands.push(integerRule());
        b.desc.results.push(integerRule());
    }

    {
        /*
         * PEXT r32a, r32b, r/m32 (VEX.LZ.F3.0F38.W0 F5) and PDEP (F2 instead of F3) - the two
         * directions of an arbitrary bit permutation, BMI2.
         *
         * The operand roles are **not** `bzhi`'s, despite the same opcode byte and the same map: the
         * *value* is VEX.vvvv and the *mask* is r/m, where `bzhi` puts its value in r/m and its
         * index in vvvv. So the mask is the operand that may be read out of a frame slot, which is
         * the right way round for these - a mask is the operand a loop holds constant.
         *
         * Neither writes any flag, which is the other difference from every BMI1 row above.
         */
        auto permute = [&](LowerIntrinsic id, StringView formName, U8 prefix) {
            auto b = add(id, formName, kFeatureBmi2);
            b.form.uses.push(anyReg());
            b.form.uses.push(regOrMem(MemoryAccessKind::Read));
            b.form.defs.push(def());
            b.form.encoding = EncodingDescriptor {
                .family = EncodingFamily::RegRm,
                .opcode = 0xf5, .prefix = prefix,
                .regField = defRef(0), .rmField = useRef(1), .vvvvField = useRef(0),
                .prefixEncoding = PrefixEncoding::Vex,
                .opcodeMap = kOpcodeMap0F38,
            };

            b.desc.operands.push(integerRule());
            b.desc.operands.push(integerRule());
            b.desc.results.push(integerRule());
        };

        permute(LowerIntrinsic::Pext, "pext r, r, r/m"_v, 0xf3);
        permute(LowerIntrinsic::Pdep, "pdep r, r, r/m"_v, 0xf2);
    }

    {
        // CPUID (0f a2) reads the leaf in eax and the subleaf in ecx, and answers in all four of
        // eax, ebx, ecx and edx. Every one of those is a fixed register the allocator copies into
        // and out of - including ebx, which is callee-saved, so a function using this pays a push
        // and a pop for it without anything here saying so.
        auto b = add(LowerIntrinsic::Cpuid, "cpuid"_v, kFeatureBaseline);
        b.form.uses.push(fixedReg(IntRegister::rax));
        b.form.uses.push(fixedReg(IntRegister::rcx));
        b.form.defs.push(fixedDef(IntRegister::rax));
        b.form.defs.push(fixedDef(IntRegister::rbx));
        b.form.defs.push(fixedDef(IntRegister::rcx));
        b.form.defs.push(fixedDef(IntRegister::rdx));
        b.form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Opcode,
            .opcode = 0xa2, .escape = 0x0f,
            .width = OperationWidth::Fixed32,
        };

        b.desc.operands.push(integer32Rule());
        b.desc.operands.push(integer32Rule());
        for(Size i = 0; i < 4; i++) b.desc.results.push(integer32Rule());

    }

    {
        // RDTSCP (0f 01 f9) reads the timestamp counter into edx:eax and the processor id into ecx.
        // Only the counter halves are results here, so ecx is a register the instruction writes
        // without naming - an implicit clobber, exactly like a call's, and the allocator keeps live
        // values out of it for exactly the same reason.
        auto b = add(LowerIntrinsic::Rdtscp, "rdtscp"_v, kFeatureBaseline);
        b.form.defs.push(fixedDef(IntRegister::rax));
        b.form.defs.push(fixedDef(IntRegister::rdx));
        b.form.clobbers.add(gpr(IntRegister::rcx));
        b.form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Opcode,
            .opcode = 0x01, .opcodeAlt = 0xf9, .escape = 0x0f,
            .width = OperationWidth::Fixed32,
        };

        b.desc.results.push(integer32Rule());
        b.desc.results.push(integer32Rule());
    }

    {
        // RDTSC (0f 31) answers in edx:eax like RDTSCP does, but this intrinsic returns the counter
        // as the one number it is - so the two halves are joined by the expansion rather than by the
        // program (PseudoKind::RdTsc). That makes it the multi-instruction case §15.2 of the plan
        // describes, and the whole of what makes expanding it *after* allocation safe is the two
        // lines below: the shift and the or touch rdx and the flags, and the form declares both
        // before anything is placed. An expansion that needed a register the form had not named
        // would have nowhere to get one from at this point, which is why it may not need one.
        auto b = add(LowerIntrinsic::Rdtsc, "rdtsc (joined)"_v, kFeatureBaseline);
        b.form.defs.push(fixedDef(IntRegister::rax));
        b.form.clobbers.add(gpr(IntRegister::rdx));
        b.form.flagsEffect = FlagsEffect::Clobber;
        b.form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::RdTsc,
        };

        b.desc.results.push(integer64Rule());
    }

    /*
     * Memory ordering.
     *
     * The first intrinsics here with no operands, no results and no register effect at all: what
     * each of them does is entirely to the instructions around it. They are `ordered` for that
     * reason, and they state which direction they order in the memory flags - which is the one
     * thing about a fence that is worth writing down, since nothing in the register allocator will
     * ever ask.
     *
     * None of them needs a feature: SSE2 is part of AMD64 rather than an extension to it.
     */

    {
        auto fence = [&](LowerIntrinsic id, StringView formName, U8 opcodeAlt, bool reads, bool writes) {
            auto b = add(id, formName, kFeatureBaseline);
            b.form.encoding = EncodingDescriptor {
                .family = EncodingFamily::Opcode,
                .opcode = 0xae, .opcodeAlt = opcodeAlt, .escape = 0x0f,
                .width = OperationWidth::Fixed32,
            };

        };

        // The three group-15 forms, which differ only in the byte after the opcode.
        fence(LowerIntrinsic::MFence, "mfence"_v, 0xf0, true, true);
        fence(LowerIntrinsic::LFence, "lfence"_v, 0xe8, true, false);
        fence(LowerIntrinsic::SFence, "sfence"_v, 0xf8, false, true);
    }

    {
        // PAUSE (f3 90) is `rep nop`: architecturally nothing, and a hint to the processor that this
        // is a spin loop. Ordered even though it fences nothing, because the one thing that would
        // make it useless is being hoisted out of the loop it belongs to.
        auto b = add(LowerIntrinsic::Pause, "pause"_v, kFeatureBaseline);
        b.form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Opcode,
            .opcode = 0x90, .prefix = 0xf3,
            .width = OperationWidth::Fixed32,
        };

    }

    /*
     * Starting a thread - `clone`, and the child's entry behind it.
     *
     * The one entry in this file that is not an instruction, and the reason it cannot be a library
     * function is what the system call does rather than how it is spelled: **it returns twice**. The
     * parent comes back with the child's identifier; the child comes back with `rax` zero, a stack
     * pointer of its own, and the parent's every other register. Whatever the compiler generated for
     * the enclosing frame is therefore valid in exactly one of the two - a spilled local is at an
     * `rsp` offset into memory the child has never written, and `rbp` still points into a frame that
     * belongs to a thread now running somewhere else.
     *
     * So the child has to reach its entry function *before it touches memory*, out of registers and
     * the stack the parent prepared for it. That is why every C library writes this one in assembly,
     * and it is the whole of what this pseudo is.
     *
     * The operands are the system call's own, in its own registers, with the exceptions that make it
     * work: `rdx` and `r9` carry the two words of the **function** the thread is to run rather than
     * the parent-id pointer and the tls, `r8` carries its **argument**, and all three are written
     * onto the child's stack before the call. `r8` is read there and zeroed immediately after, which
     * is why the kernel's `tls` and this can share it - and why `rcx` cannot be used, being a
     * register the system call takes for itself. The kernel's `parent_tid` and `tls` are then zeroed
     * here, because nothing needs them and the caller has no register left to put them in.
     *
     * Two words and not one, because what a Yana function *is* is a code pointer and an environment
     * - see FunValueLayout. Handing both across is what lets the thread run a closure rather than
     * only a top-level function, and costs one more word on the child's stack.
     *
     * `r10` is passed through untouched: it is the kernel's `child_tid`, which with
     * `CLONE_CHILD_CLEARTID` is what makes joining possible - the kernel clears that word and wakes
     * a futex on it when the thread ends.
     */
    {
        auto b = add(LowerIntrinsic::CloneThread, "clone (with entry)"_v, kFeatureBaseline);
        b.form.uses.push(fixedReg(IntRegister::rdi));   // flags
        b.form.uses.push(fixedReg(IntRegister::rsi));   // the top of the child's stack
        b.form.uses.push(fixedReg(IntRegister::rdx));   // the code word of the function it runs
        b.form.uses.push(fixedReg(IntRegister::r8));    // the argument that function is given
        b.form.uses.push(fixedReg(IntRegister::r10));   // where the kernel clears the thread id
        b.form.uses.push(fixedReg(IntRegister::r9));    // that function's environment word
        b.form.defs.push(fixedDef(IntRegister::rax));

        // What the sequence writes without naming: the system call takes rcx and r11 for itself, and
        // the child's half of the branch runs on registers the parent will never look at again.
        b.form.clobbers.add(gpr(IntRegister::rcx));
        b.form.clobbers.add(gpr(IntRegister::r11));
        b.form.clobbers.add(gpr(IntRegister::rdx));
        b.form.clobbers.add(gpr(IntRegister::rsi));
        b.form.clobbers.add(gpr(IntRegister::rdi));
        b.form.flagsEffect = FlagsEffect::Clobber;

        b.form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::CloneThread,
        };

        // Words, every one of them, and `integer64Rule` is what says so: an address here is a value
        // the sequence *writes down* rather than one an encoding reads through, which is what
        // `pointerRule` would have claimed - see validateIntrinsics.
        for(Size i = 0; i < 6; i++) b.desc.operands.push(integer64Rule());
        b.desc.results.push(integer64Rule());

    }

    /*
     * Cache and translation control.
     *
     * Each of these takes the address it operates on and nothing else, which makes them the first
     * intrinsics with a *memory* operand in the sense a load has one: the operand is an `address()`,
     * so the address folding rewrites `p + i*8 + 16` into a folded addressing mode in front of it
     * exactly as it does for the load beside it, and the encoder writes the ModRM and SIB bytes
     * through the same shared encoder. What made that free was making "which operand is an address"
     * a question about the form (MachineForm::addressOperand) rather than about the instruction
     * kind, which is what the two passes that fold and resolve one now ask.
     */

    {
        auto cacheOp = [&](LowerIntrinsic id, StringView formName, U8 opcode, U8 extension,
                           FeatureSet features = kFeatureBaseline)
        {
            auto b = add(id, formName, features);
            b.form.uses.push(address());
            b.form.encoding = EncodingDescriptor {
                .family = EncodingFamily::LoadStore,
                .opcode = opcode, .escape = 0x0f,
                .extension = extension,
                .width = OperationWidth::Fixed32,
            };

            b.desc.operands.push(pointerRule());
        };

        // What each of these does to memory is stated in lower/lower.cpp beside its arity, which is
        // where a pass reads it: PREFETCHT0 (0f 18 /1) and PREFETCHNTA (0f 18 /0) read a line
        // towards the processor and are not `ordered`, since moving one or dropping it changes how
        // fast the program runs and nothing else; CLFLUSH (0f ae /7) is a store as far as anything
        // reasoning about memory is concerned; and INVLPG (0f 01 /7) writes no memory but changes
        // what every later access to its page means.
        cacheOp(LowerIntrinsic::Prefetch, "prefetcht0 [address]"_v, 0x18, 1);
        cacheOp(LowerIntrinsic::PrefetchNta, "prefetchnta [address]"_v, 0x18, 0);
        cacheOp(LowerIntrinsic::Clflush, "clflush [address]"_v, 0xae, 7);
        cacheOp(LowerIntrinsic::Invlpg, "invlpg [address]"_v, 0x01, 7);
    }

    /*
     * Interrupts and processor state.
     *
     * Four instructions that take nothing, answer nothing and cannot be reordered against anything.
     * The flags are worth a word: CLI and STI write the interrupt flag, which lives in the same
     * register as the condition flags and is no part of them - a comparison folded across one of
     * these is still valid, so the form says FlagsEffect::None and means it.
     */

    {
        auto systemOp = [&](LowerIntrinsic id, StringView formName, U8 opcode, U8 opcodeAlt, U8 escape) {
            auto b = add(id, formName, kFeatureBaseline);
            b.form.encoding = EncodingDescriptor {
                .family = EncodingFamily::Opcode,
                .opcode = opcode, .opcodeAlt = opcodeAlt, .escape = escape,
                .width = OperationWidth::Fixed32,
            };

        };

        systemOp(LowerIntrinsic::Hlt, "hlt"_v, 0xf4, 0, 0);
        systemOp(LowerIntrinsic::Cli, "cli"_v, 0xfa, 0, 0);
        systemOp(LowerIntrinsic::Sti, "sti"_v, 0xfb, 0, 0);
        systemOp(LowerIntrinsic::Swapgs, "swapgs"_v, 0x01, 0xf8, 0x0f);
    }

    /*
     * Model-specific and extended-state registers.
     *
     * The same shape as CPUID: fixed registers on both sides, and the allocator copying values into
     * and out of them. What is new is only that one of them writes nothing at all, so its form has
     * defs to state and none to take a width from - which is why every form here says its width
     * outright rather than deriving it from a result.
     */

    {
        // RDMSR (0f 32) takes the register number in ecx and answers in edx:eax.
        auto b = add(LowerIntrinsic::Rdmsr, "rdmsr"_v, kFeatureBaseline);
        b.form.uses.push(fixedReg(IntRegister::rcx));
        b.form.defs.push(fixedDef(IntRegister::rax));
        b.form.defs.push(fixedDef(IntRegister::rdx));
        b.form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Opcode,
            .opcode = 0x32, .escape = 0x0f,
            .width = OperationWidth::Fixed32,
        };

        b.desc.operands.push(integer32Rule());
        b.desc.results.push(integer32Rule());
        b.desc.results.push(integer32Rule());
    }

    {
        // WRMSR (0f 30), the other direction: the number in ecx and the two halves in edx:eax, with
        // nothing coming back.
        auto b = add(LowerIntrinsic::Wrmsr, "wrmsr"_v, kFeatureBaseline);
        b.form.uses.push(fixedReg(IntRegister::rcx));
        b.form.uses.push(fixedReg(IntRegister::rax));
        b.form.uses.push(fixedReg(IntRegister::rdx));
        b.form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Opcode,
            .opcode = 0x30, .escape = 0x0f,
            .width = OperationWidth::Fixed32,
        };

        for(Size i = 0; i < 3; i++) b.desc.operands.push(integer32Rule());
    }

    {
        // XGETBV (0f 01 d0) reads an extended-state register: the number in ecx, the answer in
        // edx:eax. Unprivileged, and the one intrinsic here that needs a feature the architecture
        // does not guarantee - a processor without XSAVE faults on it rather than ignoring it.
        auto b = add(LowerIntrinsic::Xgetbv, "xgetbv"_v, kFeatureBaseline);
        b.form.uses.push(fixedReg(IntRegister::rcx));
        b.form.defs.push(fixedDef(IntRegister::rax));
        b.form.defs.push(fixedDef(IntRegister::rdx));
        b.form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Opcode,
            .opcode = 0x01, .opcodeAlt = 0xd0, .escape = 0x0f,
            .width = OperationWidth::Fixed32,
        };

        b.desc.operands.push(integer32Rule());
        b.desc.results.push(integer32Rule());
        b.desc.results.push(integer32Rule());
    }

    /*
     * Port I/O.
     *
     * The port goes in dx and the value in eax or al, both fixed by the encoding. AMD64 also encodes
     * a constant port in an imm8, and these do not use it: an intrinsic has one form, and choosing
     * between two on the strength of whether an operand became an embedded constant is a mechanism
     * the registry does not have (see §15 of the plan). What that costs is one `mov edx, port` in
     * front of a bus-speed instruction, which is the trade §15.3 of the plan describes.
     */

    {
        auto portOp = [&](LowerIntrinsic id, StringView formName, U8 opcode, bool in) {
            auto b = add(id, formName, kFeatureBaseline);
            b.form.uses.push(fixedReg(IntRegister::rdx));

            if(in) b.form.defs.push(fixedDef(IntRegister::rax));
            else b.form.uses.push(fixedReg(IntRegister::rax));

            b.form.encoding = EncodingDescriptor {
                .family = EncodingFamily::Opcode,
                .opcode = opcode,
                .width = OperationWidth::Fixed32,
            };

            b.desc.operands.push(integer32Rule());
            if(in) b.desc.results.push(integer32Rule());
            else b.desc.operands.push(integer32Rule());

        };

        // IN eax, dx (ed) and OUT dx, eax (ef), at the width the IR's own integers are.
        portOp(LowerIntrinsic::In32, "in eax, dx"_v, 0xed, true);
        portOp(LowerIntrinsic::Out32, "out dx, eax"_v, 0xef, false);

        // OUT dx, al (ee) writes the low byte of the value and ignores the rest, so a byte port
        // needs nothing said about the register beyond which one it is.
        portOp(LowerIntrinsic::Out8, "out dx, al"_v, 0xee, false);

        // IN al, dx (ec) is the one direction that cannot be a single instruction here: it writes
        // *only* al and leaves the rest of eax holding whatever it held, where the result is a whole
        // Int. So it is a pseudo that zero-extends the byte afterwards (PseudoKind::PortIn8), and it
        // needs no clobber and no flags to do it - the extension writes the result's own register.
        auto b = add(LowerIntrinsic::In8, "in al, dx"_v, kFeatureBaseline);
        b.form.uses.push(fixedReg(IntRegister::rdx));
        b.form.defs.push(fixedDef(IntRegister::rax));
        b.form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::PortIn8,
        };

        b.desc.operands.push(integer32Rule());
        b.desc.results.push(integer32Rule());
    }

    /*
     * Control registers.
     *
     * MOV r64, CRn (0f 20 /r) and MOV CRn, r64 (0f 22 /r) put the *number of the control register*
     * in the ModRM.reg field, which is the same place an opcode extension goes - so each of these is
     * an ordinary RmExt form with the register number as its extension, and the value's own register
     * is whichever one the allocator chose. That is why there is one intrinsic per control register
     * rather than one taking the number as an operand: the number is part of the encoding rather
     * than a value, and an operand naming it would have nowhere to be encoded.
     *
     * Both directions are fixed at 64 bits in long mode and REX.W means nothing on them, so the
     * forms say Fixed32 to keep the prefix off - the register is 64 bits either way.
     */

    {
        struct ControlRegister {
            LowerIntrinsic id;
            U8 number;
            StringView name;
            bool write;
        };

        static const ControlRegister kControlRegisters[] = {
            { LowerIntrinsic::ReadCr0,  0, "mov r, cr0"_v, false },
            { LowerIntrinsic::ReadCr2,  2, "mov r, cr2"_v, false },
            { LowerIntrinsic::ReadCr3,  3, "mov r, cr3"_v, false },
            { LowerIntrinsic::ReadCr4,  4, "mov r, cr4"_v, false },
            { LowerIntrinsic::WriteCr0, 0, "mov cr0, r"_v, true },
            { LowerIntrinsic::WriteCr3, 3, "mov cr3, r"_v, true },
            { LowerIntrinsic::WriteCr4, 4, "mov cr4, r"_v, true },
        };

        for(auto& cr: kControlRegisters) {
            auto b = add(cr.id, cr.name, kFeatureBaseline);

            if(cr.write) b.form.uses.push(anyReg());
            else b.form.defs.push(def());

            b.form.encoding = EncodingDescriptor {
                .family = EncodingFamily::RmExt,
                .opcode = U8(cr.write ? 0x22 : 0x20), .escape = 0x0f,
                .extension = cr.number,
                .rmField = cr.write ? useRef(0) : defRef(0),
                .width = OperationWidth::Fixed32,
            };

            if(cr.write) b.desc.operands.push(integer64Rule());
            else b.desc.results.push(integer64Rule());

        }
    }
}

/*
 * Validation.
 */

bool validateIntrinsics(const MachineTarget& target) {
    auto ok = true;

    for(Size i = 0; i < kLowerIntrinsicCount; i++) {
        auto id = LowerIntrinsic(i);
        auto& desc = target.intrinsics[i];
        auto& ir = lowerIntrinsicDesc(id);

        auto fail = [&](StringView what) {
            ok = false;
            logError("intrinsic \"%@\": %@", ir.name, what);
        };

        // Every intrinsic the IR can write down has to be one this target can select, or a program
        // using it would reach the encoder with no description of what to emit.
        if(!desc.defined) {
            fail("has no descriptor for this target"_v);
            continue;
        }

        if(desc.id != id || desc.opcode != opcodeForIntrinsic(id)) {
            fail("is registered under another intrinsic's opcode"_v);
            continue;
        }

        auto& form = target.form(desc.form);
        if(form.opcode != desc.opcode) fail("names a form belonging to another opcode"_v);
        if(form.requiredFeatures != desc.requiredFeatures) fail("and its form disagree about the features they need"_v);

        // A rule per operand and per result, so that the type check below covers all of them rather
        // than however many happened to be written out.
        if(desc.operands.size() != ir.args) fail("does not state a rule for every operand"_v);
        if(desc.results.size() != ir.results) fail("does not state a rule for every result"_v);

        // The form has to be able to hold everything the IR gives the intrinsic. A form saying less
        // than the instruction does would leave operands unconstrained by accident rather than on
        // purpose, which for a privileged instruction is the difference between working code and a
        // fault.
        if(form.uses.size() != ir.args) fail("and its form disagree about how many operands it has"_v);
        if(form.defs.size() != ir.results) fail("and its form disagree about how many results it has"_v);

        // The operand a form reads as an address and the one the rules call a pointer have to be
        // the same operand. A form dereferencing something a rule let through as a plain integer
        // would read through whatever the program put there, and a rule demanding a pointer for an
        // operand the encoding keeps in a register is a requirement nothing needed.
        for(Size op = 0; op < desc.operands.size() && op < form.uses.size(); op++) {
            auto address = form.uses[op].kind == OperandConstraintKind::Address;
            auto pointer = desc.operands[op].kind == IntrinsicOperandClass::Pointer;

            if(address != pointer) fail("and its form disagree about which operand is an address"_v);
        }
    }

    return ok;
}

// Whether the values an intrinsic was given match the rules its descriptor states. Asked where the
// form is selected, which is the last point at which a wrong operand is still a compile error rather
// than a wrong instruction.
bool checkIntrinsicOperands(LowerBase base, const IntrinsicDescriptor& desc, LowerInstIntrinsic* inst) {
    auto check = [&](const IntrinsicOperandRule& rule, LowerType type, LowerValue* value) {
        switch(rule.kind) {
            case IntrinsicOperandClass::Integer:
                return isIntLike(type);
            case IntrinsicOperandClass::Integer32:
                return type == LowerType::Int32;
            case IntrinsicOperandClass::Integer64:
                return type == LowerType::Int64 || type == LowerType::Pointer;

            // An address rather than a number that could be one: an operand the encoding puts in a
            // ModRM memory field is read through, and a value that is not a pointer has not been
            // through whatever established that reading it is safe.
            case IntrinsicOperandClass::Pointer:
                return isPtr(type);

            case IntrinsicOperandClass::Immediate: {
                if(!value || value->inst()->kind != LowerInst::Imm) return false;

                auto imm = ((LowerImm*)value->inst())->i;
                return imm >= rule.minImmediate && imm <= rule.maxImmediate;
            }
        }

        return false;
    };

    auto used = inst->used();
    auto created = inst->created();

    for(Size i = 0; i < desc.operands.size() && i < used.size(); i++) {
        auto value = base[used[i]];
        if(!check(desc.operands[i], value->type, value)) return false;
    }

    for(Size i = 0; i < desc.results.size() && i < created.size(); i++) {
        if(!check(desc.results[i], created[i].type, nullptr)) return false;
    }

    return true;
}
