#include "machine_internal.h"

/*
 * Everything that happens in a general register or in one scalar float.
 *
 * The reading order is the form ids in machine_internal.h: this fills in the run of them from
 * FormNop through the scalar VEX and EVEX twins, and each section below is one group of that run.
 */

void MachineFormBuilder::registerScalarForms() {
    /*
     * Instructions that emit nothing.
     */

    add(FormNop, OpNop, "nop"_v).encoding = EncodingDescriptor {
        .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::Nop,
    };

    add(FormArg, OpArg, "arg"_v);
    add(FormPhi, OpPhi, "phi"_v);

    // An addressing mode produces no code and no register of its own; its base and index are
    // ordinary register operands, read by whichever access folds it in.
    add(FormAddress, OpAddress, "address"_v).defs.push(noDef());

    /*
     * Constants and addresses.
     */

    {
        // The immediate is the value this instruction defines rather than an operand of it, which is
        // what `immField` naming a result says. Its byte width is chosen by the encoding: the
        // shortest of the three `mov` forms that reproduces the value exactly.
        auto& form = add(FormImmMov, OpImm, "mov r, imm"_v);
        form.defs.push(def());
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::MoveImm,
            .regField = defRef(0), .immField = defRef(0),
        };
    }

    {
        // `xor r, r`, which zeroes the whole register whatever the value's declared width. Two bytes
        // where `mov r, 0` is five, at the cost of the flags - which is the whole reason this is a
        // form of its own rather than an encoding detail.
        auto& form = add(FormImmZero, OpImm, "xor r, r"_v);
        form.defs.push(def());
        form.flagsEffect = FlagsEffect::Def;
        form.encoding = regRm(0x31, defRef(0), defRef(0));
        form.encoding.width = OperationWidth::Fixed32;
    }

    add(FormImmImplicit, OpImm, "imm (embedded)"_v).defs.push(noDef());

    // A floating-point constant, which no SSE encoding carries as an immediate and which this
    // backend has nowhere to put as a constant pool entry: it is materialized in a general register
    // and moved across the bank boundary. r11 is stated as a clobber rather than as a declared
    // expansion temporary for the reason the unrolled block copy states its scratch that way - a
    // clobber keeps a live value out of the register at this one instruction, where a declared
    // temporary would be held back from the whole function.
    //
    // The two forms differ only in the width the pair moves at, which the pseudo reads from the
    // result's own type.
    auto floatImm = [&](MachineFormId id, StringView formName, RegisterClassId cls) {
        auto& form = add(id, OpImm, formName);
        form.defs.push(def(cls));
        form.clobbers.add(gpr(IntRegister::r11));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::FloatImm,
        };
    };

    floatImm(FormImmFloat32, "mov r, imm; movd xmm, r"_v, ClassFloat32);
    floatImm(FormImmFloat64, "mov r, imm; movq xmm, r"_v, ClassFloat64);

    // RIP-relative, against a displacement that is only known once every function and global has
    // been emitted - so the address the legalizer resolves carries the symbol rather than bytes.
    auto symbolAddress = [&](MachineFormId id, MachineOpcodeId opcode, StringView formName) {
        auto& form = add(id, opcode, formName);
        form.defs.push(def());
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Lea,
            .opcode = 0x8d,
            .regField = defRef(0),
            .width = OperationWidth::Fixed64,
        };
    };

    // The elided twin of each: a direct call encodes its target as a rel32 and never reads the
    // address out of a register, and a global read or written through its own address is the same
    // case - `[rip + g]` is an addressing mode, so the access carries the symbol itself and there is
    // nothing left for a register to hold. Both emit no bytes at all.
    symbolAddress(FormGlobalAddress, OpGlobalAddress, "lea r, [rip + global]"_v);
    add(FormGlobalImplicit, OpGlobalAddress, "globaddr (folded)"_v).defs.push(noDef());

    symbolAddress(FormFunctionAddress, OpFunctionAddress, "lea r, [rip + fun]"_v);
    add(FormFunctionImplicit, OpFunctionAddress, "funaddr (elided)"_v).defs.push(noDef());

    /*
     * Moves and casts.
     */

    {
        // MOV r, r/m: a source still in the frame is read in place rather than reloaded into a
        // register the copy would then read again.
        //
        // Both ends are one type, so a register source needs no clearing and a copy between one
        // register and itself is nothing at all - which is what `omitWhenSame` says, and what lets
        // buildWebs coalesce across a `Set` rather than only across a bitcast. The cast that cannot
        // omit itself is `FormCastMov`, whose two ends are *not* one type.
        auto& form = add(FormMove, OpMove, "mov r, r/m"_v);
        form.uses.push(regOrMem(MemoryAccessKind::Read));
        form.defs.push(def());
        form.encoding = regRm(0x8b, defRef(0), useRef(0));
        form.encoding.omitWhenSame = true;
    }

    // MOVSS/MOVSD xmm, xmm/m: the same shape one bank over. A register source merges into the
    // destination's upper bytes rather than clearing them, which costs nothing here - the class is
    // the scalar view, so those bytes hold nothing this value or any other is relying on.
    //
    // A bitcast between two float types of one width is the same copy, and is a form of its own for
    // the same reason the integer bitcast is: it is a copy that emits nothing at all when the
    // allocator has already put source and destination in one register.
    auto floatMove = [&](MachineFormId id, MachineOpcodeId opcode, StringView formName, U8 prefix,
                         RegisterClassId cls, bool omitWhenSame)
    {
        auto& form = add(id, opcode, formName);
        form.uses.push(regOrMem(MemoryAccessKind::Read, cls));
        form.defs.push(def(cls));
        form.encoding = sseRegRm(prefix, 0x10, defRef(0), useRef(0), OperationWidth::FromResult);
        form.encoding.omitWhenSame = omitWhenSame;
    };

    floatMove(FormMoveF32, OpMove, "movss xmm, xmm/m"_v, 0xf3, ClassFloat32, false);
    floatMove(FormMoveF64, OpMove, "movsd xmm, xmm/m"_v, 0xf2, ClassFloat64, false);

    // Casts have no memory form: their source and result widths differ by definition, and a slot is
    // exactly as wide as the value in it, so an access at the other width would take a neighbour
    // with it.
    {
        // Moved at the narrower of the two widths. A 32-bit MOV always clears the upper half of its
        // destination, so one encoding both truncates a 64-bit source and zero-extends into a 64-bit
        // destination - which is what an unsigned cast means in either direction. The move is
        // emitted even between one register and itself, since that clearing is the whole point.
        auto& form = add(FormCastMov, OpCast, "mov r, r"_v);
        form.uses.push(anyReg());
        form.defs.push(def());
        form.encoding = regRm(0x8b, defRef(0), useRef(0));
        form.encoding.width = OperationWidth::Narrowest;
    }

    {
        // The same move, for a cast whose clearing has been shown to be a no-op: the source register
        // already holds the bits the destination has to end up with, so this emits nothing at all
        // once the two are in one register and an ordinary copy while they are not. That is the only
        // difference from the form above, which cannot omit itself for exactly the reason it exists.
        //
        // Which of the two an integer cast takes is trySkipCastExtend's answer, recorded on the
        // instruction; both write nothing to the flags, so choosing between them is not one of the
        // decisions OpCast's flags-selectiveness is about.
        auto& form = add(FormCastCopy, OpCast, "mov r, r (extended)"_v);
        form.uses.push(anyReg());
        form.defs.push(def());
        form.encoding = regRm(0x8b, defRef(0), useRef(0));
        form.encoding.width = OperationWidth::Narrowest;
        form.encoding.omitWhenSame = true;
    }

    {
        // Widening a signed value into a signed one is the one case that has to carry the sign bit
        // up rather than clear the upper half.
        auto& form = add(FormCastSext, OpCast, "movsxd r, r"_v);
        form.uses.push(anyReg());
        form.defs.push(def());
        form.encoding = regRm(0x63, defRef(0), useRef(0));
    }

    /*
     * The same widening at the two narrower sources - see LowerInst::X86Sext.
     *
     * One form per source width, because the width read is an opcode byte and not an operand size:
     * `0f be` reads a byte and `0f bf` a word, and what the operand-size prefix says is how much of
     * the *destination* gets filled. So the width is taken `FromResult`, exactly as the `movsx`
     * loads two thousand lines below take theirs, and the two source widths are two rows.
     *
     * `movsxd` is a third row rather than `FormCastSext` reused: the form a selection answers with
     * has to belong to the opcode it answered for, and `verifySelection` says so.
     *
     * `byteRmField` on the first is the trap this family has and the others do not. The r/m operand
     * of a byte-source `movsx` is a *byte* register, and without a REX prefix the four encodings 4-7
     * name ah/ch/dh/bh rather than spl/bpl/sil/dil - so `movsx eax, sil` and `movsx eax, dh` are the
     * same three bytes. The store path already carries the flag for its own ModRM.reg operand; this
     * is the same rule on the other field.
     */
    {
        auto& form = add(FormSext8, OpSext, "movsx r, r8"_v);
        form.uses.push(anyReg());
        form.defs.push(def());
        form.encoding = regRm(0xbe, defRef(0), useRef(0));
        form.encoding.escape = 0x0f;
        form.encoding.width = OperationWidth::FromResult;
        form.encoding.byteRmField = true;
    }

    {
        auto& form = add(FormSext16, OpSext, "movsx r, r16"_v);
        form.uses.push(anyReg());
        form.defs.push(def());
        form.encoding = regRm(0xbf, defRef(0), useRef(0));
        form.encoding.escape = 0x0f;
        form.encoding.width = OperationWidth::FromResult;
    }

    // Only ever selected at a 64-bit result - there is no 32-to-32 `movsxd` - so the width is fixed
    // rather than read, which is what the identical load row does.
    {
        auto& form = add(FormSext32, OpSext, "movsxd r64, r32"_v);
        form.uses.push(anyReg());
        form.defs.push(def());
        form.encoding = regRm(0x63, defRef(0), useRef(0));
        form.encoding.width = OperationWidth::Fixed64;
    }

    {
        // An immediate source makes the cast a constant materialization, already narrowed or widened
        // by the move encoding's own choice of width.
        auto& form = add(FormCastImm, OpCast, "mov r, imm"_v);
        form.uses.push(immediate(ImmediateWidth::Imm64));
        form.defs.push(def());
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::MoveImm,
            .regField = defRef(0), .immField = useRef(0),
        };
    }

    {
        // And zero is `xor r, r` here for the same reason it is under OpImm: two bytes where the
        // move is five, since the register is zeroed whole whatever width either end of the cast
        // declares. The immediate is still declared as the operand it is - what the source is has
        // not changed, only what the encoding does with it - so the operand accounting is the same
        // as the form above's and the peephole that embeds the constant sees one answer for both.
        auto& form = add(FormCastZero, OpCast, "xor r, r"_v);
        form.uses.push(immediate(ImmediateWidth::Imm64));
        form.defs.push(def());
        form.flagsEffect = FlagsEffect::Def;
        form.encoding = regRm(0x31, defRef(0), defRef(0));
        form.encoding.width = OperationWidth::Fixed32;
    }

    {
        // A bitcast between two integer classes is a copy and nothing more, so one between a
        // register and itself emits nothing at all.
        auto& form = add(FormBitcast, OpBitcast, "mov r, r"_v);
        form.uses.push(anyReg());
        form.defs.push(def());
        form.encoding = regRm(0x8b, defRef(0), useRef(0));
        form.encoding.omitWhenSame = true;
    }

    {
        auto& form = add(FormBitcastImm, OpBitcast, "mov r, imm"_v);
        form.uses.push(immediate(ImmediateWidth::Imm64));
        form.defs.push(def());
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::MoveImm,
            .regField = defRef(0), .immField = useRef(0),
        };
    }

    {
        // The zero of the pair, which is the one that matters here: `bitcast 0` is what the lowering
        // makes of a null pointer, and it is the only constant a bitcast is ever given.
        auto& form = add(FormBitcastZero, OpBitcast, "xor r, r"_v);
        form.uses.push(immediate(ImmediateWidth::Imm64));
        form.defs.push(def());
        form.flagsEffect = FlagsEffect::Def;
        form.encoding = regRm(0x31, defRef(0), defRef(0));
        form.encoding.width = OperationWidth::Fixed32;
    }

    /*
     * Conversions between the banks.
     *
     * These are the one place REX.W keeps its ordinary meaning on an SSE encoding: the mandatory
     * prefix states which *float* the instruction works with and REX.W states how wide the
     * *integer* it converts to or from is. So they take their width from whichever operand is the
     * integer one, and are the SSE forms that do not set `widthInPrefix`.
     *
     * Only the signed conversions exist. An unsigned one is not an encoding this instruction set
     * has: 64-bit unsigned needs a halve-convert-double sequence, and 32-bit unsigned needs its
     * source zero-extended into a register the 64-bit conversion can read - neither of which is a
     * form, and both of which selection rejects rather than quietly emitting the signed instruction.
     */

    auto intToFloat = [&](MachineFormId id, StringView formName, U8 prefix, RegisterClassId cls) {
        auto& form = add(id, OpCast, formName);
        form.uses.push(regOrMem(MemoryAccessKind::Read));
        form.defs.push(def(cls));
        form.encoding = sseRegRm(prefix, 0x2a, defRef(0), useRef(0), OperationWidth::FromUse0);
        form.encoding.widthInPrefix = false;

        // One lane written and the rest of the register left alone - see mergesIntoDestination.
        form.encoding.mergesIntoDestination = true;
    };

    intToFloat(FormCastIToF32, "cvtsi2ss xmm, r/m"_v, 0xf3, ClassFloat32);
    intToFloat(FormCastIToF64, "cvtsi2sd xmm, r/m"_v, 0xf2, ClassFloat64);

    // Truncating towards zero, which is what a cast to an integer means everywhere else in this
    // compiler; the rounding conversion is a different instruction and would be a different form.
    auto floatToInt = [&](MachineFormId id, StringView formName, U8 prefix, RegisterClassId cls) {
        auto& form = add(id, OpCast, formName);
        form.uses.push(regOrMem(MemoryAccessKind::Read, cls));
        form.defs.push(def());
        form.encoding = sseRegRm(prefix, 0x2c, defRef(0), useRef(0), OperationWidth::FromResult);
        form.encoding.widthInPrefix = false;
    };

    floatToInt(FormCastF32ToI, "cvttss2si r, xmm/m"_v, 0xf3, ClassFloat32);
    floatToInt(FormCastF64ToI, "cvttsd2si r, xmm/m"_v, 0xf2, ClassFloat64);

    // Between the two float widths, where the prefix is the whole of the width statement again: the
    // one that names the *source*, since that is what the instruction is reading.
    auto floatToFloat = [&](MachineFormId id, StringView formName, U8 prefix,
                            RegisterClassId from, RegisterClassId to)
    {
        auto& form = add(id, OpCast, formName);
        form.uses.push(regOrMem(MemoryAccessKind::Read, from));
        form.defs.push(def(to));
        form.encoding = sseRegRm(prefix, 0x5a, defRef(0), useRef(0), OperationWidth::FromUse0);
        form.encoding.mergesIntoDestination = true;
    };

    floatToFloat(FormCastF32ToF64, "cvtss2sd xmm, xmm/m"_v, 0xf3, ClassFloat32, ClassFloat64);
    floatToFloat(FormCastF64ToF32, "cvtsd2ss xmm, xmm/m"_v, 0xf2, ClassFloat64, ClassFloat32);

    /*
     * Bitcasts across the banks.
     *
     * MOVD/MOVQ, which are one opcode each way and differ in REX.W alone - a bitcast preserves the
     * width by definition, so there is nothing for a prefix to select and the width is fixed per
     * form rather than read from an operand.
     */

    auto floatToIntBits = [&](MachineFormId id, StringView formName, RegisterClassId from,
                              RegisterClassId to, OperationWidth width)
    {
        auto& form = add(id, OpBitcast, formName);
        form.uses.push(anyReg(from));
        form.defs.push(def(to));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = 0x7e, .escape = 0x0f, .prefix = 0x66,
            .regField = useRef(0), .rmField = defRef(0),
            .width = width,
        };
    };

    floatToIntBits(FormBitcastF32ToI, "movd r/m, xmm"_v, ClassFloat32, ClassGpr32, OperationWidth::Fixed32);
    floatToIntBits(FormBitcastF64ToI, "movq r/m, xmm"_v, ClassFloat64, ClassGpr64, OperationWidth::Fixed64);

    auto intToFloatBits = [&](MachineFormId id, StringView formName, RegisterClassId from,
                              RegisterClassId to, OperationWidth width)
    {
        auto& form = add(id, OpBitcast, formName);
        form.uses.push(regOrMem(MemoryAccessKind::Read, from));
        form.defs.push(def(to));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = 0x6e, .escape = 0x0f, .prefix = 0x66,
            .regField = defRef(0), .rmField = useRef(0),
            .width = width,
        };
    };

    intToFloatBits(FormBitcastIToF32, "movd xmm, r/m"_v, ClassGpr32, ClassFloat32, OperationWidth::Fixed32);
    intToFloatBits(FormBitcastIToF64, "movq xmm, r/m"_v, ClassGpr64, ClassFloat64, OperationWidth::Fixed64);

    floatMove(FormBitcastF32, OpBitcast, "movss xmm, xmm/m"_v, 0xf3, ClassFloat32, true);
    floatMove(FormBitcastF64, OpBitcast, "movsd xmm, xmm/m"_v, 0xf2, ClassFloat64, true);

    /*
     * Unary arithmetic.
     *
     * NEG and NOT take their subject as r/m, so a value the allocator left in the frame is negated
     * or inverted in place rather than loaded, changed and stored back.
     *
     * They share an encoding shape and differ in one thing beyond the opcode extension: `neg` is a
     * subtraction from zero and sets the flags accordingly, while `not` is a bitwise complement and
     * leaves them entirely alone. Saying otherwise costs a compare that could have been folded
     * across it.
     */

    auto unaryArith = [&](MachineFormId id, MachineOpcodeId opcode, StringView formName, U8 extension,
                          FlagsEffect flags)
    {
        auto& form = add(id, opcode, formName);
        form.uses.push(regOrMem(MemoryAccessKind::ReadWrite));
        form.defs.push(tiedDef(0));
        form.flagsEffect = flags;
        form.encoding = rmExt(0xf7, extension, useRef(0));
    };

    unaryArith(FormNeg, OpNeg, "neg r/m"_v, 3, FlagsEffect::Def);
    unaryArith(FormNot, OpNot, "not r/m"_v, 2, FlagsEffect::None);

    /*
     * BSWAP r (0f c8+r) - the byte reversal, and the one unary here whose operand cannot be a frame
     * slot.
     *
     * The register is part of the opcode byte rather than named in a ModRM field, which is what
     * `EncodingFamily::OpcodeReg` says and is also why there is no `bswap [m]` to declare as a
     * memory twin: the encoding has nowhere to put an address. A value the allocator left in the
     * frame is therefore loaded first, exactly as it is for any other form with no r/m operand -
     * and where the value came *out* of memory in the first place, `selectByteSwapMemory` has
     * already turned the pair into a `movbe` and this form is not reached at all.
     *
     * Destructive like the two-address ALU operations, and it writes no flags: the whole of the
     * instruction is the permutation.
     *
     * **32 and 64 bits only**, which is all the IR can hand it - `bswap r16` is undefined on this
     * architecture, and the 16-bit swap a program can write was spent above the lower IR precisely
     * because there is no register width here to hold one.
     */
    {
        auto& form = add(FormBswap, OpBswap, "bswap r"_v);
        form.uses.push(anyReg());
        form.defs.push(tiedDef(0));
        form.flagsEffect = FlagsEffect::None;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::OpcodeReg,
            .opcode = 0xc8, .escape = 0x0f,
            .rmField = useRef(0),
            .width = OperationWidth::FromUse0,
        };
    }

    // `neg` sets ZF from the result the way the group-1 operations do; `not` writes no flag at all,
    // which is why a comparison of its result against zero is a comparison and not a redundancy.
    forms[FormNeg].resultInFlags = true;

    /*
     * The group-1 ALU operations.
     *
     * Two forms each. The register one can take either operand from memory - `add [slot], rcx` when
     * the result lives in that very slot, or `add rax, [slot]` when the right-hand side does - and
     * only one of the two at a time, because both want the single r/m field. The immediate one has
     * no register right-hand side to take from anywhere.
     */

    auto binaryAlu = [&](MachineFormId regId, MachineFormId immId, MachineFormId memId, MachineOpcodeId opcode,
                         StringView regName, StringView immName, StringView memName,
                         U8 rmRegOp, U8 regRmOp, U8 extension, bool logical)
    {
        auto& regForm = add(regId, opcode, regName);
        regForm.uses.push(regOrMem(MemoryAccessKind::ReadWrite));
        regForm.uses.push(regOrMem(MemoryAccessKind::Read));
        regForm.defs.push(tiedDef(0));
        regForm.flagsEffect = FlagsEffect::Def;
        regForm.resultInFlags = true;
        regForm.signInFlags = logical;
        regForm.encoding = regRm(rmRegOp, useRef(1), useRef(0), regRmOp);

        auto& immForm = add(immId, opcode, immName);
        immForm.uses.push(regOrMem(MemoryAccessKind::ReadWrite));
        immForm.uses.push(immediate(ImmediateWidth::Imm8OrImm32));
        immForm.defs.push(tiedDef(0));
        immForm.flagsEffect = FlagsEffect::Def;
        immForm.resultInFlags = true;
        immForm.signInFlags = logical;
        immForm.encoding = rmExtImm(0x83, 0x81, extension, useRef(0), useRef(1));

        // The twin is a copy of the register form, so it carries both flag claims with it - which is
        // right: reading an operand out of memory changes where the operands come from and nothing
        // about what the operation leaves in the flags.
        memoryTwin(memId, regId, memName);
    };

    // `logical` is the second claim: `and`, `or` and `xor` clear OF, so SF against OF is the sign of
    // their result and a signed comparison of it against zero is answered too. `add` and `sub` set OF
    // from the operation and are not that - see MachineForm::signInFlags.
    binaryAlu(FormAddReg, FormAddImm, FormAddMem, OpAdd,
        "add r/m, r"_v, "add r/m, imm"_v, "add r, [address]"_v, 0x01, 0x03, 0, false);
    binaryAlu(FormSubReg, FormSubImm, FormSubMem, OpSub,
        "sub r/m, r"_v, "sub r/m, imm"_v, "sub r, [address]"_v, 0x29, 0x2b, 5, false);
    binaryAlu(FormAndReg, FormAndImm, FormAndMem, OpAnd,
        "and r/m, r"_v, "and r/m, imm"_v, "and r, [address]"_v, 0x21, 0x23, 4, true);
    binaryAlu(FormOrReg, FormOrImm, FormOrMem, OpOr,
        "or r/m, r"_v, "or r/m, imm"_v, "or r, [address]"_v, 0x09, 0x0b, 1, true);
    binaryAlu(FormXorReg, FormXorImm, FormXorMem, OpXor,
        "xor r/m, r"_v, "xor r/m, imm"_v, "xor r, [address]"_v, 0x31, 0x33, 6, true);

    /*
     * Increment and decrement.
     *
     * `inc r/m` is one byte shorter than the `add r/m, 1` it replaces, and takes its subject as r/m
     * exactly as that does - so a destination the allocator left in the frame is incremented in
     * place like any other in-place accumulator. Which of the two an addition of one takes is
     * decided from the immediate alone, so this is a form rather than something the encoder notices.
     */

    auto unitStep = [&](MachineFormId id, MachineOpcodeId opcode, StringView formName, U8 extension) {
        auto& form = add(id, opcode, formName);
        form.uses.push(regOrMem(MemoryAccessKind::ReadWrite));
        form.uses.push(immediate(ImmediateWidth::Imm8OrImm32));
        form.defs.push(tiedDef(0));
        form.flagsEffect = FlagsEffect::Def;
        form.resultInFlags = true;
        form.encoding = rmExt(0xff, extension, useRef(0));
    };

    unitStep(FormAddInc, OpAdd, "inc r/m"_v, 0);
    unitStep(FormAddDec, OpAdd, "dec r/m"_v, 1);
    unitStep(FormSubInc, OpSub, "inc r/m"_v, 0);
    unitStep(FormSubDec, OpSub, "dec r/m"_v, 1);

    /*
     * Multiply and divide.
     *
     * The group-3 forms read their first operand out of rax and write their result back into it (or,
     * for a remainder, into rdx), and take the second as r/m - so a divisor can come straight out of
     * the frame with no reload. rdx is written either way, which is why it is a clobber even where it
     * is also the result: a value living there has to be out of the way regardless.
     */

    auto group3 = [&](MachineFormId id, MachineOpcodeId opcode, StringView formName,
                      IntRegister result, bool clobberRax, U8 extension, EncodingPrelude prelude)
    {
        auto& form = add(id, opcode, formName);
        form.uses.push(fixedReg(IntRegister::rax));
        form.uses.push(regOrMem(MemoryAccessKind::Read));

        auto resultDef = def();
        resultDef.kind = OperandConstraintKind::FixedRegister;
        resultDef.fixedReg = gpr(result);
        form.defs.push(resultDef);

        form.clobbers.add(gpr(IntRegister::rdx));
        if(clobberRax) form.clobbers.add(gpr(IntRegister::rax));

        form.flagsEffect = FlagsEffect::Def;
        form.encoding = rmExt(0xf7, extension, useRef(1));
        form.encoding.prelude = prelude;
    };

    group3(FormMul, OpMul, "mul r/m"_v, IntRegister::rax, false, 4, EncodingPrelude::None);
    group3(FormDiv, OpDiv, "div r/m"_v, IntRegister::rax, false, 6, EncodingPrelude::ZeroRdx);
    group3(FormIDiv, OpIDiv, "idiv r/m"_v, IntRegister::rax, false, 7, EncodingPrelude::SignExtendRax);
    group3(FormRem, OpRem, "div r/m (remainder)"_v, IntRegister::rdx, true, 6, EncodingPrelude::ZeroRdx);
    group3(FormIRem, OpIRem, "idiv r/m (remainder)"_v, IntRegister::rdx, true, 7, EncodingPrelude::SignExtendRax);

    // The same two multiplies, read for the half they are usually asked to throw away. No prelude:
    // unlike a division, a multiply *writes* the whole pair rather than reading it, so nothing has
    // to be in rdx beforehand.
    group3(FormMulHi, OpMulHi, "mul r/m (high)"_v, IntRegister::rdx, true, 4, EncodingPrelude::None);
    group3(FormIMulHi, OpIMulHi, "imul r/m (high)"_v, IntRegister::rdx, true, 5, EncodingPrelude::None);

    {
        // IMUL r, r/m is the two-operand form: the destination doubles as a source, so it is
        // destructive like the group-1 operations. Only one direction exists - the destination is
        // always the reg field - so a spilled operand can only ever be the right-hand side.
        auto& form = add(FormIMulReg, OpIMul, "imul r, r/m"_v);
        form.uses.push(anyReg());
        form.uses.push(regOrMem(MemoryAccessKind::Read));
        form.defs.push(tiedDef(0));
        form.flagsEffect = FlagsEffect::Def;
        form.encoding = regRm(0xaf, useRef(0), useRef(1));
        form.encoding.escape = 0x0f;
    }

    memoryTwin(FormIMulMem, FormIMulReg, "imul r, [address]"_v);

    {
        // IMUL r, r/m, imm is a true three-operand form - the destination can differ from the source
        // - which is why the immediate case is not destructive where the register case is.
        auto& form = add(FormIMulImm, OpIMul, "imul r, r, imm"_v);
        form.uses.push(anyReg());
        form.uses.push(immediate(ImmediateWidth::Imm8OrImm32));
        form.defs.push(def());
        form.flagsEffect = FlagsEffect::Def;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRmImm,
            .opcode = 0x6b, .opcodeAlt = 0x69,
            .regField = defRef(0), .rmField = useRef(0), .immField = useRef(1),
        };
    }

    /*
     * Shifts.
     *
     * Every shift form takes its subject as r/m, so a destination in the frame is shifted in place.
     * The count is either an immediate in the instruction or in cl, and is never the memory operand.
     */

    auto shift = [&](MachineFormId immId, MachineFormId oneId, MachineFormId clId, MachineOpcodeId opcode,
                     StringView immName, StringView oneName, StringView clName, U8 extension)
    {
        auto& immForm = add(immId, opcode, immName);
        immForm.uses.push(regOrMem(MemoryAccessKind::ReadWrite));
        immForm.uses.push(immediate(ImmediateWidth::Imm8));
        immForm.defs.push(tiedDef(0));
        immForm.flagsEffect = FlagsEffect::Def;
        immForm.encoding = rmExtImm(0xc1, 0, extension, useRef(0), useRef(1));

        // A shift by one has an encoding with no immediate byte at all, which is a form rather than
        // an encoder's notice for the same reason `inc` is: it is decided by the value alone.
        auto& oneForm = add(oneId, opcode, oneName);
        oneForm.uses.push(regOrMem(MemoryAccessKind::ReadWrite));
        oneForm.uses.push(immediate(ImmediateWidth::Imm8));
        oneForm.defs.push(tiedDef(0));
        oneForm.flagsEffect = FlagsEffect::Def;
        oneForm.encoding = rmExt(0xd1, extension, useRef(0));

        auto& clForm = add(clId, opcode, clName);
        clForm.uses.push(regOrMem(MemoryAccessKind::ReadWrite));
        clForm.uses.push(fixedReg(IntRegister::rcx));
        clForm.defs.push(tiedDef(0));
        clForm.flagsEffect = FlagsEffect::Def;
        clForm.encoding = rmExt(0xd3, extension, useRef(0));
    };

    shift(FormShlImm, FormShlOne, FormShlCl, OpShl, "shl r/m, imm"_v, "shl r/m, 1"_v, "shl r/m, cl"_v, 4);
    shift(FormShrImm, FormShrOne, FormShrCl, OpShr, "shr r/m, imm"_v, "shr r/m, 1"_v, "shr r/m, cl"_v, 5);
    shift(FormSarImm, FormSarOne, FormSarCl, OpSar, "sar r/m, imm"_v, "sar r/m, 1"_v, "sar r/m, cl"_v, 7);

    // The rotations are the same three encodings at extensions 0 and 1, so they are the same
    // builder and nothing about them is a special case anywhere below this line. `rol r/m, 1` is
    // the one-byte form for the same reason `shl r/m, 1` is - and unlike `shl`, it is *not* also an
    // `lea`, which is why the address-folding peephole in gen.cpp asks for `OpShl` by name.
    shift(FormRolImm, FormRolOne, FormRolCl, OpRol, "rol r/m, imm"_v, "rol r/m, 1"_v, "rol r/m, cl"_v, 0);
    shift(FormRorImm, FormRorOne, FormRorCl, OpRor, "ror r/m, imm"_v, "ror r/m, 1"_v, "ror r/m, cl"_v, 1);

    /*
     * Comparison.
     *
     * A comparison works at the width of the values compared, not at the width of what it produces:
     * its result is an Int32 whatever went into it, so `widthFromUse` points at the left-hand side.
     * The result is written to a register only when the flags could not be carried to its use
     * directly, which the compare folding decides.
     */

    // Two forms each, differing only in whether the flags are materialized afterwards: `setcc` into
    // the result's low byte and a zero-extension over the rest of it. Which applies is the compare
    // folding's answer, recorded on the instruction as an implicit result.
    auto compare = [&](MachineFormId flagsId, MachineFormId setId, StringView flagsName, StringView setName,
                       const MachineOperandConstraint& rhs, EncodingDescriptor encoding)
    {
        auto& flagsForm = add(flagsId, OpCmp, flagsName);
        flagsForm.uses.push(anyReg());
        flagsForm.uses.push(rhs);
        flagsForm.defs.push(noDef());
        flagsForm.flagsEffect = FlagsEffect::Def;
        flagsForm.encoding = encoding;
        flagsForm.encoding.width = OperationWidth::FromUse0;

        auto& setForm = add(setId, OpCmp, setName);
        setForm.uses.push(anyReg());
        setForm.uses.push(rhs);
        setForm.defs.push(def(ClassGpr32));
        setForm.flagsEffect = FlagsEffect::Def;
        setForm.encoding = encoding;
        setForm.encoding.width = OperationWidth::FromUse0;
        setForm.encoding.materializeFlags = true;
    };

    compare(FormCmpReg, FormCmpRegSet, "cmp r, r/m"_v, "cmp r, r/m; setcc r"_v,
        regOrMem(MemoryAccessKind::Read), regRm(0x39, useRef(1), useRef(0), 0x3b));

    memoryTwin(FormCmpMem, FormCmpReg, "cmp r, [address]"_v);
    memoryTwin(FormCmpMemSet, FormCmpRegSet, "cmp r, [address]; setcc r"_v);

    // A comparison against zero has a shorter equivalent in `test r, r`, which leaves every
    // condition code this backend reads in the same state. It needs the value in a register, so the
    // descriptor states it as the alternative and an operand still in the frame keeps the `cmp`.
    auto cmpImm = rmExtImm(0x83, 0x81, 7, useRef(0), useRef(1));
    cmpImm.zeroRegOpcode = 0x85;

    compare(FormCmpImm, FormCmpImmSet, "cmp r/m, imm"_v, "cmp r/m, imm; setcc r"_v,
        immediate(ImmediateWidth::Imm8OrImm32), cmpImm);

    {
        /*
         * §3.5.2.2 And the comparison that emits nothing, because the instruction that produced its
         * left-hand side already left the answer in ZF.
         *
         * The operands stay ordinary operands. Nothing is folded and nothing is declared implicit:
         * the value being compared is one some other instruction computed and some other instruction
         * reads, and saying that this one no longer names it would shorten a live range for no gain
         * and leave the arithmetic looking like a definition nothing wants. What changes is the
         * encoding and the flags effect, and those are the whole of the difference.
         *
         * The operands are declared exactly as FormCmpImm's are, which is what makes the choice
         * between the two free: the elision is decided after every allocation question has been
         * asked, so a form demanding anything different would be a demand nothing could still meet.
         * The right-hand side is the embedded constant zero, always - `tryElideCompare` admits no
         * other - and the left-hand side wants a register there for the same reason it wants one in
         * FormCmpImm, which is that a comparison against an immediate has no memory form.
         */
        auto& form = add(FormCmpNone, OpCmp, "cmp (already in flags)"_v);
        form.uses.push(anyReg());
        form.uses.push(immediate(ImmediateWidth::Imm8OrImm32));
        form.defs.push(noDef());
        form.flagsEffect = FlagsEffect::None;
    }

    /*
     * Scalar floating-point arithmetic.
     *
     * Destructive in the same way the group-1 integer operations are, and constrained rather more:
     * there is only one direction, so the operand that may stay in the frame is always the
     * right-hand side, and there is no immediate form at all - which is why a float constant is
     * never embedded and always materialized (see isEmbeddableImm in transform_peephole.cpp).
     *
     * None of them touches the flags. That is a real difference from the integer opcodes rather
     * than a convenience: it is what lets a comparison be folded across a stretch of floating-point
     * arithmetic into the branch that reads it.
     */

    auto floatArith = [&](MachineFormId f32, MachineFormId f64, MachineOpcodeId opcode,
                          StringView name32, StringView name64, U8 op)
    {
        auto build = [&](MachineFormId id, StringView formName, U8 prefix, RegisterClassId cls) {
            auto& form = add(id, opcode, formName);
            form.uses.push(anyReg(cls));
            form.uses.push(regOrMem(MemoryAccessKind::Read, cls));
            form.defs.push(tiedDef(0, cls));
            form.encoding = sseRegRm(prefix, op, useRef(0), useRef(1), OperationWidth::FromResult);
        };

        build(f32, name32, 0xf3, ClassFloat32);
        build(f64, name64, 0xf2, ClassFloat64);
    };

    floatArith(FormFAdd32, FormFAdd64, OpFAdd, "addss xmm, xmm/m"_v, "addsd xmm, xmm/m"_v, 0x58);
    floatArith(FormFSub32, FormFSub64, OpFSub, "subss xmm, xmm/m"_v, "subsd xmm, xmm/m"_v, 0x5c);
    floatArith(FormFMul32, FormFMul64, OpFMul, "mulss xmm, xmm/m"_v, "mulsd xmm, xmm/m"_v, 0x59);
    floatArith(FormFDiv32, FormFDiv64, OpFDiv, "divss xmm, xmm/m"_v, "divsd xmm, xmm/m"_v, 0x5e);

    /*
     * Floating-point negation.
     *
     * AMD64 has no scalar float negate, so it is an exclusive-or against a sign mask - and the mask
     * is sixteen bytes, which is why this had to wait for the constant pool. Until it existed the
     * sign bit was toggled in a general register instead: `movq r, xmm; btc r, 63; movq xmm, r`,
     * three instructions, a bank crossing in each direction, and r11 declared as a clobber so that
     * nothing live could be sitting in it.
     *
     * That is now `xorps xmm, [rip + m]` - one instruction and no general register. The third gain
     * is the one that reaches past the negation itself: **these no longer touch the flags**, where
     * `btc` wrote the carry, so a negation may now sit inside a comparison's fold window.
     *
     * The mask is on the MachineFunction rather than in the operand list, because the encoding names
     * it and the allocator therefore has nothing to place - see `poolSignMasks`.
     */

    auto floatNeg = [&](MachineFormId id, StringView formName, RegisterClassId cls, OperationWidth width) {
        auto& form = add(id, OpFNeg, formName);
        form.uses.push(anyReg(cls));
        form.defs.push(tiedDef(0, cls));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::FloatNeg,
            .width = width,
        };
    };

    floatNeg(FormFNeg32, "xorps xmm, [rip + signmask]"_v, ClassFloat32, OperationWidth::Fixed32);
    floatNeg(FormFNeg64, "xorpd xmm, [rip + signmask]"_v, ClassFloat64, OperationWidth::Fixed64);

    // The memory sources of the four above, in the same order the enum declares them. Scalar SSE has
    // one direction only, so each twin is its source's own encoding with the r/m field addressed.
    memoryTwin(FormFAdd32Mem, FormFAdd32, "addss xmm, [address]"_v);
    memoryTwin(FormFAdd64Mem, FormFAdd64, "addsd xmm, [address]"_v);
    memoryTwin(FormFSub32Mem, FormFSub32, "subss xmm, [address]"_v);
    memoryTwin(FormFSub64Mem, FormFSub64, "subsd xmm, [address]"_v);
    memoryTwin(FormFMul32Mem, FormFMul32, "mulss xmm, [address]"_v);
    memoryTwin(FormFMul64Mem, FormFMul64, "mulsd xmm, [address]"_v);
    memoryTwin(FormFDiv32Mem, FormFDiv32, "divss xmm, [address]"_v);
    memoryTwin(FormFDiv64Mem, FormFDiv64, "divsd xmm, [address]"_v);

    /*
     * Floating-point comparison.
     *
     * UCOMISS/UCOMISD leave the result in the same flags an unsigned integer comparison does - CF
     * for "below", ZF for "equal" - so every condition code the rest of this backend already writes
     * reads correctly, and, crucially, so does the *negation* of each: `ja` and `jbe` remain exact
     * opposites, which is what the branch and select forms rely on when they flip a condition to
     * fall through the other way.
     *
     * An operand that is NaN sets CF, ZF and PF together, and that is the whole of what makes these
     * forms not enough on their own. Two things arrange the rest, neither of them here:
     *
     *  - orderFloatCompare exchanges the operands of `lt` and `le` so that every ordering comparison
     *    reaching selection is `gt` or `ge`. Those read CF, which a NaN sets, so they answer false -
     *    which is what an ordered comparison of a NaN has to do.
     *  - equality is not a condition code at all, since it needs ZF *and* PF. tryMergeCompare
     *    therefore refuses to leave one in the flags, and genFloatFlagsToReg writes the answer into
     *    the register with the parity correction attached.
     *
     * So all six agree with the LLVM backend and with the JavaScript one: every ordered comparison
     * of a NaN is false, and `!=` alone is true.
     */

    auto floatCompare = [&](MachineFormId flagsId, MachineFormId setId, StringView flagsName,
                            StringView setName, U8 prefix, RegisterClassId cls)
    {
        auto encoding = sseRegRm(prefix, 0x2e, useRef(0), useRef(1), OperationWidth::FromUse0);

        auto& flagsForm = add(flagsId, OpFCmp, flagsName);
        flagsForm.uses.push(anyReg(cls));
        flagsForm.uses.push(regOrMem(MemoryAccessKind::Read, cls));
        flagsForm.defs.push(noDef());
        flagsForm.flagsEffect = FlagsEffect::Def;
        flagsForm.encoding = encoding;

        auto& setForm = add(setId, OpFCmp, setName);
        setForm.uses.push(anyReg(cls));
        setForm.uses.push(regOrMem(MemoryAccessKind::Read, cls));
        setForm.defs.push(def(ClassGpr32));
        setForm.flagsEffect = FlagsEffect::Def;
        setForm.encoding = encoding;
        setForm.encoding.materializeFlags = true;
    };

    floatCompare(FormFCmp32, FormFCmp32Set, "ucomiss xmm, xmm/m"_v, "ucomiss xmm, xmm/m; setcc r"_v,
        0, ClassFloat32);
    floatCompare(FormFCmp64, FormFCmp64Set, "ucomisd xmm, xmm/m"_v, "ucomisd xmm, xmm/m; setcc r"_v,
        0x66, ClassFloat64);

    memoryTwin(FormFCmp32Mem, FormFCmp32, "ucomiss xmm, [address]"_v);
    memoryTwin(FormFCmp32MemSet, FormFCmp32Set, "ucomiss xmm, [address]; setcc r"_v);
    memoryTwin(FormFCmp64Mem, FormFCmp64, "ucomisd xmm, [address]"_v);
    memoryTwin(FormFCmp64MemSet, FormFCmp64Set, "ucomisd xmm, [address]; setcc r"_v);

    /*
     * And every one of them again with a VEX prefix, for a target that has AVX.
     *
     * Selection takes these in place of the forms above wherever the feature is present, and takes
     * the memory twin of whichever it landed on - see selectForm. Nothing else in the backend knows
     * they exist: they are the same opcodes with the same operands and the same effects, so every
     * peephole, every costing and every verifier reads them exactly as it reads the originals.
     *
     * The arithmetic is where the win is, and it is not the one byte the prefix costs or saves. A
     * three-operand encoding needs no copy in front of it to satisfy a tie, which is one instruction
     * removed from every floating-point operation whose result outlives one of its sources.
     *
     * The negation is here too and is the same change made to a pseudo: `vxorps` against the pooled
     * sign mask names its destination separately, where `xorps` toggled the bit in place.
     *
     * The rest of the scalar float set - the conversions, the constant materialization, the select -
     * used to be deliberately absent, on the grounds that none of them carries a tie worth removing
     * and that mixing the two spellings costs nothing while no 256-bit instruction is written. **The
     * second half of that stopped being true when the wide tier landed**, and it was the reasoning
     * rather than the list that was wrong: a legacy encoding is not merely a longer spelling, it is
     * a *partial write* of a register whose upper half something else may have dirtied. So every one
     * of them has a twin now, built by the sweep at the end of this constructor rather than listed
     * here - what is written out by hand is the forms that also need an EVEX tier above them, which
     * is what these are.
     */

    // One operation, both tiers, at both widths, with and without a folded address: sixteen forms
    // from one call, and the only thing stated sixteen times is a name.
    auto vexArith = [&](MachineFormId f32, MachineFormId f64, MachineFormId f32Mem, MachineFormId f64Mem,
                        MachineFormId e32, MachineFormId e64, MachineFormId e32Mem, MachineFormId e64Mem,
                        MachineFormId source32, MachineFormId source64, MachineFormId sourceMem32,
                        MachineFormId sourceMem64, StringView name32, StringView name64,
                        StringView memName32, StringView memName64,
                        StringView eName32, StringView eName64, StringView eMemName32, StringView eMemName64)
    {
        vexTwin(f32, source32, name32, true);
        vexTwin(f64, source64, name64, true);

        // The memory twins are built from the *VEX* forms rather than derived from the legacy memory
        // ones, so that the three-operand shape and the folded address come from one place. Their
        // legacy counterparts get the link as well, which makes the two swaps commute: whichever of
        // them selection applies first, the other still finds what it is looking for.
        memoryTwin(f32Mem, f32, memName32).alternativeOf = sourceMem32;
        memoryTwin(f64Mem, f64, memName64).alternativeOf = sourceMem64;

        forms[sourceMem32].alternative = f32Mem;
        forms[sourceMem64].alternative = f64Mem;

        // And the same again one tier up, out of the VEX forms.
        evexTwin(e32, f32, eName32, true);
        evexTwin(e64, f64, eName64, true);

        memoryTwin(e32Mem, e32, eMemName32).alternativeOf = f32Mem;
        memoryTwin(e64Mem, e64, eMemName64).alternativeOf = f64Mem;

        forms[f32Mem].alternative = e32Mem;
        forms[f64Mem].alternative = e64Mem;
    };

    vexArith(FormFAdd32Vex, FormFAdd64Vex, FormFAdd32VexMem, FormFAdd64VexMem,
        FormFAdd32Evex, FormFAdd64Evex, FormFAdd32EvexMem, FormFAdd64EvexMem,
        FormFAdd32, FormFAdd64, FormFAdd32Mem, FormFAdd64Mem,
        "vaddss xmm, xmm, xmm/m"_v, "vaddsd xmm, xmm, xmm/m"_v,
        "vaddss xmm, xmm, [address]"_v, "vaddsd xmm, xmm, [address]"_v,
        "vaddss (evex) xmm, xmm, xmm/m"_v, "vaddsd (evex) xmm, xmm, xmm/m"_v,
        "vaddss (evex) xmm, xmm, [address]"_v, "vaddsd (evex) xmm, xmm, [address]"_v);

    vexArith(FormFSub32Vex, FormFSub64Vex, FormFSub32VexMem, FormFSub64VexMem,
        FormFSub32Evex, FormFSub64Evex, FormFSub32EvexMem, FormFSub64EvexMem,
        FormFSub32, FormFSub64, FormFSub32Mem, FormFSub64Mem,
        "vsubss xmm, xmm, xmm/m"_v, "vsubsd xmm, xmm, xmm/m"_v,
        "vsubss xmm, xmm, [address]"_v, "vsubsd xmm, xmm, [address]"_v,
        "vsubss (evex) xmm, xmm, xmm/m"_v, "vsubsd (evex) xmm, xmm, xmm/m"_v,
        "vsubss (evex) xmm, xmm, [address]"_v, "vsubsd (evex) xmm, xmm, [address]"_v);

    vexArith(FormFMul32Vex, FormFMul64Vex, FormFMul32VexMem, FormFMul64VexMem,
        FormFMul32Evex, FormFMul64Evex, FormFMul32EvexMem, FormFMul64EvexMem,
        FormFMul32, FormFMul64, FormFMul32Mem, FormFMul64Mem,
        "vmulss xmm, xmm, xmm/m"_v, "vmulsd xmm, xmm, xmm/m"_v,
        "vmulss xmm, xmm, [address]"_v, "vmulsd xmm, xmm, [address]"_v,
        "vmulss (evex) xmm, xmm, xmm/m"_v, "vmulsd (evex) xmm, xmm, xmm/m"_v,
        "vmulss (evex) xmm, xmm, [address]"_v, "vmulsd (evex) xmm, xmm, [address]"_v);

    vexArith(FormFDiv32Vex, FormFDiv64Vex, FormFDiv32VexMem, FormFDiv64VexMem,
        FormFDiv32Evex, FormFDiv64Evex, FormFDiv32EvexMem, FormFDiv64EvexMem,
        FormFDiv32, FormFDiv64, FormFDiv32Mem, FormFDiv64Mem,
        "vdivss xmm, xmm, xmm/m"_v, "vdivsd xmm, xmm, xmm/m"_v,
        "vdivss xmm, xmm, [address]"_v, "vdivsd xmm, xmm, [address]"_v,
        "vdivss (evex) xmm, xmm, xmm/m"_v, "vdivsd (evex) xmm, xmm, xmm/m"_v,
        "vdivss (evex) xmm, xmm, [address]"_v, "vdivsd (evex) xmm, xmm, [address]"_v);

    vexTwin(FormFNeg32Vex, FormFNeg32, "vxorps xmm, xmm, [rip + signmask]"_v, true);
    vexTwin(FormFNeg64Vex, FormFNeg64, "vxorpd xmm, xmm, [rip + signmask]"_v, true);

    vexTwin(FormFCmp32Vex, FormFCmp32, "vucomiss xmm, xmm/m"_v, false);
    vexTwin(FormFCmp32VexSet, FormFCmp32Set, "vucomiss xmm, xmm/m; setcc r"_v, false);
    vexTwin(FormFCmp64Vex, FormFCmp64, "vucomisd xmm, xmm/m"_v, false);
    vexTwin(FormFCmp64VexSet, FormFCmp64Set, "vucomisd xmm, xmm/m; setcc r"_v, false);

    memoryTwin(FormFCmp32VexMem, FormFCmp32Vex, "vucomiss xmm, [address]"_v).alternativeOf = FormFCmp32Mem;
    memoryTwin(FormFCmp32VexMemSet, FormFCmp32VexSet, "vucomiss xmm, [address]; setcc r"_v).alternativeOf = FormFCmp32MemSet;
    memoryTwin(FormFCmp64VexMem, FormFCmp64Vex, "vucomisd xmm, [address]"_v).alternativeOf = FormFCmp64Mem;
    memoryTwin(FormFCmp64VexMemSet, FormFCmp64VexSet, "vucomisd xmm, [address]; setcc r"_v).alternativeOf = FormFCmp64MemSet;

    forms[FormFCmp32Mem].alternative = FormFCmp32VexMem;
    forms[FormFCmp32MemSet].alternative = FormFCmp32VexMemSet;
    forms[FormFCmp64Mem].alternative = FormFCmp64VexMem;
    forms[FormFCmp64MemSet].alternative = FormFCmp64VexMemSet;
}
