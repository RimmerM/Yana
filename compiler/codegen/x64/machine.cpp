#include "machine.h"
#include "x64_util.h"

/*
 * The AMD64 form table, and the selection that chooses between its entries.
 *
 * Everything the backend used to know about an instruction in several places is stated here once.
 * Reading order: the form ids below name the table's entries, the builder fills them, and selectForm
 * at the bottom says which one an instruction takes.
 *
 * Two conventions keep the table short. A form's `uses` array only has to cover the operands that
 * have something to say: an operand beyond its length is an unconstrained register of its own class,
 * which is what most operands of most instructions are. And a form whose operand count is not fixed
 * - a call has as many as it has arguments - sets `conventionOperands` and takes its constraints
 * from the calling convention instead.
 */

enum: MachineFormId {
    FormNop,
    FormArg,
    FormPhi,
    FormAddress,

    FormImmMov,
    FormImmZero,
    FormImmImplicit,
    FormImmFloat32,
    FormImmFloat64,
    FormGlobalAddress,
    FormFunctionAddress,
    FormFunctionImplicit,

    FormMove,
    FormMoveF32,
    FormMoveF64,
    FormCastMov,
    FormCastSext,
    FormCastImm,
    FormBitcast,
    FormBitcastImm,

    FormCastIToF32, FormCastIToF64,
    FormCastF32ToI, FormCastF64ToI,
    FormCastF32ToF64, FormCastF64ToF32,

    FormBitcastF32ToI, FormBitcastF64ToI,
    FormBitcastIToF32, FormBitcastIToF64,
    FormBitcastF32,    FormBitcastF64,

    FormNeg,
    FormNot,

    FormAddReg, FormAddImm,
    FormSubReg, FormSubImm,
    FormAndReg, FormAndImm,
    FormOrReg,  FormOrImm,
    FormXorReg, FormXorImm,

    FormAddInc, FormAddDec,
    FormSubInc, FormSubDec,

    FormMul,
    FormDiv,
    FormIDiv,
    FormRem,
    FormIRem,
    FormIMulReg,
    FormIMulImm,

    FormShlImm, FormShlOne, FormShlCl,
    FormShrImm, FormShrOne, FormShrCl,
    FormSarImm, FormSarOne, FormSarCl,

    FormCmpReg,
    FormCmpRegSet,
    FormCmpImm,
    FormCmpImmSet,

    FormFAdd32, FormFAdd64,
    FormFSub32, FormFSub64,
    FormFMul32, FormFMul64,
    FormFDiv32, FormFDiv64,
    FormFNeg32, FormFNeg64,

    FormFCmp32, FormFCmp32Set,
    FormFCmp64, FormFCmp64Set,

    FormSelectFlags,
    FormSelectReg,
    FormSelectFloat32Flags, FormSelectFloat64Flags,
    FormSelectFloat32Reg,   FormSelectFloat64Reg,

    FormAllocaFixed,
    FormAllocaDynamic,

    FormLoad8, FormLoad8S,
    FormLoad16, FormLoad16S,
    FormLoad32, FormLoad32S,
    FormLoad64,
    FormLoadF32, FormLoadF64,

    FormStore8, FormStore16, FormStore32, FormStore64,
    FormStoreF32, FormStoreF64,

    FormBlockCopyRep,
    FormBlockCopyUnrolled,
    FormBlockSetRep,
    FormBlockSetUnrolled,

    FormCallDirect,
    FormCallIndirect,
    FormSyscall,
    FormPushArgReg,
    FormPushArgF32,
    FormPushArgF64,
    FormPushArgImm,

    FormLea,

    FormJmp,
    FormJccFlags,
    FormJccReg,
    FormRet,

    kMachineFormCount,
};

/*
 * Encoding shorthands.
 *
 * Each one names the bytes and the operand-to-field mapping of one encoding shape. Emission walks
 * these; nothing below is a function the encoder has to know the name of.
 */

// `op reg, r/m`, with the two ModRM fields taken from the named operands. `alt` is the same
// operation with those fields swapped, which is what an operand left in a frame slot takes: a memory
// operand has to occupy the r/m field, so whichever operand needs a register moves into the reg one.
// Zero for an operation encoded in only one direction.
static EncodingDescriptor regRm(U8 opcode, OperandRef reg, OperandRef rm, U8 alt = 0) {
    return EncodingDescriptor {
        .family = EncodingFamily::RegRm,
        .opcode = opcode, .opcodeAlt = alt,
        .regField = reg, .rmField = rm,
    };
}

// `op r/m` with an opcode extension in the ModRM.reg field, for the one-operand shapes that have no
// second register: neg, not, mul, div, inc, and the shifts by one or by cl.
static EncodingDescriptor rmExt(U8 opcode, U8 extension, OperandRef rm) {
    return EncodingDescriptor {
        .family = EncodingFamily::RmExt,
        .opcode = opcode, .extension = extension,
        .rmField = rm,
    };
}

// `op r/m, imm`. `imm32` is the wider encoding of the same operation, or zero for one that carries
// an 8-bit immediate only.
static EncodingDescriptor rmExtImm(U8 imm8, U8 imm32, U8 extension, OperandRef rm, OperandRef imm) {
    return EncodingDescriptor {
        .family = EncodingFamily::RmExtImm,
        .opcode = imm8, .opcodeAlt = imm32, .extension = extension,
        .rmField = rm, .immField = imm,
    };
}

// `op xmm, xmm/m` - the SSE two-byte shape, where the mandatory prefix is the width and there is no
// direction bit: the destination is always the ModRM.reg field, so an operand left in the frame can
// only ever be the one this puts in r/m. `prefix` is 0xf3 for the single-precision form, 0xf2 for
// the double-precision one, 0x66 for a packed-double or integer one, and zero for packed single.
static EncodingDescriptor sseRegRm(U8 prefix, U8 opcode, OperandRef reg, OperandRef rm, OperationWidth width) {
    return EncodingDescriptor {
        .family = EncodingFamily::RegRm,
        .opcode = opcode,
        .escape = 0x0f, .prefix = prefix,
        .regField = reg, .rmField = rm,
        .width = width,
        .widthInPrefix = true,
    };
}

/*
 * The table.
 */

MachineTarget::MachineTarget() {
    for(Size i = 0; i < kMachineOpcodeCount; i++) opcodes[i].id = MachineOpcodeId(i);

    auto name = [&](MachineOpcodeId id, StringView text, bool flagsSelective = false) {
        opcodes[id].name = text;
        opcodes[id].flagsSelective = flagsSelective;
    };

    name(OpNone, "none"_v);
    name(OpNop, "nop"_v);
    name(OpArg, "arg"_v);
    name(OpPhi, "phi"_v);

    // An immediate of zero is materialized with `xor r, r` rather than `mov r, 0` - two bytes
    // instead of five, at the cost of writing the flags, which the move does not. Which of the two
    // it is depends on the value alone and on nothing any peephole decides, which is what makes it
    // safe for the compare folding to ask this question while those passes are still running.
    name(OpImm, "imm"_v, true);

    name(OpGlobalAddress, "globaladdr"_v);
    name(OpFunctionAddress, "funaddr"_v);
    name(OpMove, "move"_v);
    name(OpCast, "cast"_v);
    name(OpBitcast, "bitcast"_v);
    name(OpNeg, "neg"_v);
    name(OpNot, "not"_v);
    name(OpAdd, "add"_v);
    name(OpSub, "sub"_v);
    name(OpMul, "mul"_v);
    name(OpIMul, "imul"_v);
    name(OpDiv, "div"_v);
    name(OpIDiv, "idiv"_v);
    name(OpRem, "rem"_v);
    name(OpIRem, "irem"_v);
    name(OpShl, "shl"_v);
    name(OpShr, "shr"_v);
    name(OpSar, "sar"_v);
    name(OpAnd, "and"_v);
    name(OpOr, "or"_v);
    name(OpXor, "xor"_v);
    name(OpCmp, "cmp"_v);

    name(OpFAdd, "fadd"_v);
    name(OpFSub, "fsub"_v);
    name(OpFMul, "fmul"_v);
    name(OpFDiv, "fdiv"_v);
    name(OpFNeg, "fneg"_v);
    name(OpFCmp, "fcmp"_v);

    // A select whose condition arrives in a register tests it first, and that test writes the flags;
    // one whose condition is already in the flags reads them and writes nothing.
    name(OpSelect, "select"_v, true);

    // A compile-time size is one `lea` and touches nothing; a run-time one rounds the size up and
    // moves the stack pointer, which writes the flags. Which of the two applies follows the count
    // being an embedded constant, so this is one of the opcodes whose selection a peephole moves -
    // see MachineOpcodeDesc::flagsSelective for why that is still safe.
    name(OpAlloca, "alloca"_v, true);

    name(OpLoad, "load"_v);
    name(OpStore, "store"_v);
    name(OpBlockCopy, "blockcopy"_v);
    name(OpBlockSet, "blockset"_v);
    name(OpCall, "call"_v);
    name(OpPushArg, "pusharg"_v);
    name(OpAddress, "address"_v);
    name(OpLea, "lea"_v);
    name(OpJmp, "jmp"_v);

    // As with the select above: a branch on a register tests it, a branch on the flags does not.
    name(OpJcc, "jcc"_v, true);

    name(OpRet, "ret"_v);

    // Each form is pushed in the order the ids above declare it, so that the id is its index.
    auto add = [&](MachineFormId id, MachineOpcodeId opcode, StringView formName) -> MachineForm& {
        assertTrue(forms.size() == id); // the form ids and the construction order have drifted apart

        forms.push(MachineForm {});

        auto& form = forms[forms.size() - 1];
        form.id = id;
        form.opcode = opcode;
        form.name = formName;
        return form;
    };

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

    symbolAddress(FormGlobalAddress, OpGlobalAddress, "lea r, [rip + global]"_v);
    symbolAddress(FormFunctionAddress, OpFunctionAddress, "lea r, [rip + fun]"_v);

    // A direct call encodes its target as a rel32 and never reads the address out of a register, so
    // the address is not materialized at all.
    add(FormFunctionImplicit, OpFunctionAddress, "funaddr (elided)"_v).defs.push(noDef());

    /*
     * Moves and casts.
     */

    {
        // MOV r, r/m: a source still in the frame is read in place rather than reloaded into a
        // register the copy would then read again.
        auto& form = add(FormMove, OpMove, "mov r, r/m"_v);
        form.uses.push(regOrMem(MemoryAccessKind::Read));
        form.defs.push(def());
        form.encoding = regRm(0x8b, defRef(0), useRef(0));
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
        // Widening a signed value into a signed one is the one case that has to carry the sign bit
        // up rather than clear the upper half.
        auto& form = add(FormCastSext, OpCast, "movsxd r, r"_v);
        form.uses.push(anyReg());
        form.defs.push(def());
        form.encoding = regRm(0x63, defRef(0), useRef(0));
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
     * The group-1 ALU operations.
     *
     * Two forms each. The register one can take either operand from memory - `add [slot], rcx` when
     * the result lives in that very slot, or `add rax, [slot]` when the right-hand side does - and
     * only one of the two at a time, because both want the single r/m field. The immediate one has
     * no register right-hand side to take from anywhere.
     */

    auto binaryAlu = [&](MachineFormId regId, MachineFormId immId, MachineOpcodeId opcode,
                         StringView regName, StringView immName, U8 rmRegOp, U8 regRmOp, U8 extension)
    {
        auto& regForm = add(regId, opcode, regName);
        regForm.uses.push(regOrMem(MemoryAccessKind::ReadWrite));
        regForm.uses.push(regOrMem(MemoryAccessKind::Read));
        regForm.defs.push(tiedDef(0));
        regForm.flagsEffect = FlagsEffect::Def;
        regForm.encoding = regRm(rmRegOp, useRef(1), useRef(0), regRmOp);

        auto& immForm = add(immId, opcode, immName);
        immForm.uses.push(regOrMem(MemoryAccessKind::ReadWrite));
        immForm.uses.push(immediate(ImmediateWidth::Imm8OrImm32));
        immForm.defs.push(tiedDef(0));
        immForm.flagsEffect = FlagsEffect::Def;
        immForm.encoding = rmExtImm(0x83, 0x81, extension, useRef(0), useRef(1));
    };

    binaryAlu(FormAddReg, FormAddImm, OpAdd, "add r/m, r"_v, "add r/m, imm"_v, 0x01, 0x03, 0);
    binaryAlu(FormSubReg, FormSubImm, OpSub, "sub r/m, r"_v, "sub r/m, imm"_v, 0x29, 0x2b, 5);
    binaryAlu(FormAndReg, FormAndImm, OpAnd, "and r/m, r"_v, "and r/m, imm"_v, 0x21, 0x23, 4);
    binaryAlu(FormOrReg, FormOrImm, OpOr, "or r/m, r"_v, "or r/m, imm"_v, 0x09, 0x0b, 1);
    binaryAlu(FormXorReg, FormXorImm, OpXor, "xor r/m, r"_v, "xor r/m, imm"_v, 0x31, 0x33, 6);

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

    // A comparison against zero has a shorter equivalent in `test r, r`, which leaves every
    // condition code this backend reads in the same state. It needs the value in a register, so the
    // descriptor states it as the alternative and an operand still in the frame keeps the `cmp`.
    auto cmpImm = rmExtImm(0x83, 0x81, 7, useRef(0), useRef(1));
    cmpImm.zeroRegOpcode = 0x85;

    compare(FormCmpImm, FormCmpImmSet, "cmp r/m, imm"_v, "cmp r/m, imm; setcc r"_v,
        immediate(ImmediateWidth::Imm8OrImm32), cmpImm);

    /*
     * Scalar floating-point arithmetic.
     *
     * Destructive in the same way the group-1 integer operations are, and constrained rather more:
     * there is only one direction, so the operand that may stay in the frame is always the
     * right-hand side, and there is no immediate form at all - which is why a float constant is
     * never embedded and always materialized (see isEmbeddableImm in transform.cpp).
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
     * AMD64 has no scalar float negate. The usual expansion exclusive-ors against a sign mask held
     * in a second vector register, which would need either a constant pool this backend has no
     * section for or a vector scratch register nothing reserves - so the sign bit is toggled in a
     * general register instead, which needs one scratch this backend already knows how to hold back
     * at a single instruction: three instructions and one clobber, against three instructions and a
     * whole-function reservation.
     *
     * `btc` writes the carry flag, which is why these declare a flags effect at all where the rest
     * of the floating-point arithmetic declares none.
     */

    auto floatNeg = [&](MachineFormId id, StringView formName, RegisterClassId cls, OperationWidth width) {
        auto& form = add(id, OpFNeg, formName);
        form.uses.push(anyReg(cls));
        form.defs.push(tiedDef(0, cls));
        form.clobbers.add(gpr(IntRegister::r11));
        form.flagsEffect = FlagsEffect::Clobber;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::FloatNeg,
            .width = width,
        };
    };

    floatNeg(FormFNeg32, "movd r, xmm; btc r, 31; movd xmm, r"_v, ClassFloat32, OperationWidth::Fixed32);
    floatNeg(FormFNeg64, "movq r, xmm; btc r, 63; movq xmm, r"_v, ClassFloat64, OperationWidth::Fixed64);

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

    /*
     * Select.
     */

    {
        // CMOVcc r, r/m: the destination doubles as a source, since it keeps its own value when the
        // condition does not hold. The flags were set by a comparison this select consumed.
        auto& form = add(FormSelectFlags, OpSelect, "cmovcc r, r"_v);
        form.uses.push(anyReg());
        form.uses.push(anyReg());
        form.uses.push(folded()); // the condition was consumed by the comparison that set the flags
        form.defs.push(tiedDef(0));
        form.flagsEffect = FlagsEffect::Use;

        // `select` yields its first operand when the condition holds and its second otherwise. The
        // tie has already put the first in the destination, so the move that remains is the second
        // one - which is why the condition the encoding carries is the negated one.
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Conditional,
            .opcode = 0x40, .escape = 0x0f,
            .regField = defRef(0), .rmField = useRef(1),
            .negateCondition = true,
        };
    }

    {
        // The condition arrived in a register instead, so it is tested first - and that test writes
        // the flags, which is why this form and the one above disagree about them.
        auto& form = add(FormSelectReg, OpSelect, "test r, r; cmovcc r, r"_v);
        form.uses.push(anyReg());
        form.uses.push(anyReg());
        form.uses.push(anyReg(ClassGpr32));
        form.defs.push(tiedDef(0));
        form.flagsEffect = FlagsEffect::UseDef;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Conditional,
            .opcode = 0x40, .escape = 0x0f,
            .regField = defRef(0), .rmField = useRef(1),
            .prelude = EncodingPrelude::TestLastUse,
            .negateCondition = true,
        };
    }

    /*
     * Floating-point select.
     *
     * There is no CMOVcc for a vector register, so the conditional move is a conditional *branch*
     * over an unconditional one: the tie has already put the first operand in the destination, so
     * what is left is to skip the copy of the second when the condition holds. Two instructions and
     * a forward jump of known length, against the shuffle-and-blend sequence the alternative would
     * be - which would need a mask in a third vector register and SSE4.1 besides.
     *
     * The copy is MOVAPS rather than MOVSS/MOVSD: it moves the whole register, so one form serves
     * both widths, and it needs no prefix to say which.
     */

    auto floatSelect = [&](MachineFormId id, StringView formName, RegisterClassId cls, bool testCondition) {
        auto& form = add(id, OpSelect, formName);
        form.uses.push(anyReg(cls));
        form.uses.push(anyReg(cls));

        // As for the integer select: a condition already in the flags was consumed by the
        // comparison that set them, and one still in a register is tested here.
        if(testCondition) form.uses.push(anyReg(ClassGpr32));
        else form.uses.push(folded());

        form.defs.push(tiedDef(0, cls));
        form.flagsEffect = testCondition ? FlagsEffect::UseDef : FlagsEffect::Use;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::FloatSelect,
            .opcode = 0x28, .escape = 0x0f,
            .regField = defRef(0), .rmField = useRef(1),
            .width = OperationWidth::Fixed32,
            .prelude = testCondition ? EncodingPrelude::TestLastUse : EncodingPrelude::None,
        };
    };

    floatSelect(FormSelectFloat32Flags, "jcc over; movaps xmm, xmm"_v, ClassFloat32, false);
    floatSelect(FormSelectFloat64Flags, "jcc over; movaps xmm, xmm"_v, ClassFloat64, false);
    floatSelect(FormSelectFloat32Reg, "test r, r; jcc over; movaps xmm, xmm"_v, ClassFloat32, true);
    floatSelect(FormSelectFloat64Reg, "test r, r; jcc over; movaps xmm, xmm"_v, ClassFloat64, true);

    /*
     * Stack allocation.
     *
     * A compile-time size becomes a frame object and one `lea`, which leaves the flags alone; a size
     * only known at run time has to round itself up and move the stack pointer, which does not. So
     * the two forms disagree about the flags, and OpAlloca says so.
     */

    {
        auto& form = add(FormAllocaFixed, OpAlloca, "lea r, [frame]"_v);
        form.uses.push(immediate(ImmediateWidth::Imm64));
        form.defs.push(def());
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::AllocaFixed,
        };
    }

    {
        auto& form = add(FormAllocaDynamic, OpAlloca, "sub rsp, r"_v);
        form.uses.push(anyReg());
        form.defs.push(def());
        form.flagsEffect = FlagsEffect::Clobber;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::AllocaDynamic,
        };
    }

    /*
     * Memory access.
     *
     * One form per access width, because the width decides the bytes: a narrow load has to extend
     * into the whole destination register rather than merge with what it held, which is a different
     * opcode rather than a different operand size. A store only writes the bytes it names, so it
     * needs nothing but the right size - and, at one byte, the REX prefix that names spl/bpl/sil/dil
     * rather than ah/ch/dh/bh.
     */

    auto load = [&](MachineFormId id, StringView formName, U8 opcode, U8 escape, OperationWidth width) {
        auto& form = add(id, OpLoad, formName);
        form.uses.push(address());
        form.defs.push(def());
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::LoadStore,
            .opcode = opcode, .escape = escape,
            .regField = defRef(0),
            .width = width,
        };
    };

    // The narrow loads take their operand size from the *result*, since that is the register the
    // extension has to fill; the wider ones are the width they load.
    load(FormLoad8, "movzx r, byte [address]"_v, 0xb6, 0x0f, OperationWidth::FromResult);
    load(FormLoad8S, "movsx r, byte [address]"_v, 0xbe, 0x0f, OperationWidth::FromResult);
    load(FormLoad16, "movzx r, word [address]"_v, 0xb7, 0x0f, OperationWidth::FromResult);
    load(FormLoad16S, "movsx r, word [address]"_v, 0xbf, 0x0f, OperationWidth::FromResult);
    load(FormLoad32, "mov r32, [address]"_v, 0x8b, 0, OperationWidth::Fixed32);
    load(FormLoad32S, "movsxd r64, [address]"_v, 0x63, 0, OperationWidth::Fixed64);
    load(FormLoad64, "mov r64, [address]"_v, 0x8b, 0, OperationWidth::Fixed64);

    // The float loads say their width in the prefix like every other SSE form, so there is one per
    // width rather than one per (width, signedness): a float is never sign- or zero-extended by
    // being loaded, and a narrower one is a different type rather than a narrower access.
    auto floatLoad = [&](MachineFormId id, StringView formName, U8 prefix, RegisterClassId cls) {
        auto& form = add(id, OpLoad, formName);
        form.uses.push(address());
        form.defs.push(def(cls));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::LoadStore,
            .opcode = 0x10, .escape = 0x0f, .prefix = prefix,
            .regField = defRef(0),
            .widthInPrefix = true,
        };
    };

    floatLoad(FormLoadF32, "movss xmm, [address]"_v, 0xf3, ClassFloat32);
    floatLoad(FormLoadF64, "movsd xmm, [address]"_v, 0xf2, ClassFloat64);

    auto store = [&](MachineFormId id, StringView formName, U8 opcode, OperationWidth width) {
        auto& form = add(id, OpStore, formName);
        form.uses.push(address());
        form.uses.push(anyReg());
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::LoadStore,
            .opcode = opcode,
            .regField = useRef(1),
            .width = width,
        };

        return &form;
    };

    store(FormStore8, "mov byte [address], r"_v, 0x88, OperationWidth::Fixed32)->encoding.byteRegField = true;
    store(FormStore16, "mov word [address], r"_v, 0x89, OperationWidth::Fixed32)->encoding.prefix = 0x66;
    store(FormStore32, "mov dword [address], r"_v, 0x89, OperationWidth::Fixed32);
    store(FormStore64, "mov qword [address], r"_v, 0x89, OperationWidth::Fixed64);

    // A store has no result to take its width from, so it states it - and states it as the width of
    // the value rather than of the address, which is what the prefix already says in bytes.
    auto floatStore = [&](MachineFormId id, StringView formName, U8 prefix, RegisterClassId cls,
                          OperationWidth width)
    {
        auto& form = add(id, OpStore, formName);
        form.uses.push(address());
        form.uses.push(anyReg(cls));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::LoadStore,
            .opcode = 0x11, .escape = 0x0f, .prefix = prefix,
            .regField = useRef(1),
            .width = width,
            .widthInPrefix = true,
        };
    };

    floatStore(FormStoreF32, "movss [address], xmm"_v, 0xf3, ClassFloat32, OperationWidth::Fixed32);
    floatStore(FormStoreF64, "movsd [address], xmm"_v, 0xf2, ClassFloat64, OperationWidth::Fixed64);

    /*
     * Block operations.
     *
     * Two encodings with very different register requirements: `rep movsb`/`rep stosb` demand fixed
     * registers and consume them as they run, while the unrolled form works out of whatever
     * registers the operands already occupy. Which one applies is chosen once by the transform
     * pipeline and recorded on the instruction.
     */

    {
        auto& form = add(FormBlockCopyRep, OpBlockCopy, "rep movsb"_v);
        form.uses.push(fixedReg(IntRegister::rdi));
        form.uses.push(fixedReg(IntRegister::rsi));
        form.uses.push(fixedReg(IntRegister::rcx));

        // Consumed as it runs: rdi and rsi are left pointing past the copied region and rcx is
        // counted down to zero, so a value still live afterwards has to be copied somewhere safe
        // first instead of being read back advanced.
        form.clobbers.add(gpr(IntRegister::rdi));
        form.clobbers.add(gpr(IntRegister::rsi));
        form.clobbers.add(gpr(IntRegister::rcx));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::BlockCopyRep,
        };
    }

    {
        // The unrolled form needs one general register to carry each word through, and states it as a
        // clobber of a fixed register rather than as a declared temporary (MachineForm::temporaries).
        // The two would reserve it at different scopes: a clobber keeps a live value out of r11 at
        // this one instruction, where a declared temporary is held back from the whole function.
        auto& form = add(FormBlockCopyUnrolled, OpBlockCopy, "mov (unrolled)"_v);
        form.uses.push(anyReg());
        form.uses.push(anyReg());
        form.clobbers.add(gpr(IntRegister::r11));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::BlockCopyUnrolled,
        };
    }

    {
        // Positional, matching the instruction's own operand order (to, count, pattern) rather than
        // the rdi/rax/rcx order `rep stosb` reads them in.
        auto& form = add(FormBlockSetRep, OpBlockSet, "rep stosb"_v);
        form.uses.push(fixedReg(IntRegister::rdi));
        form.uses.push(fixedReg(IntRegister::rcx));
        form.uses.push(fixedReg(IntRegister::rax));

        // rdi is advanced past the filled region and rcx counted down; rax is only read.
        form.clobbers.add(gpr(IntRegister::rdi));
        form.clobbers.add(gpr(IntRegister::rcx));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::BlockSetRep,
        };
    }

    {
        auto& form = add(FormBlockSetUnrolled, OpBlockSet, "mov (unrolled)"_v);
        form.uses.push(anyReg());
        form.uses.push(anyReg());
        form.uses.push(anyReg());
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::BlockSetUnrolled,
        };
    }

    /*
     * Calls.
     *
     * Operand and result locations come from the selected calling convention rather than from a
     * table here: where an argument goes depends on how many of each bank came before it, which a
     * flat list cannot say. The clobber set comes from the same place.
     */

    auto call = [&](MachineFormId id, StringView formName, PseudoKind pseudo) {
        auto& form = add(id, OpCall, formName);
        form.conventionOperands = true;
        form.flagsEffect = FlagsEffect::Clobber;
        form.encoding = EncodingDescriptor { .family = EncodingFamily::Pseudo, .pseudo = pseudo };
    };

    call(FormCallDirect, "call rel32"_v, PseudoKind::CallDirect);
    call(FormCallIndirect, "call r/m"_v, PseudoKind::CallIndirect);
    call(FormSyscall, "syscall"_v, PseudoKind::Syscall);

    // The argument area is addressed through rsp, at the offset the convention assigned - an address
    // the legalizer resolves like any other, so this is an ordinary store.
    {
        auto& form = add(FormPushArgReg, OpPushArg, "mov [rsp + n], r"_v);
        form.uses.push(anyReg());
        form.defs.push(noDef()); // stands in for the argument in the call's operand list
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::LoadStore,
            .opcode = 0x89,
            .regField = useRef(0),
            .width = OperationWidth::Fixed64,
        };
    }

    // A float argument is stored by the instruction that owns its bank, at its own width: the slot
    // is eight bytes wide whatever goes in it, and the callee reads back exactly the four or eight
    // the convention put there.
    auto floatPushArg = [&](MachineFormId id, StringView formName, U8 prefix, RegisterClassId cls,
                            OperationWidth width)
    {
        auto& form = add(id, OpPushArg, formName);
        form.uses.push(anyReg(cls));
        form.defs.push(noDef());
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::LoadStore,
            .opcode = 0x11, .escape = 0x0f, .prefix = prefix,
            .regField = useRef(0),
            .width = width,
            .widthInPrefix = true,
        };
    };

    floatPushArg(FormPushArgF32, "movss [rsp + n], xmm"_v, 0xf3, ClassFloat32, OperationWidth::Fixed32);
    floatPushArg(FormPushArgF64, "movsd [rsp + n], xmm"_v, 0xf2, ClassFloat64, OperationWidth::Fixed64);

    {
        // MOV r/m64, imm32 sign-extends, which is what a narrower constant occupying a full 8-byte
        // argument slot wants anyway.
        auto& form = add(FormPushArgImm, OpPushArg, "mov [rsp + n], imm"_v);
        form.uses.push(immediate(ImmediateWidth::Imm32));
        form.defs.push(noDef());
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::LoadStore,
            .opcode = 0xc7, .extension = 0,
            .immField = useRef(0),
            .width = OperationWidth::Fixed64,
        };
    }

    /*
     * The remaining target operations.
     */

    {
        auto& form = add(FormLea, OpLea, "lea r, [address]"_v);
        form.defs.push(def());
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Lea,
            .opcode = 0x8d,
            .regField = defRef(0),
            .width = OperationWidth::Fixed64,
        };
    }

    /*
     * Terminators.
     *
     * All four are pseudos: which bytes a branch takes depends on which of its successors the block
     * order put next, and a return has to emit the epilogue the frame layout decided on.
     */

    add(FormJmp, OpJmp, "jmp rel32"_v).encoding = EncodingDescriptor {
        .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::Jump,
    };

    {
        auto& form = add(FormJccFlags, OpJcc, "jcc rel32"_v);
        form.uses.push(folded()); // the condition was consumed by the comparison that set the flags
        form.flagsEffect = FlagsEffect::Use;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::Branch,
        };
    }

    {
        auto& form = add(FormJccReg, OpJcc, "test r, r; jcc rel32"_v);
        form.uses.push(anyReg(ClassGpr32));
        form.flagsEffect = FlagsEffect::UseDef;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::Branch,
            .prelude = EncodingPrelude::TestLastUse,
        };
    }

    {
        // A return's operands are the function's results, placed by the result half of its own
        // convention. Nothing is live once the function has returned, so it clobbers nothing.
        auto& form = add(FormRet, OpRet, "ret"_v);
        form.conventionOperands = true;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::Return,
        };
    }

    assertTrue(forms.size() == kMachineFormCount);

    // The intrinsics' forms go into the same table, after the described ones, so that everything
    // downstream asks an intrinsic the same questions it asks an `add` - see intrinsic.cpp.
    addIntrinsics(*this);

    assertTrue(validateMachineForms(*this));
    assertTrue(validateIntrinsics(*this));
}

const MachineTarget& machineTarget() {
    static MachineTarget target;
    return target;
}

/*
 * Validation.
 */

bool validateMachineForms(const MachineTarget& target) {
    auto& registers = targetRegisters();
    auto ok = true;

    auto fail = [&](const MachineForm& form, StringView what) {
        ok = false;
        logError("machine form \"%@\": %@", form.name, what);
    };

    // Every opcode has at least one form, and every form belongs to an opcode that exists.
    bool hasForm[kMachineOpcodeCount] = {};

    for(auto& form: target.forms) {
        if(form.opcode >= kMachineOpcodeCount) {
            fail(form, "names an opcode that does not exist"_v);
            continue;
        }

        hasForm[form.opcode] = true;

        auto checkOperand = [&](const MachineOperandConstraint& c, bool isDef) {
            if(c.regClass >= kRegisterClassCount) {
                fail(form, "names a register class that does not exist"_v);
                return;
            }

            auto& cls = registers.regClass(c.regClass);

            // A fixed register has to be one a value of that operand's class could have been given
            // in the first place: a form demanding rsp, or a general register for a vector operand,
            // is describing a machine that does not exist.
            if(c.kind == OperandConstraintKind::FixedRegister) {
                if(c.fixedReg.bank != cls.bank || !cls.allowedPhysical.has(c.fixedReg)) {
                    fail(form, "fixes an operand to a register its class cannot occupy"_v);
                }
            }

            // Every register the subset names has to be one the class already allows: the subset
            // narrows a class rather than reaching outside it.
            if(c.kind == OperandConstraintKind::RegisterSubset) {
                if(!((c.allowed & cls.allowedPhysical) == c.allowed)) {
                    fail(form, "allows an operand a register outside its class"_v);
                }
            }

            // A tie joins a def to a use that exists, and only a def may be tied.
            if(c.tiedOperand != kNoTiedOperand) {
                if(!isDef) fail(form, "ties a use to another operand"_v);
                if(c.tiedOperand >= form.uses.size()) fail(form, "ties a result to an operand that does not exist"_v);
            }

            if(c.kind == OperandConstraintKind::ReuseOperand && c.tiedOperand == kNoTiedOperand) {
                fail(form, "reuses an operand without saying which"_v);
            }

            if((c.kind == OperandConstraintKind::Immediate) != (c.immediate != ImmediateWidth::None)) {
                fail(form, "states an immediate width for an operand that is not one, or the reverse"_v);
            }

            auto isMemory = c.kind == OperandConstraintKind::RegisterOrMemory
                || c.kind == OperandConstraintKind::Memory;

            if(isMemory != (c.memoryAccess != MemoryAccessKind::None)) {
                fail(form, "states a memory access for an operand that has none, or the reverse"_v);
            }

            /*
             * Descriptor fields no generic pass implements yet.
             *
             * Each of these is part of the representation because the first form that needs it must
             * be able to say so rather than being handled by a special case. But a field placement
             * and legalization do not read is worse than one that does not exist: a form using it
             * looks complete while half of what it declares is silently ignored. So a use of one is
             * rejected here until the pass that would honour it exists, which turns "adding this
             * instruction needs allocator work" into a build failure rather than into wrong code.
             */

            // Placement is first-fit over a class's registers and does not narrow by `allowed`.
            if(c.kind == OperandConstraintKind::RegisterSubset) {
                fail(form, "restricts an operand to a register subset, which placement does not implement"_v);
            }

            // Legalization can leave an operand in a frame slot, but has no rule for one that may
            // *only* be in memory - there is nothing to reload it into and nothing to spill.
            if(c.kind == OperandConstraintKind::Memory) {
                fail(form, "requires an operand in memory, which legalization does not implement"_v);
            }

            // A write-only memory operand would be a result written to a slot the instruction never
            // read, which nothing produces and legalization's in-place rule does not cover.
            if(c.memoryAccess == MemoryAccessKind::Write) {
                fail(form, "writes an operand in memory without reading it, which legalization does not implement"_v);
            }

            // Every form described so far reads all of its operands before writing any result, and
            // placement's rule for a tied result assumes exactly that.
            auto defaultTiming = isDef ? OperandTiming::LateDef : OperandTiming::EarlyUse;
            if(c.timing != defaultTiming) {
                fail(form, "states an operand timing that placement does not implement"_v);
            }
        };

        for(auto& c: form.uses) checkOperand(c, false);
        for(auto& c: form.defs) checkOperand(c, true);

        // At most one operand may be taken from memory at any one instruction, and each of the two
        // roles is named at most once: a general memory operand occupies the r/m field, and there is
        // one of those.
        Size reads = 0, readWrites = 0;
        for(auto& c: form.uses) {
            if(c.kind != OperandConstraintKind::RegisterOrMemory) continue;
            if(c.memoryAccess == MemoryAccessKind::Read) reads++;
            if(c.memoryAccess == MemoryAccessKind::ReadWrite) readWrites++;
        }

        if(reads > 1 || readWrites > 1) fail(form, "names more than one operand for the single r/m field"_v);

        // A form that can write its r/m operand in place has to say which result goes there, since
        // the operand and the result then have to occupy one slot.
        if(readWrites > 0 && form.tiedResult() != form.memoryDef()) {
            fail(form, "writes an operand in place without tying its result to it"_v);
        }

        /*
         * The encoding descriptor.
         */

        auto& encoding = form.encoding;

        // Every field the encoding names has to be an operand the instruction actually has, since
        // emission indexes the resolved operands by these without looking at anything else.
        auto checkField = [&](OperandRef ref, StringView what) {
            if(ref.isNone()) return;

            auto& list = ref.result ? form.defs : form.uses;
            if(Size(ref.index) >= list.size()) fail(form, what);
        };

        checkField(encoding.regField, "names a ModRM.reg field that is not an operand of it"_v);
        checkField(encoding.rmField, "names an r/m field that is not an operand of it"_v);
        checkField(encoding.immField, "names an immediate field that is not an operand of it"_v);

        // An immediate field has to name an operand the form declared as one, or - for a constant
        // materialization, whose immediate is the value it defines rather than an operand - a
        // result. Otherwise the encoding would be writing bytes for something with no value.
        if(!encoding.immField.isNone() && !encoding.immField.result) {
            auto& constraint = form.uses[encoding.immField.index];
            if(constraint.kind != OperandConstraintKind::Immediate) {
                fail(form, "encodes an immediate from an operand that is not one"_v);
            }
        }

        // The r/m field is the one that may hold a memory operand, so a form with a memory
        // alternative has to encode that operand there and nowhere else.
        auto memoryOperand = form.memoryUse() != -1 ? form.memoryUse() : form.memoryDef();
        if(memoryOperand != -1 && encoding.family != EncodingFamily::Pseudo) {
            auto& rm = encoding.rmField;
            auto& reg = encoding.regField;

            auto encodable = (!rm.isNone() && !rm.result && rm.index == memoryOperand)
                || (encoding.opcodeAlt != 0 && !reg.isNone() && !reg.result && reg.index == memoryOperand);

            if(!encodable) fail(form, "allows an operand in memory that its encoding cannot address"_v);
        }

        // A width taken from an operand needs that operand to exist, whatever the encoding does with
        // it: the memory-operand rules ask the same question to decide whether a slot fits.
        auto width = encoding.width;
        if((width == OperationWidth::FromUse0 || width == OperationWidth::Narrowest) && form.uses.isEmpty()) {
            fail(form, "takes its width from an operand that does not exist"_v);
        }

        if(width == OperationWidth::FromResult && form.defs.isEmpty() && !form.conventionOperands
            && encoding.family != EncodingFamily::None && encoding.family != EncodingFamily::Pseudo)
        {
            fail(form, "takes its width from a result that does not exist"_v);
        }

        if((encoding.family == EncodingFamily::Pseudo) != (encoding.pseudo != PseudoKind::None)) {
            fail(form, "names a dedicated encoder without being a pseudo, or the reverse"_v);
        }

        // A form with convention-derived operands states no operand constraints of its own: the two
        // would be two answers to one question.
        if(form.conventionOperands && (form.uses.isNotEmpty() || form.defs.isNotEmpty())) {
            fail(form, "states operand constraints as well as taking them from a convention"_v);
        }

        // Implicit effects and clobbers are physical registers by construction, but a clobber that
        // named a reserved register would be an instruction the allocator cannot work around.
        RegSet reserved;
        for(auto& bank: registers.banks) reserved |= bank.reserved;
        if(!(form.clobbers & reserved).isEmpty()) fail(form, "clobbers a reserved register"_v);

        // The other two halves of the same fence as the operand fields above. A register an
        // instruction destroys is expressible today as a clobber, which every pass honours; one it
        // merely *reads* without naming, and one it defines without naming, are read by nothing -
        // so a form stating either would have that effect ignored rather than respected.
        if(!form.implicitUses.isEmpty()) {
            fail(form, "reads a register it does not name, which no pass implements"_v);
        }

        if(!form.implicitDefs.isEmpty()) {
            fail(form, "defines a register it does not name - state it as a clobber instead"_v);
        }

        // And the third: the temporary reserve derives its two pools from what legalization asks for
        // (see TemporaryReserve), and has no pool for a register a form's own *expansion* needs. The
        // one expansion that needs a scratch register today names a fixed one as a clobber instead,
        // which reserves it at the instruction rather than for the whole function.
        for(auto count: form.temporaries.counts) {
            if(count != 0) fail(form, "declares expansion temporaries, which the reserve does not implement"_v);
        }
    }

    for(Size i = 1; i < kMachineOpcodeCount; i++) {
        if(!hasForm[i]) {
            ok = false;
            logError("machine opcode \"%@\" has no form", target.opcodes[i].name);
        }
    }

    // Unless an opcode says its forms differ, they have to agree about the flags. The compare
    // folding in transform.cpp asks what an instruction does to the flags while the peephole passes
    // are still deciding which form it will take, and that question only has one answer if every
    // form of the opcode gives the same one.
    for(Size op = 1; op < kMachineOpcodeCount; op++) {
        if(target.opcodes[op].flagsSelective) continue;

        Maybe<bool> writes;
        for(auto& form: target.forms) {
            if(form.opcode != op) continue;

            auto formWrites = writesFlags(form.flagsEffect);
            if(writes.isNothing()) writes = Just(formWrites);
            else if(writes.unwrap() != formWrites) {
                ok = false;
                logError("machine opcode \"%@\" has forms that disagree about the flags",
                    target.opcodes[op].name);
                break;
            }
        }
    }

    // And the same for the address operand, for the same reason: the address folding runs before
    // the form is settled and asks the *opcode* which operand is an address (opcodeAddressOperand),
    // so a load whose narrow form named its address somewhere else would have the fold rewrite one
    // operand and the encoder read another.
    for(Size op = 1; op < kMachineOpcodeCount; op++) {
        Maybe<I32> address;
        for(auto& form: target.forms) {
            if(form.opcode != op) continue;

            auto formAddress = form.addressOperand();
            if(address.isNothing()) address = Just(formAddress);
            else if(address.unwrap() != formAddress) {
                ok = false;
                logError("machine opcode \"%@\" has forms that disagree about which operand is an address",
                    target.opcodes[op].name);
                break;
            }
        }
    }

    return ok;
}

/*
 * Selection.
 */

MachineOpcodeId opcodeFor(LowerBase base, LowerInst* inst) {
    // Whether this instruction's operands live in the vector bank. Read from the operands rather
    // than from the result, because the two disagree for exactly the operation whose opcode this
    // most needs to decide: a comparison of two floats produces an integer.
    auto isFloatOp = [&] {
        if(inst->createdCount > 0 && isFloat(inst->created()[0].type)) return true;
        return inst->usedCount > 0 && isFloat(base[inst->used()[0]]->type);
    };

    switch(inst->kind) {
        case LowerInst::Arg:        return OpArg;
        case LowerInst::Global:     return OpGlobalAddress;
        case LowerInst::Fun:        return OpFunctionAddress;
        case LowerInst::Imm:        return OpImm;
        case LowerInst::Nop:        return OpNop;
        case LowerInst::Set:        return OpMove;
        case LowerInst::Cast:       return OpCast;
        case LowerInst::Bitcast:    return OpBitcast;
        case LowerInst::Not:        return OpNot;
        case LowerInst::IMul:       return OpIMul;
        case LowerInst::IDiv:       return OpIDiv;
        case LowerInst::Rem:        return OpRem;
        case LowerInst::IRem:       return OpIRem;
        case LowerInst::Shl:        return OpShl;
        case LowerInst::Shr:        return OpShr;
        case LowerInst::Sar:        return OpSar;
        case LowerInst::And:        return OpAnd;
        case LowerInst::Or:         return OpOr;
        case LowerInst::Xor:        return OpXor;

        // The six the IR states once and the machine has twice, one operation per bank.
        case LowerInst::Neg:        return isFloatOp() ? OpFNeg : OpNeg;
        case LowerInst::Add:        return isFloatOp() ? OpFAdd : OpAdd;
        case LowerInst::Sub:        return isFloatOp() ? OpFSub : OpSub;
        case LowerInst::Mul:        return isFloatOp() ? OpFMul : OpMul;
        case LowerInst::Div:        return isFloatOp() ? OpFDiv : OpDiv;
        case LowerInst::Cmp:        return isFloatOp() ? OpFCmp : OpCmp;

        case LowerInst::Select:     return OpSelect;
        case LowerInst::Alloca:     return OpAlloca;
        case LowerInst::Load:       return OpLoad;
        case LowerInst::Store:      return OpStore;
        case LowerInst::Copy:       return OpBlockCopy;
        case LowerInst::SetPattern: return OpBlockSet;
        case LowerInst::Call:       return OpCall;
        case LowerInst::Je:         return OpJcc;
        case LowerInst::Jmp:        return OpJmp;
        case LowerInst::Ret:        return OpRet;
        case LowerInst::Phi:        return OpPhi;
        case LowerInst::X86Address: return OpAddress;
        case LowerInst::X86Lea:     return OpLea;
        case LowerInst::Intrinsic:
            return machineTarget().intrinsic(((LowerInstIntrinsic*)inst)->getIntrinsic()).opcode;
        case LowerInst::X86PushArg: return OpPushArg;
    }

    assertTrue("no machine opcode for this instruction" == nullptr);
    return OpNone;
}

// The right-hand side of a binary operation, as the selector sees it: an immediate the peepholes
// embedded into the encoding, or an operand that still needs a register.
static bool hasEmbeddedRhs(LowerBase base, LowerInst* inst) {
    return isImm(base[((LowerInstBinary*)inst)->rhs]);
}

// The value of an embedded immediate operand. Only asked about operands hasEmbeddedRhs has already
// answered for, so the instruction behind it is an Imm by construction.
static U64 embeddedValue(LowerBase base, LowerPtr<LowerValue> operand) {
    return ((LowerImm*)base[operand]->inst())->i;
}

LowerType operationType(LowerBase base, const MachineForm& form, LowerInst* inst) {
    auto resultType = [&] {
        assertTrue(inst->createdCount > 0); // a form taking its width from a result that does not exist
        return inst->created()[0].type;
    };

    auto firstUseType = [&] {
        assertTrue(inst->usedCount > 0); // a form taking its width from an operand that does not exist
        return base[inst->used()[0]]->type;
    };

    switch(form.encoding.width) {
        case OperationWidth::FromResult: return resultType();
        case OperationWidth::FromUse0:   return firstUseType();
        case OperationWidth::Fixed32:    return LowerType::Int32;
        case OperationWidth::Fixed64:    return LowerType::Int64;

        case OperationWidth::Narrowest:
            // A 32-bit move clears the upper half of its destination, so one encoding both truncates
            // a wide source and zero-extends a narrow one. Using the wider of the two would copy the
            // source's upper half unchanged when widening, propagating whatever it held.
            return is64Bit(firstUseType()) && is64Bit(resultType()) ? resultType() : LowerType::Int32;
    }

    return resultType();
}

Maybe<LowerCmp> selectCondition(LowerInst* inst) {
    switch(inst->kind) {
        case LowerInst::Cmp:
            return Just(((LowerInstCmp*)inst)->getCmp());

        // A condition that arrived in a register rather than in the flags is turned into flags by
        // the `test` the form declares as its prelude, and `test r, r` sets ZF exactly when the
        // register is zero - so "the condition holds" is the not-equal case.
        case LowerInst::Je: {
            auto embedded = ((LowerInstJe*)inst)->getEmbeddedCmp();
            return embedded ? embedded : Just(LowerCmp::neq);
        }

        case LowerInst::Select: {
            auto embedded = ((LowerInstSelect*)inst)->getEmbeddedCmp();
            return embedded ? embedded : Just(LowerCmp::neq);
        }

        default:
            return Nothing();
    }
}

// The rejections belong here rather than in the encoder: a selector that returned an integer form
// for a float operand, or a signed conversion for an unsigned one, would produce a working compile
// of the wrong program, and no later stage could tell.
static void requireIntLike(LowerType type) {
    assertTrue(isIntLike(type)); // an integer form asked for a floating-point operand
}

// One of the two scalar float forms an operation has, chosen by the width its operands work at.
static MachineFormId byFloatWidth(LowerType type, MachineFormId f32, MachineFormId f64) {
    assertTrue(isFloat(type)); // a floating-point form asked for an operand that is not one
    return type == LowerType::Float32 ? f32 : f64;
}

// The form an instruction takes, before the target's own features are consulted.
static MachineFormId selectFormForTarget(LowerBase base, LowerInst* inst);

MachineFormId selectForm(LowerBase base, LowerInst* inst) {
    auto id = selectFormForTarget(base, inst);

    // A form whose encoding needs an extension this build does not have is not selectable, and the
    // rejection belongs here rather than in the encoder: by then the operands have been allocated
    // against the form's constraints and there is nothing left to choose instead. Checked for every
    // form rather than for the intrinsics alone, which is what makes adding a VEX or EVEX
    // alternative a question of listing it with its features rather than of remembering to guard it.
    assertTrue((machineTarget().form(id).requiredFeatures & ~targetFeatures()) == 0);
    return id;
}

static MachineFormId selectFormForTarget(LowerBase base, LowerInst* inst) {
    switch(inst->kind) {
        case LowerInst::Nop:        return FormNop;
        case LowerInst::Arg:        return FormArg;
        case LowerInst::Phi:        return FormPhi;
        case LowerInst::X86Address: return FormAddress;
        case LowerInst::X86Lea:     return FormLea;

        // An intrinsic's form is a row of the registry rather than a case here - see intrinsic.cpp.
        // What is checked at the point of selection is what only the target knows: that this build
        // has the features the encoding needs, and that the values the program gave it are ones the
        // instruction accepts.
        case LowerInst::Intrinsic: {
            auto intrinsic = (LowerInstIntrinsic*)inst;
            auto& desc = machineTarget().intrinsic(intrinsic->getIntrinsic());

            assertTrue(desc.defined); // an intrinsic this target has no description for
            assertTrue((desc.requiredFeatures & ~targetFeatures()) == 0); // ... that it cannot encode
            assertTrue(checkIntrinsicOperands(base, desc, intrinsic)); // ... with operands it cannot take

            return desc.form;
        }

        case LowerInst::Global:     return FormGlobalAddress;

        case LowerInst::X86PushArg: {
            auto arg = base[((LowerInstX86PushArg*)inst)->arg];
            if(isImm(arg)) return FormPushArgImm;
            if(isFloat(arg->type)) return byFloatWidth(arg->type, FormPushArgF32, FormPushArgF64);
            return FormPushArgReg;
        }

        case LowerInst::Imm: {
            // Decided from the value alone. Whether the immediate is embedded is a peephole's
            // answer and may still change; whether it would be materialized with `xor` or with
            // `mov` is not, which is what lets the compare folding read the flags effect early.
            auto imm = (LowerImm*)inst;
            if(isImplicit(&imm->result)) return FormImmImplicit;

            // No SSE encoding carries a float as an immediate, so a float constant is never
            // embedded (isEmbeddableImm says so) and always takes the materializing pseudo.
            auto type = imm->result.type;
            if(isFloat(type)) return byFloatWidth(type, FormImmFloat32, FormImmFloat64);

            return imm->i == 0 ? FormImmZero : FormImmMov;
        }

        case LowerInst::Fun:
            return isImplicit(&((LowerInstFun*)inst)->result) ? FormFunctionImplicit : FormFunctionAddress;

        case LowerInst::Set: {
            auto type = ((LowerInstUnary*)inst)->result.type;
            if(isFloat(type)) return byFloatWidth(type, FormMoveF32, FormMoveF64);
            return FormMove;
        }

        case LowerInst::Cast: {
            auto cast = (LowerInstCast*)inst;
            auto from = base[cast->from]->type;
            auto to = cast->result.type;

            // Between the banks, where only the signed direction has an encoding. An unsigned
            // conversion never reaches here: it is a sequence rather than an instruction, and
            // expandUnsignedConversions replaced it with one made of signed ones before selection
            // ran. Emitting a signed instruction for it instead would be wrong for exactly the
            // values that motivated asking for unsigned in the first place.
            if(isFloat(from) != isFloat(to)) {
                if(isFloat(to)) {
                    assertTrue(cast->isSignedSource()); // an unsigned conversion the expansion missed
                    return byFloatWidth(to, FormCastIToF32, FormCastIToF64);
                }

                assertTrue(cast->isSignedResult()); // an unsigned conversion the expansion missed
                return byFloatWidth(from, FormCastF32ToI, FormCastF64ToI);
            }

            if(isFloat(from)) {
                assertTrue(from != to); // a cast between one float type and itself
                return from == LowerType::Float32 ? FormCastF32ToF64 : FormCastF64ToF32;
            }

            requireIntLike(from);
            requireIntLike(to);
            if(isImm(base[cast->from])) return FormCastImm;

            /*
             * Only a signed value *widened* into a signed one has to carry its sign bit up; every
             * other cast between integer classes is the truncating-and-clearing move.
             *
             * Widening rather than merely signed, because `movsxd` reads a 32-bit source whatever
             * register it is given - it is the 32-to-64 encoding and there is no other. Choosing it
             * for a cast whose source is already 64 bits drops the top half and sign-extends what is
             * left, which is silent: the values it is wrong for are exactly the ones that do not fit
             * in 32 bits. A refinement of a 64-bit type widening to the type it refines - `@bits(40)
             * WideInt` to `WideInt` - is signed at both ends and 64 bits at both ends, and is what
             * reached this.
             */
            auto widens = !is64Bit(from) && is64Bit(to);
            return widens && cast->isSignedSource() && cast->isSignedResult() ? FormCastSext
                                                                             : FormCastMov;
        }

        case LowerInst::Bitcast: {
            auto bitcast = (LowerInstUnary*)inst;
            auto from = base[bitcast->from]->type;
            auto to = bitcast->result.type;

            // A bitcast preserves the width, so crossing the banks is MOVD or MOVQ and which of the
            // two is decided by that width alone.
            if(isFloat(from) != isFloat(to)) {
                assertTrue(is64Bit(from) == is64Bit(to)); // a bitcast between two different widths

                return isFloat(to)
                    ? byFloatWidth(to, FormBitcastIToF32, FormBitcastIToF64)
                    : byFloatWidth(from, FormBitcastF32ToI, FormBitcastF64ToI);
            }

            // Within the vector bank a bitcast is a copy, and one between a register and itself is
            // no instruction at all.
            if(isFloat(from)) {
                assertTrue(from == to); // a bitcast between two float types of different widths
                return byFloatWidth(to, FormBitcastF32, FormBitcastF64);
            }

            requireIntLike(from);
            requireIntLike(to);
            return isImm(base[bitcast->from]) ? FormBitcastImm : FormBitcast;
        }

        case LowerInst::Neg: {
            auto type = ((LowerInstUnary*)inst)->result.type;
            if(isFloat(type)) return byFloatWidth(type, FormFNeg32, FormFNeg64);
            return FormNeg;
        }

        case LowerInst::Not:
            requireIntLike(base[((LowerInstUnary*)inst)->from]->type);
            return FormNot;

        case LowerInst::And: return hasEmbeddedRhs(base, inst) ? FormAndImm : FormAndReg;
        case LowerInst::Or:  return hasEmbeddedRhs(base, inst) ? FormOrImm : FormOrReg;
        case LowerInst::Xor: return hasEmbeddedRhs(base, inst) ? FormXorImm : FormXorReg;

        // A comparison whose result the folding could not leave in the flags has to be materialized
        // into a register afterwards, which is a form of its own rather than a tail the encoder
        // decides to add.
        case LowerInst::Cmp: {
            auto type = base[((LowerInstBinary*)inst)->lhs]->type;
            auto materialize = !isImplicit(&((LowerInstCmp*)inst)->result);

            if(isFloat(type)) {
                return materialize
                    ? byFloatWidth(type, FormFCmp32Set, FormFCmp64Set)
                    : byFloatWidth(type, FormFCmp32, FormFCmp64);
            }

            requireIntLike(type);
            if(hasEmbeddedRhs(base, inst)) return materialize ? FormCmpImmSet : FormCmpImm;
            return materialize ? FormCmpRegSet : FormCmpReg;
        }

        // Add and subtract come through here too, since the two banks share their IR instruction.
        case LowerInst::Add:
        case LowerInst::Sub:
        case LowerInst::Mul:
        case LowerInst::Div: {
            auto type = ((LowerInstBinary*)inst)->result.type;

            if(isFloat(type)) {
                switch(inst->kind) {
                    case LowerInst::Add: return byFloatWidth(type, FormFAdd32, FormFAdd64);
                    case LowerInst::Sub: return byFloatWidth(type, FormFSub32, FormFSub64);
                    case LowerInst::Mul: return byFloatWidth(type, FormFMul32, FormFMul64);
                    default:             return byFloatWidth(type, FormFDiv32, FormFDiv64);
                }
            }

            // An addition or subtraction of one is a byte shorter as `inc`/`dec`, and which of the
            // two it is depends only on the constant - so it is chosen here rather than noticed by
            // the encoder. A subtraction of one decrements, and of minus one increments.
            if(inst->kind == LowerInst::Add || inst->kind == LowerInst::Sub) {
                auto reg = inst->kind == LowerInst::Add ? FormAddReg : FormSubReg;
                if(!hasEmbeddedRhs(base, inst)) return reg;

                auto value = embeddedValue(base, ((LowerInstBinary*)inst)->rhs);
                auto up = inst->kind == LowerInst::Add ? FormAddInc : FormSubInc;
                auto down = inst->kind == LowerInst::Add ? FormAddDec : FormSubDec;

                if(value == 1) return inst->kind == LowerInst::Add ? up : down;
                if(value == U64(I64(-1))) return inst->kind == LowerInst::Add ? down : up;
                return inst->kind == LowerInst::Add ? FormAddImm : FormSubImm;
            }

            // The group-3 multiply and divide read and write the rdx:rax pair, which only the
            // integer encodings have.
            assertTrue(isInt(type)); // no integer form for this type
            return inst->kind == LowerInst::Mul ? FormMul : FormDiv;
        }

        case LowerInst::IDiv:
        case LowerInst::Rem:
        case LowerInst::IRem:
        case LowerInst::IMul: {
            assertTrue(isInt(((LowerInstBinary*)inst)->result.type)); // no integer form for this type

            switch(inst->kind) {
                case LowerInst::IDiv: return FormIDiv;
                case LowerInst::Rem:  return FormRem;
                case LowerInst::IRem: return FormIRem;
                default: return hasEmbeddedRhs(base, inst) ? FormIMulImm : FormIMulReg;
            }
        }

        // A shift by one has an encoding that carries no immediate byte at all.
        case LowerInst::Shl:
        case LowerInst::Shr:
        case LowerInst::Sar: {
            static const struct { MachineFormId imm, one, cl; } shifts[] = {
                { FormShlImm, FormShlOne, FormShlCl },
                { FormShrImm, FormShrOne, FormShrCl },
                { FormSarImm, FormSarOne, FormSarCl },
            };

            auto& forms = shifts[inst->kind - LowerInst::Shl];
            if(!hasEmbeddedRhs(base, inst)) return forms.cl;
            return embeddedValue(base, ((LowerInstBinary*)inst)->rhs) == 1 ? forms.one : forms.imm;
        }

        case LowerInst::Select: {
            auto select = (LowerInstSelect*)inst;
            auto onFlags = select->getEmbeddedCmp();
            auto type = select->result.type;

            if(isFloat(type)) {
                return onFlags
                    ? byFloatWidth(type, FormSelectFloat32Flags, FormSelectFloat64Flags)
                    : byFloatWidth(type, FormSelectFloat32Reg, FormSelectFloat64Reg);
            }

            return onFlags ? FormSelectFlags : FormSelectReg;
        }

        case LowerInst::Alloca:
            return isImm(base[((LowerInstAlloca*)inst)->byteCount]) ? FormAllocaFixed : FormAllocaDynamic;

        // One form per access width and signedness: a narrow load extends into the whole destination
        // register, which is a different opcode rather than a different operand size. A 4-byte load
        // only needs one when its result is wider than it is, since a 32-bit move already clears the
        // upper half of what it writes.
        case LowerInst::Load: {
            auto load = (LowerInstLoad*)inst;
            auto isSigned = load->isSigned();

            // A float is loaded by the instruction that owns its bank, at exactly its own width:
            // nothing extends into a vector register, so there is no narrow form to choose between.
            if(isFloat(load->result.type)) {
                assertTrue(load->getWidth() == (load->result.type == LowerType::Float32 ? 4u : 8u));
                return byFloatWidth(load->result.type, FormLoadF32, FormLoadF64);
            }

            switch(load->getWidth()) {
                case 1: return isSigned ? FormLoad8S : FormLoad8;
                case 2: return isSigned ? FormLoad16S : FormLoad16;
                case 4: return isSigned && is64Bit(load->result.type) ? FormLoad32S : FormLoad32;
                default: return FormLoad64;
            }
        }

        case LowerInst::Store: {
            auto store = (LowerInstStore*)inst;
            auto type = base[store->value]->type;

            if(isFloat(type)) {
                assertTrue(store->getWidth() == (type == LowerType::Float32 ? 4u : 8u));
                return byFloatWidth(type, FormStoreF32, FormStoreF64);
            }

            switch(store->getWidth()) {
                case 1: return FormStore8;
                case 2: return FormStore16;
                case 4: return FormStore32;
                default: return FormStore64;
            }
        }

        case LowerInst::Copy:
            return ((LowerInstCopy*)inst)->isUnrolled() ? FormBlockCopyUnrolled : FormBlockCopyRep;
        case LowerInst::SetPattern:
            return ((LowerInstSetPattern*)inst)->isUnrolled() ? FormBlockSetUnrolled : FormBlockSetRep;

        case LowerInst::Call: {
            auto call = (LowerInstCall*)inst;
            if(call->getCallType() == LowerCallType::Syscall) return FormSyscall;

            // A statically known callee is a rel32 call that never reads the address out of a
            // register; anything else goes through one.
            auto callee = base[call->used()[0]];
            return callee->inst()->kind == LowerInst::Fun ? FormCallDirect : FormCallIndirect;
        }

        case LowerInst::Je:
            return ((LowerInstJe*)inst)->getEmbeddedCmp() ? FormJccFlags : FormJccReg;
        case LowerInst::Jmp: return FormJmp;
        case LowerInst::Ret: return FormRet;
    }

    assertTrue("no machine form for this instruction" == nullptr);
    return FormNop;
}

I32 opcodeAddressOperand(MachineOpcodeId opcode) {
    // The first form of the opcode answers for all of them: validateMachineForms requires them to
    // agree, which is what lets this be asked before selection has chosen between them.
    for(auto& form: machineTarget().forms) {
        if(form.opcode == opcode) return form.addressOperand();
    }

    return -1;
}

bool opcodeCanEmbedImmediate(MachineOpcodeId opcode, Size index, U64 value) {
    // Every form of the opcode, rather than the first one that names an immediate there: which form
    // an instruction ends up in is not settled while the peepholes are still running, so the
    // question is whether *any* of them could carry this value in this position.
    for(auto& form: machineTarget().forms) {
        if(form.opcode != opcode) continue;
        if(index >= form.uses.size()) continue;
        if(form.uses[index].kind != OperandConstraintKind::Immediate) continue;
        if(fitsImmediate(form.uses[index].immediate, value)) return true;
    }

    return false;
}
