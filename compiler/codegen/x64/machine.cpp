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
    FormGlobalAddress,
    FormFunctionAddress,
    FormFunctionImplicit,

    FormMove,
    FormCastMov,
    FormCastSext,
    FormCastImm,
    FormBitcast,
    FormBitcastImm,

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

    FormSelectFlags,
    FormSelectReg,

    FormAllocaFixed,
    FormAllocaDynamic,

    FormLoad8, FormLoad8S,
    FormLoad16, FormLoad16S,
    FormLoad32, FormLoad32S,
    FormLoad64,

    FormStore8, FormStore16, FormStore32, FormStore64,

    FormBlockCopyRep,
    FormBlockCopyUnrolled,
    FormBlockSetRep,
    FormBlockSetUnrolled,

    FormCallDirect,
    FormCallIndirect,
    FormSyscall,
    FormPushArgReg,
    FormPushArgImm,

    FormLea,
    FormPush,
    FormPop,

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

    // A select whose condition arrives in a register tests it first, and that test writes the flags;
    // one whose condition is already in the flags reads them and writes nothing.
    name(OpSelect, "select"_v, true);

    name(OpAlloca, "alloca"_v);
    name(OpLoad, "load"_v);
    name(OpStore, "store"_v);
    name(OpBlockCopy, "blockcopy"_v);
    name(OpBlockSet, "blockset"_v);
    name(OpCall, "call"_v);
    name(OpPushArg, "pusharg"_v);
    name(OpAddress, "address"_v);
    name(OpLea, "lea"_v);
    name(OpPush, "push"_v);
    name(OpPop, "pop"_v);
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
     * Unary arithmetic.
     *
     * NEG and NOT take their subject as r/m, so a value the allocator left in the frame is negated
     * or inverted in place rather than loaded, changed and stored back.
     */

    auto unaryArith = [&](MachineFormId id, MachineOpcodeId opcode, StringView formName, U8 extension) {
        auto& form = add(id, opcode, formName);
        form.uses.push(regOrMem(MemoryAccessKind::ReadWrite));
        form.defs.push(tiedDef(0));
        form.flagsEffect = FlagsEffect::Def;
        form.encoding = rmExt(0xf7, extension, useRef(0));
    };

    unaryArith(FormNeg, OpNeg, "neg r/m"_v, 3);
    unaryArith(FormNot, OpNot, "not r/m"_v, 2);

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
     * Stack allocation.
     *
     * A compile-time size becomes a frame object and one `lea`; a size only known at run time has to
     * round itself up and move the stack pointer. Both are declared as writing the flags, because
     * which of the two applies is settled at frame layout rather than here and the dynamic expansion
     * does write them.
     */

    {
        auto& form = add(FormAllocaFixed, OpAlloca, "lea r, [frame]"_v);
        form.uses.push(immediate(ImmediateWidth::Imm64));
        form.defs.push(def());
        form.flagsEffect = FlagsEffect::Clobber;
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
        // The unrolled form needs one general register to carry each word through. It is declared as
        // a temporary *and* held as a clobber, which is how the reservation is made today: the
        // clobber is what keeps a live value out of it.
        auto& form = add(FormBlockCopyUnrolled, OpBlockCopy, "mov (unrolled)"_v);
        form.uses.push(anyReg());
        form.uses.push(anyReg());
        form.clobbers.add(gpr(IntRegister::r11));
        form.temporaries.counts[BankGpr] = 1;
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

    // push and pop are fixed at 64-bit operand size in long mode, so a REX prefix here only ever
    // extends the register number - which is what a 32-bit width states.
    {
        auto& form = add(FormPush, OpPush, "push r"_v);
        form.uses.push(anyReg());
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::OpcodeReg,
            .opcode = 0x50,
            .rmField = useRef(0),
            .width = OperationWidth::Fixed32,
        };
    }

    {
        auto& form = add(FormPop, OpPop, "pop r"_v);
        form.defs.push(def());
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::OpcodeReg,
            .opcode = 0x58,
            .rmField = defRef(0),
            .width = OperationWidth::Fixed32,
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

            if(c.kind == OperandConstraintKind::RegisterSubset) {
                if(!(c.allowed.complement(cls.allowedPhysical) == c.allowed)) {
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
        auto reserved = registers.bank(BankGpr).reserved | registers.bank(BankVector).reserved;
        if(!(form.clobbers & reserved).isEmpty()) fail(form, "clobbers a reserved register"_v);
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

    return ok;
}

/*
 * Selection.
 */

MachineOpcodeId opcodeFor(LowerInst* inst) {
    switch(inst->kind) {
        case LowerInst::Arg:        return OpArg;
        case LowerInst::Global:     return OpGlobalAddress;
        case LowerInst::Fun:        return OpFunctionAddress;
        case LowerInst::Imm:        return OpImm;
        case LowerInst::Nop:        return OpNop;
        case LowerInst::Set:        return OpMove;
        case LowerInst::Cast:       return OpCast;
        case LowerInst::Bitcast:    return OpBitcast;
        case LowerInst::Neg:        return OpNeg;
        case LowerInst::Not:        return OpNot;
        case LowerInst::Add:        return OpAdd;
        case LowerInst::Sub:        return OpSub;
        case LowerInst::Mul:        return OpMul;
        case LowerInst::IMul:       return OpIMul;
        case LowerInst::Div:        return OpDiv;
        case LowerInst::IDiv:       return OpIDiv;
        case LowerInst::Rem:        return OpRem;
        case LowerInst::IRem:       return OpIRem;
        case LowerInst::Shl:        return OpShl;
        case LowerInst::Shr:        return OpShr;
        case LowerInst::Sar:        return OpSar;
        case LowerInst::And:        return OpAnd;
        case LowerInst::Or:         return OpOr;
        case LowerInst::Xor:        return OpXor;
        case LowerInst::Cmp:        return OpCmp;
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
        case LowerInst::X86Push:    return OpPush;
        case LowerInst::X86Pop:     return OpPop;
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

// The types this backend has forms for. Floating-point selection is the vector work of the plan's
// stage C, and the rejection belongs here rather than in the encoder: a selector that returned an
// integer form for a float operand would produce a working compile of the wrong program, and no
// later stage could tell.
static void requireIntLike(LowerType type) {
    assertTrue(isIntLike(type)); // no form for this operand type - floating point is not selected yet
}

MachineFormId selectForm(LowerBase base, LowerInst* inst) {
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

        case LowerInst::X86Push:    return FormPush;
        case LowerInst::X86Pop:     return FormPop;
        case LowerInst::Global:     return FormGlobalAddress;

        case LowerInst::X86PushArg:
            return isImm(base[((LowerInstX86PushArg*)inst)->arg]) ? FormPushArgImm : FormPushArgReg;

        case LowerInst::Imm: {
            // Decided from the value alone. Whether the immediate is embedded is a peephole's
            // answer and may still change; whether it would be materialized with `xor` or with
            // `mov` is not, which is what lets the compare folding read the flags effect early.
            auto imm = (LowerImm*)inst;
            if(isImplicit(&imm->result)) return FormImmImplicit;
            return imm->i == 0 && isIntLike(imm->result.type) ? FormImmZero : FormImmMov;
        }

        case LowerInst::Fun:
            return isImplicit(&((LowerInstFun*)inst)->result) ? FormFunctionImplicit : FormFunctionAddress;

        case LowerInst::Set: return FormMove;

        case LowerInst::Cast: {
            auto cast = (LowerInstCast*)inst;
            requireIntLike(base[cast->from]->type);
            requireIntLike(cast->result.type);
            if(isImm(base[cast->from])) return FormCastImm;

            // Only a signed value widened into a signed one has to carry its sign bit up; every
            // other cast between integer classes is the truncating-and-clearing move.
            return cast->isSignedSource() && cast->isSignedResult() ? FormCastSext : FormCastMov;
        }

        case LowerInst::Bitcast: {
            auto bitcast = (LowerInstUnary*)inst;
            requireIntLike(base[bitcast->from]->type);
            requireIntLike(bitcast->result.type);
            return isImm(base[bitcast->from]) ? FormBitcastImm : FormBitcast;
        }

        case LowerInst::Neg: return FormNeg;

        case LowerInst::Not:
            requireIntLike(base[((LowerInstUnary*)inst)->from]->type);
            return FormNot;

        // An addition or subtraction of one is a byte shorter as `inc`/`dec`, and which of the two
        // it is depends only on the constant - so it is chosen here rather than noticed by the
        // encoder. A subtraction of one decrements, and of minus one increments.
        case LowerInst::Add: {
            if(!hasEmbeddedRhs(base, inst)) return FormAddReg;

            auto value = embeddedValue(base, ((LowerInstBinary*)inst)->rhs);
            if(value == 1) return FormAddInc;
            if(value == U64(I64(-1))) return FormAddDec;
            return FormAddImm;
        }

        case LowerInst::Sub: {
            if(!hasEmbeddedRhs(base, inst)) return FormSubReg;

            auto value = embeddedValue(base, ((LowerInstBinary*)inst)->rhs);
            if(value == 1) return FormSubDec;
            if(value == U64(I64(-1))) return FormSubInc;
            return FormSubImm;
        }

        case LowerInst::And: return hasEmbeddedRhs(base, inst) ? FormAndImm : FormAndReg;
        case LowerInst::Or:  return hasEmbeddedRhs(base, inst) ? FormOrImm : FormOrReg;
        case LowerInst::Xor: return hasEmbeddedRhs(base, inst) ? FormXorImm : FormXorReg;

        // A comparison whose result the folding could not leave in the flags has to be materialized
        // into a register afterwards, which is a form of its own rather than a tail the encoder
        // decides to add.
        case LowerInst::Cmp: {
            requireIntLike(base[((LowerInstBinary*)inst)->lhs]->type);
            auto materialize = !isImplicit(&((LowerInstCmp*)inst)->result);

            if(hasEmbeddedRhs(base, inst)) return materialize ? FormCmpImmSet : FormCmpImm;
            return materialize ? FormCmpRegSet : FormCmpReg;
        }

        // The group-3 multiplies and divides read and write the rdx:rax pair, which only the integer
        // encodings have.
        case LowerInst::Mul:
        case LowerInst::Div:
        case LowerInst::IDiv:
        case LowerInst::Rem:
        case LowerInst::IRem:
        case LowerInst::IMul: {
            assertTrue(isInt(((LowerInstBinary*)inst)->result.type)); // no integer form for this type

            switch(inst->kind) {
                case LowerInst::Mul:  return FormMul;
                case LowerInst::Div:  return FormDiv;
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

        case LowerInst::Select:
            return ((LowerInstSelect*)inst)->getEmbeddedCmp() ? FormSelectFlags : FormSelectReg;

        case LowerInst::Alloca:
            return isImm(base[((LowerInstAlloca*)inst)->byteCount]) ? FormAllocaFixed : FormAllocaDynamic;

        // One form per access width and signedness: a narrow load extends into the whole destination
        // register, which is a different opcode rather than a different operand size. A 4-byte load
        // only needs one when its result is wider than it is, since a 32-bit move already clears the
        // upper half of what it writes.
        case LowerInst::Load: {
            auto load = (LowerInstLoad*)inst;
            auto isSigned = load->isSigned();

            switch(load->getWidth()) {
                case 1: return isSigned ? FormLoad8S : FormLoad8;
                case 2: return isSigned ? FormLoad16S : FormLoad16;
                case 4: return isSigned && is64Bit(load->result.type) ? FormLoad32S : FormLoad32;
                default: return FormLoad64;
            }
        }

        case LowerInst::Store: {
            switch(((LowerInstStore*)inst)->getWidth()) {
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

ImmediateWidth immediateWidthFor(MachineOpcodeId opcode, Size index) {
    auto& target = machineTarget();

    for(auto& form: target.forms) {
        if(form.opcode != opcode) continue;
        if(index >= form.uses.size()) continue;
        if(form.uses[index].kind != OperandConstraintKind::Immediate) continue;

        return form.uses[index].immediate;
    }

    return ImmediateWidth::None;
}
