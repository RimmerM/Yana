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
    FormCast,
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

    FormMul,
    FormDiv,
    FormIDiv,
    FormRem,
    FormIRem,
    FormIMulReg,
    FormIMulImm,

    FormShlImm, FormShlCl,
    FormShrImm, FormShrCl,
    FormSarImm, FormSarCl,

    FormCmpReg,
    FormCmpImm,

    FormSelectFlags,
    FormSelectReg,

    FormAllocaFixed,
    FormAllocaDynamic,

    FormLoad,
    FormStore,

    FormBlockCopyRep,
    FormBlockCopyUnrolled,
    FormBlockSetRep,
    FormBlockSetUnrolled,

    FormCallDirect,
    FormCallIndirect,
    FormSyscall,
    FormPushArg,

    FormLea,
    FormBswap,
    FormPush,
    FormPop,

    FormJmp,
    FormJccFlags,
    FormJccReg,
    FormRet,

    kMachineFormCount,
};

/*
 * Operand constraint shorthands.
 */

static MachineOperandConstraint anyReg(RegisterClassId cls = ClassGpr64) {
    return MachineOperandConstraint {
        .kind = OperandConstraintKind::Register,
        .regClass = cls,
    };
}

static MachineOperandConstraint fixedReg(IntRegister reg) {
    return MachineOperandConstraint {
        .kind = OperandConstraintKind::FixedRegister,
        .regClass = ClassGpr64,
        .fixedReg = gpr(reg),
    };
}

// An operand the encoding can take from a frame slot: `add rax, [slot]` in place of a reload and an
// add. `access` says whether the slot is only read, or read and written in place - the latter is the
// read-modify-write direction, which removes the store as well.
static MachineOperandConstraint regOrMem(MemoryAccessKind access) {
    return MachineOperandConstraint {
        .kind = OperandConstraintKind::RegisterOrMemory,
        .regClass = ClassGpr64,
        .memoryAccess = access,
    };
}

static MachineOperandConstraint immediate(ImmediateWidth width) {
    return MachineOperandConstraint {
        .kind = OperandConstraintKind::Immediate,
        .immediate = width,
    };
}

static MachineOperandConstraint address() {
    return MachineOperandConstraint { .kind = OperandConstraintKind::Address };
}

// A result the encoding writes over one of its operands - the destructive two-address rule, which is
// the shape most of the AMD64 ALU takes.
static MachineOperandConstraint tiedDef(U8 operand) {
    return MachineOperandConstraint {
        .role = OperandRole::Def,
        .timing = OperandTiming::LateDef,
        .kind = OperandConstraintKind::ReuseOperand,
        .regClass = ClassGpr64,
        .tiedOperand = operand,
    };
}

static MachineOperandConstraint def(RegisterClassId cls = ClassGpr64) {
    return MachineOperandConstraint {
        .role = OperandRole::Def,
        .timing = OperandTiming::LateDef,
        .kind = OperandConstraintKind::Register,
        .regClass = cls,
    };
}

// A result that occupies nothing: a comparison consumed as flags, an elided direct callee, the
// result of an argument store that stands in for the argument and is never read.
static MachineOperandConstraint noDef() {
    return MachineOperandConstraint { .role = OperandRole::Def, .kind = OperandConstraintKind::None };
}

// An operand the encoding swallowed: it occupies no location, and nothing is copied anywhere for it.
static MachineOperandConstraint folded() {
    return MachineOperandConstraint { .kind = OperandConstraintKind::None };
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
    name(OpBswap, "bswap"_v);
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

    add(FormNop, OpNop, "nop"_v).encoding = EncodingFamily::Pseudo;
    add(FormArg, OpArg, "arg"_v);
    add(FormPhi, OpPhi, "phi"_v);

    // An addressing mode produces no code and no register of its own; its base and index are
    // ordinary register operands, read by whichever access folds it in.
    add(FormAddress, OpAddress, "address"_v).defs.push(noDef());

    /*
     * Constants and addresses.
     */

    {
        auto& form = add(FormImmMov, OpImm, "mov r, imm"_v);
        form.defs.push(def());
        form.encoding = EncodingFamily::Move;
    }

    {
        // `xor r, r`, which zeroes the whole register whatever the value's declared width. Two bytes
        // where `mov r, 0` is five, at the cost of the flags - which is the whole reason this is a
        // form of its own rather than an encoding detail.
        auto& form = add(FormImmZero, OpImm, "xor r, r"_v);
        form.defs.push(def());
        form.flagsEffect = FlagsEffect::Def;
        form.encoding = EncodingFamily::RegRegAlu;
    }

    add(FormImmImplicit, OpImm, "imm (embedded)"_v).defs.push(noDef());

    {
        auto& form = add(FormGlobalAddress, OpGlobalAddress, "lea r, [rip + global]"_v);
        form.defs.push(def());
        form.encoding = EncodingFamily::Lea;
    }

    {
        auto& form = add(FormFunctionAddress, OpFunctionAddress, "lea r, [rip + fun]"_v);
        form.defs.push(def());
        form.encoding = EncodingFamily::Lea;
    }

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
        form.encoding = EncodingFamily::Move;
    }

    // Casts have no memory form: their source and result widths differ by definition, and a slot is
    // exactly as wide as the value in it, so an access at the other width would take a neighbour
    // with it.
    {
        auto& form = add(FormCast, OpCast, "mov/movsxd r, r"_v);
        form.uses.push(anyReg());
        form.defs.push(def());
        form.encoding = EncodingFamily::Move;
    }

    {
        // An immediate source makes the cast a constant materialization, already narrowed or widened
        // by the move encoding's own choice of width.
        auto& form = add(FormCastImm, OpCast, "mov r, imm"_v);
        form.uses.push(immediate(ImmediateWidth::Imm64));
        form.defs.push(def());
        form.encoding = EncodingFamily::Move;
    }

    {
        auto& form = add(FormBitcast, OpBitcast, "mov r, r"_v);
        form.uses.push(anyReg());
        form.defs.push(def());
        form.encoding = EncodingFamily::Move;
    }

    {
        auto& form = add(FormBitcastImm, OpBitcast, "mov r, imm"_v);
        form.uses.push(immediate(ImmediateWidth::Imm64));
        form.defs.push(def());
        form.encoding = EncodingFamily::Move;
    }

    /*
     * Unary arithmetic.
     *
     * NEG and NOT take their subject as r/m, so a value the allocator left in the frame is negated
     * or inverted in place rather than loaded, changed and stored back.
     */

    auto unaryArith = [&](MachineFormId id, MachineOpcodeId opcode, StringView formName) {
        auto& form = add(id, opcode, formName);
        form.uses.push(regOrMem(MemoryAccessKind::ReadWrite));
        form.defs.push(tiedDef(0));
        form.flagsEffect = FlagsEffect::Def;
        form.encoding = EncodingFamily::Group3;
    };

    unaryArith(FormNeg, OpNeg, "neg r/m"_v);
    unaryArith(FormNot, OpNot, "not r/m"_v);

    /*
     * The group-1 ALU operations.
     *
     * Two forms each. The register one can take either operand from memory - `add [slot], rcx` when
     * the result lives in that very slot, or `add rax, [slot]` when the right-hand side does - and
     * only one of the two at a time, because both want the single r/m field. The immediate one has
     * no register right-hand side to take from anywhere.
     */

    auto binaryAlu = [&](MachineFormId regId, MachineFormId immId, MachineOpcodeId opcode,
                         StringView regName, StringView immName)
    {
        auto& regForm = add(regId, opcode, regName);
        regForm.uses.push(regOrMem(MemoryAccessKind::ReadWrite));
        regForm.uses.push(regOrMem(MemoryAccessKind::Read));
        regForm.defs.push(tiedDef(0));
        regForm.flagsEffect = FlagsEffect::Def;
        regForm.encoding = EncodingFamily::RegRegAlu;

        auto& immForm = add(immId, opcode, immName);
        immForm.uses.push(regOrMem(MemoryAccessKind::ReadWrite));
        immForm.uses.push(immediate(ImmediateWidth::Imm8OrImm32));
        immForm.defs.push(tiedDef(0));
        immForm.flagsEffect = FlagsEffect::Def;
        immForm.encoding = EncodingFamily::RegImmAlu;
    };

    binaryAlu(FormAddReg, FormAddImm, OpAdd, "add r/m, r"_v, "add r/m, imm"_v);
    binaryAlu(FormSubReg, FormSubImm, OpSub, "sub r/m, r"_v, "sub r/m, imm"_v);
    binaryAlu(FormAndReg, FormAndImm, OpAnd, "and r/m, r"_v, "and r/m, imm"_v);
    binaryAlu(FormOrReg, FormOrImm, OpOr, "or r/m, r"_v, "or r/m, imm"_v);
    binaryAlu(FormXorReg, FormXorImm, OpXor, "xor r/m, r"_v, "xor r/m, imm"_v);

    /*
     * Multiply and divide.
     *
     * The group-3 forms read their first operand out of rax and write their result back into it (or,
     * for a remainder, into rdx), and take the second as r/m - so a divisor can come straight out of
     * the frame with no reload. rdx is written either way, which is why it is a clobber even where it
     * is also the result: a value living there has to be out of the way regardless.
     */

    auto group3 = [&](MachineFormId id, MachineOpcodeId opcode, StringView formName,
                      IntRegister result, bool clobberRax)
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
        form.encoding = EncodingFamily::Group3;
    };

    group3(FormMul, OpMul, "mul r/m"_v, IntRegister::rax, false);
    group3(FormDiv, OpDiv, "div r/m"_v, IntRegister::rax, false);
    group3(FormIDiv, OpIDiv, "idiv r/m"_v, IntRegister::rax, false);
    group3(FormRem, OpRem, "div r/m (remainder)"_v, IntRegister::rdx, true);
    group3(FormIRem, OpIRem, "idiv r/m (remainder)"_v, IntRegister::rdx, true);

    {
        // IMUL r, r/m is the two-operand form: the destination doubles as a source, so it is
        // destructive like the group-1 operations.
        auto& form = add(FormIMulReg, OpIMul, "imul r, r/m"_v);
        form.uses.push(anyReg());
        form.uses.push(regOrMem(MemoryAccessKind::Read));
        form.defs.push(tiedDef(0));
        form.flagsEffect = FlagsEffect::Def;
        form.encoding = EncodingFamily::RegRegAlu;
    }

    {
        // IMUL r, r/m, imm is a true three-operand form - the destination can differ from the source
        // - which is why the immediate case is not destructive where the register case is.
        auto& form = add(FormIMulImm, OpIMul, "imul r, r, imm"_v);
        form.uses.push(anyReg());
        form.uses.push(immediate(ImmediateWidth::Imm8OrImm32));
        form.defs.push(def());
        form.flagsEffect = FlagsEffect::Def;
        form.encoding = EncodingFamily::RegImmAlu;
    }

    /*
     * Shifts.
     *
     * Every shift form takes its subject as r/m, so a destination in the frame is shifted in place.
     * The count is either an immediate in the instruction or in cl, and is never the memory operand.
     */

    auto shift = [&](MachineFormId immId, MachineFormId clId, MachineOpcodeId opcode,
                     StringView immName, StringView clName)
    {
        auto& immForm = add(immId, opcode, immName);
        immForm.uses.push(regOrMem(MemoryAccessKind::ReadWrite));
        immForm.uses.push(immediate(ImmediateWidth::Imm8));
        immForm.defs.push(tiedDef(0));
        immForm.flagsEffect = FlagsEffect::Def;
        immForm.encoding = EncodingFamily::Shift;

        auto& clForm = add(clId, opcode, clName);
        clForm.uses.push(regOrMem(MemoryAccessKind::ReadWrite));
        clForm.uses.push(fixedReg(IntRegister::rcx));
        clForm.defs.push(tiedDef(0));
        clForm.flagsEffect = FlagsEffect::Def;
        clForm.encoding = EncodingFamily::Shift;
    };

    shift(FormShlImm, FormShlCl, OpShl, "shl r/m, imm"_v, "shl r/m, cl"_v);
    shift(FormShrImm, FormShrCl, OpShr, "shr r/m, imm"_v, "shr r/m, cl"_v);
    shift(FormSarImm, FormSarCl, OpSar, "sar r/m, imm"_v, "sar r/m, cl"_v);

    /*
     * Comparison.
     *
     * A comparison works at the width of the values compared, not at the width of what it produces:
     * its result is an Int32 whatever went into it, so `widthFromUse` points at the left-hand side.
     * The result is written to a register only when the flags could not be carried to its use
     * directly, which the compare folding decides.
     */

    {
        auto& form = add(FormCmpReg, OpCmp, "cmp r, r/m"_v);
        form.uses.push(anyReg());
        form.uses.push(regOrMem(MemoryAccessKind::Read));
        form.defs.push(def(ClassGpr32));
        form.flagsEffect = FlagsEffect::Def;
        form.encoding = EncodingFamily::Compare;
        form.widthFromUse = 0;
    }

    {
        auto& form = add(FormCmpImm, OpCmp, "cmp r, imm"_v);
        form.uses.push(anyReg());
        form.uses.push(immediate(ImmediateWidth::Imm8OrImm32));
        form.defs.push(def(ClassGpr32));
        form.flagsEffect = FlagsEffect::Def;
        form.encoding = EncodingFamily::Compare;
        form.widthFromUse = 0;
    }

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
        form.encoding = EncodingFamily::Conditional;
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
        form.encoding = EncodingFamily::Conditional;
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
        form.encoding = EncodingFamily::Pseudo;
    }

    {
        auto& form = add(FormAllocaDynamic, OpAlloca, "sub rsp, r"_v);
        form.uses.push(anyReg());
        form.defs.push(def());
        form.flagsEffect = FlagsEffect::Clobber;
        form.encoding = EncodingFamily::Pseudo;
    }

    /*
     * Memory access.
     */

    {
        auto& form = add(FormLoad, OpLoad, "mov r, [address]"_v);
        form.uses.push(address());
        form.defs.push(def());
        form.encoding = EncodingFamily::LoadStore;
    }

    {
        auto& form = add(FormStore, OpStore, "mov [address], r"_v);
        form.uses.push(address());
        form.uses.push(anyReg());
        form.encoding = EncodingFamily::LoadStore;
    }

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
        form.encoding = EncodingFamily::Pseudo;
    }

    {
        // The unrolled form needs one general register to carry each word through. It is declared as
        // a temporary *and* held as a clobber, which is how the reservation is made today: the
        // clobber is what keeps a live value out of it.
        auto& form = add(FormBlockCopyUnrolled, OpBlockCopy, "mov (unrolled)"_v);
        form.clobbers.add(gpr(IntRegister::r11));
        form.temporaries.counts[BankGpr] = 1;
        form.encoding = EncodingFamily::Pseudo;
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
        form.encoding = EncodingFamily::Pseudo;
    }

    add(FormBlockSetUnrolled, OpBlockSet, "mov (unrolled)"_v).encoding = EncodingFamily::Pseudo;

    /*
     * Calls.
     *
     * Operand and result locations come from the selected calling convention rather than from a
     * table here: where an argument goes depends on how many of each bank came before it, which a
     * flat list cannot say. The clobber set comes from the same place.
     */

    auto call = [&](MachineFormId id, StringView formName) {
        auto& form = add(id, OpCall, formName);
        form.conventionOperands = true;
        form.flagsEffect = FlagsEffect::Clobber;
        form.encoding = EncodingFamily::Pseudo;
    };

    call(FormCallDirect, "call rel32"_v);
    call(FormCallIndirect, "call r/m"_v);
    call(FormSyscall, "syscall"_v);

    {
        auto& form = add(FormPushArg, OpPushArg, "mov [rsp + n], r"_v);
        form.uses.push(anyReg());
        form.defs.push(noDef()); // stands in for the argument in the call's operand list
        form.encoding = EncodingFamily::LoadStore;
    }

    /*
     * The remaining target operations.
     */

    {
        auto& form = add(FormLea, OpLea, "lea r, [address]"_v);
        form.defs.push(def());
        form.encoding = EncodingFamily::Lea;
    }

    {
        auto& form = add(FormBswap, OpBswap, "bswap r"_v);
        form.uses.push(anyReg());
        form.defs.push(tiedDef(0));
        form.encoding = EncodingFamily::Pseudo;
    }

    add(FormPush, OpPush, "push r"_v).encoding = EncodingFamily::Stack;

    {
        auto& form = add(FormPop, OpPop, "pop r"_v);
        form.defs.push(def());
        form.encoding = EncodingFamily::Stack;
    }

    /*
     * Terminators.
     */

    add(FormJmp, OpJmp, "jmp rel32"_v).encoding = EncodingFamily::Pseudo;

    {
        auto& form = add(FormJccFlags, OpJcc, "jcc rel32"_v);
        form.uses.push(folded()); // the condition was consumed by the comparison that set the flags
        form.flagsEffect = FlagsEffect::Use;
        form.encoding = EncodingFamily::Conditional;
    }

    {
        auto& form = add(FormJccReg, OpJcc, "test r, r; jcc rel32"_v);
        form.uses.push(anyReg(ClassGpr32));
        form.flagsEffect = FlagsEffect::UseDef;
        form.encoding = EncodingFamily::Conditional;
    }

    {
        // A return's operands are the function's results, placed by the result half of its own
        // convention. Nothing is live once the function has returned, so it clobbers nothing.
        auto& form = add(FormRet, OpRet, "ret"_v);
        form.conventionOperands = true;
        form.encoding = EncodingFamily::Pseudo;
    }

    assertTrue(forms.size() == kMachineFormCount);
    assertTrue(validateMachineForms(*this));
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

        if(form.widthFromUse >= 0 && Size(form.widthFromUse) >= form.uses.size()) {
            fail(form, "takes its width from an operand that does not exist"_v);
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
        case LowerInst::X86Bswap:   return OpBswap;
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

MachineFormId selectForm(LowerBase base, LowerInst* inst) {
    switch(inst->kind) {
        case LowerInst::Nop:        return FormNop;
        case LowerInst::Arg:        return FormArg;
        case LowerInst::Phi:        return FormPhi;
        case LowerInst::X86Address: return FormAddress;
        case LowerInst::X86Lea:     return FormLea;
        case LowerInst::X86Bswap:   return FormBswap;
        case LowerInst::X86Push:    return FormPush;
        case LowerInst::X86Pop:     return FormPop;
        case LowerInst::X86PushArg: return FormPushArg;
        case LowerInst::Global:     return FormGlobalAddress;

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

        case LowerInst::Set:     return FormMove;
        case LowerInst::Cast:    return isImm(base[((LowerInstUnary*)inst)->from]) ? FormCastImm : FormCast;
        case LowerInst::Bitcast: return isImm(base[((LowerInstUnary*)inst)->from]) ? FormBitcastImm : FormBitcast;
        case LowerInst::Neg:     return FormNeg;
        case LowerInst::Not:     return FormNot;

        case LowerInst::Add: return hasEmbeddedRhs(base, inst) ? FormAddImm : FormAddReg;
        case LowerInst::Sub: return hasEmbeddedRhs(base, inst) ? FormSubImm : FormSubReg;
        case LowerInst::And: return hasEmbeddedRhs(base, inst) ? FormAndImm : FormAndReg;
        case LowerInst::Or:  return hasEmbeddedRhs(base, inst) ? FormOrImm : FormOrReg;
        case LowerInst::Xor: return hasEmbeddedRhs(base, inst) ? FormXorImm : FormXorReg;
        case LowerInst::Cmp: return hasEmbeddedRhs(base, inst) ? FormCmpImm : FormCmpReg;

        case LowerInst::Mul:  return FormMul;
        case LowerInst::Div:  return FormDiv;
        case LowerInst::IDiv: return FormIDiv;
        case LowerInst::Rem:  return FormRem;
        case LowerInst::IRem: return FormIRem;
        case LowerInst::IMul: return hasEmbeddedRhs(base, inst) ? FormIMulImm : FormIMulReg;

        case LowerInst::Shl: return hasEmbeddedRhs(base, inst) ? FormShlImm : FormShlCl;
        case LowerInst::Shr: return hasEmbeddedRhs(base, inst) ? FormShrImm : FormShrCl;
        case LowerInst::Sar: return hasEmbeddedRhs(base, inst) ? FormSarImm : FormSarCl;

        case LowerInst::Select:
            return ((LowerInstSelect*)inst)->getEmbeddedCmp() ? FormSelectFlags : FormSelectReg;

        case LowerInst::Alloca:
            return isImm(base[((LowerInstAlloca*)inst)->byteCount]) ? FormAllocaFixed : FormAllocaDynamic;

        case LowerInst::Load:  return FormLoad;
        case LowerInst::Store: return FormStore;

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
