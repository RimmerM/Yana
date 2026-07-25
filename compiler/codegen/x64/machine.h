#pragma once

#include "target.h"

/*
 * Selected machine instructions.
 *
 * The lower IR is semantic and target-independent. This is what AMD64 selection turns it into: for
 * every instruction, a *machine opcode* naming the target operation and a *machine form* naming the
 * one legal encoding shape it was selected into.
 *
 *   opcode   the semantic target operation - integer add, signed divide, syscall.
 *   form     one legal shape of it, with its operand constraints and its machine effects. An add
 *            with an immediate right-hand side and one with a register right-hand side are two
 *            forms of one opcode, and they differ in more than their bytes: one has an immediate
 *            operand and the other has an operand the allocator has to place.
 *
 * The point of the split is that everything downstream asks the form rather than the instruction.
 * Whether an operand is destructive, whether one of them may stay in a frame slot, which registers
 * an instruction forces its operands into, what it does to the flags, how wide an immediate it can
 * carry - each of those used to be a separate table or a separate switch, and each of them was a
 * place two answers could drift apart. They are fields of one descriptor now.
 *
 * Forms are target data. Generic placement, legalization and emission ask them questions; none of
 * them switches on an instruction name.
 *
 * During this migration a MachineInst annotates the LowerInst it was selected from rather than
 * replacing it - see MachineFunction below.
 */

using MachineOpcodeId = U16;
using MachineFormId = U16;

/*
 * Opcodes.
 */

enum : MachineOpcodeId {
    OpNone = 0,

    OpNop,
    OpArg,              // no code: an argument is already in place on entry
    OpPhi,              // no code: carried by the parallel copies at each predecessor's terminator
    OpImm,
    OpGlobalAddress,
    OpFunctionAddress,
    OpMove,             // Set
    OpCast,
    OpBitcast,
    OpNeg,
    OpNot,
    OpAdd,
    OpSub,
    OpMul,
    OpIMul,
    OpDiv,
    OpIDiv,
    OpRem,
    OpIRem,
    OpShl,
    OpShr,
    OpSar,
    OpAnd,
    OpOr,
    OpXor,
    OpCmp,
    OpSelect,
    OpAlloca,
    OpLoad,
    OpStore,
    OpBlockCopy,
    OpBlockSet,
    OpCall,
    OpPushArg,
    OpAddress,          // an addressing mode, folded into the access that reads it
    OpLea,
    OpBswap,
    OpPush,
    OpPop,
    OpJmp,
    OpJcc,
    OpRet,

    kMachineOpcodeCount,
};

/*
 * Operand constraints.
 */

enum class OperandRole: U8 {
    Use,
    Def,
    UseDef,
};

// When in the instruction the operand is read or written, which is what says whether a def may share
// a register with a use. Every AMD64 form described so far reads all of its operands before writing
// any result, so nothing is early-clobber yet; the field exists because the first form that is not
// must be able to say so rather than being handled by a special case in the allocator.
enum class OperandTiming: U8 {
    EarlyUse,
    LateUse,
    EarlyDef,
    LateDef,
};

enum class OperandConstraintKind: U8 {
    None,             // this operand occupies nothing - it was folded into the encoding
    Register,         // any register of the operand's class
    FixedRegister,    // one particular register, which the encoding forces
    RegisterSubset,   // any register of `allowed`
    RegisterOrMemory, // a register, or a frame slot read (or written) in place
    Memory,           // a memory reference, never a register
    Immediate,        // a constant carried in the instruction
    Address,          // a complete addressing mode
    ReuseOperand,     // the same location as `tiedOperand`
};

// What a form does to the operand it may take from memory. A read-only memory source removes a
// reload; a read/write one removes the store as well, which is what makes an in-place accumulator
// free once it has been spilled.
enum class MemoryAccessKind: U8 {
    None,
    Read,
    Write,
    ReadWrite,
};

// How wide an immediate a form can carry, which is what decides whether a constant can be embedded
// into it at all. Stated once here: the selector asks it to decide whether to fold a constant, and
// the encoder asks it to choose the byte width.
enum class ImmediateWidth: U8 {
    None,
    Imm8,
    Imm8OrImm32, // the shorter encoding where the value fits, otherwise the longer one
    Imm32,
    Imm64,
};

static constexpr U8 kNoTiedOperand = 0xff;

struct MachineOperandConstraint {
    OperandRole role = OperandRole::Use;
    OperandTiming timing = OperandTiming::EarlyUse;
    OperandConstraintKind kind = OperandConstraintKind::None;

    // The class the operand may occupy. Ignored for the kinds that occupy no register.
    RegisterClassId regClass = ClassGpr64;

    // For FixedRegister.
    PhysicalReg fixedReg;

    // For RegisterSubset.
    RegSet allowed;

    // For ReuseOperand, and for a def that shares its location with one of the uses: the operand
    // index within the *use* list that this one is tied to.
    U8 tiedOperand = kNoTiedOperand;

    // For RegisterOrMemory and Memory.
    MemoryAccessKind memoryAccess = MemoryAccessKind::None;

    // For Immediate.
    ImmediateWidth immediate = ImmediateWidth::None;
};

/*
 * Flags.
 */

enum class FlagsEffect: U8 {
    None,
    Def,
    Use,
    UseDef,
    Clobber,
};

inline bool writesFlags(FlagsEffect effect) {
    return effect == FlagsEffect::Def || effect == FlagsEffect::UseDef || effect == FlagsEffect::Clobber;
}

/*
 * Encoding.
 *
 * The byte layout of each family is still written by hand in gen.cpp; what the form states is which
 * family a selected instruction belongs to, so that emission is a dispatch on selected data rather
 * than a second switch over instruction kinds. Turning the regular families into byte tables is the
 * next step, and it changes this descriptor rather than anything that reads it.
 */
enum class EncodingFamily: U8 {
    None,          // emits nothing at all
    Pseudo,        // expanded by a dedicated encoder (calls, allocas, block operations, the frame)
    RegRegAlu,     // the group-1 two-operand shapes and their memory directions
    RegImmAlu,
    Group3,        // the one-operand r/m forms with an opcode extension: mul, div, neg, not
    Shift,
    Move,
    Lea,
    Compare,
    Conditional,   // setcc, cmovcc, jcc
    LoadStore,
    Stack,         // push/pop
};

// The target features a form needs. Everything described so far is baseline AMD64, so this is zero
// throughout; it exists because the SSE-through-AVX-512 forms cannot be added without it.
using FeatureSet = U32;
static constexpr FeatureSet kFeatureBaseline = 0;

// The scratch registers a form's expansion needs beyond its declared operands, by bank. `rep movsb`
// needs none; the unrolled copy needs one general register to carry each word through.
struct TemporaryDemand {
    U8 counts[kRegisterBankCount] = {};
};

/*
 * Forms.
 */

struct MachineForm {
    MachineFormId id = 0;
    MachineOpcodeId opcode = OpNone;
    StringView name;

    // Parallel to the instruction's used() and created() buffers, so operand N is entry N. Empty for
    // the pseudos whose operand count is not fixed - a call has as many operands as it has arguments
    // - which say so with `conventionOperands` and take their constraints from the convention.
    Array<MachineOperandConstraint> uses;
    Array<MachineOperandConstraint> defs;

    // The operand and result locations come from the selected calling convention rather than from
    // the arrays above: a call's, a syscall's and a return's.
    bool conventionOperands = false;

    // Registers the form writes behind its operands' backs, and the ones it reads or writes without
    // naming them as operands.
    RegSet implicitUses;
    RegSet implicitDefs;
    RegSet clobbers;

    FlagsEffect flagsEffect = FlagsEffect::None;
    FeatureSet requiredFeatures = kFeatureBaseline;
    EncodingFamily encoding = EncodingFamily::None;
    TemporaryDemand temporaries;

    // Which operand's type gives the width the operation works at, as an index into `uses`, or -1
    // for the first result. It is not always the operand's own: a comparison produces an Int32
    // whatever it compared, so its width comes from what went in.
    //
    // This is what decides whether an operand may be read out of a frame slot in place: slots are
    // packed by width, so an access of any other width would take a neighbouring value with it.
    I8 widthFromUse = -1;

    // The operand that may be read directly out of a frame slot, and the one that may be read and
    // written there in place, as indices into `uses`. At most one of the two can be taken at any one
    // instruction: both want the r/m field, and there is one.
    I32 memoryUse() const { return findMemory(MemoryAccessKind::Read); }
    I32 memoryDef() const { return findMemory(MemoryAccessKind::ReadWrite); }

    // The use this form's first result is written over, or -1 for a form that writes its result
    // somewhere of its own. This is the destructive two-address rule, stated as a tie.
    I32 tiedResult() const {
        if(defs.size() == 0 || defs[0].tiedOperand == kNoTiedOperand) return -1;
        return I32(defs[0].tiedOperand);
    }

private:
    I32 findMemory(MemoryAccessKind access) const {
        for(Size i = 0; i < uses.size(); i++) {
            if(uses[i].kind != OperandConstraintKind::RegisterOrMemory) continue;
            if(uses[i].memoryAccess == access) return I32(i);
        }

        return -1;
    }
};

// One opcode, and the forms selection may choose between for it.
struct MachineOpcodeDesc {
    MachineOpcodeId id = OpNone;
    StringView name;

    // Set when which form is chosen changes whether the instruction writes the flags. Selection for
    // such an opcode must not depend on anything the peephole passes decide, because the compare
    // folding in transform.cpp asks the flags question while those passes are still running. Every
    // other opcode's forms have to agree on their flags effect, which validateMachineForms checks.
    bool flagsSelective = false;
};

/*
 * The form table.
 */

struct MachineTarget {
    MachineTarget();

    Array<MachineForm> forms;
    MachineOpcodeDesc opcodes[kMachineOpcodeCount];

    const MachineForm& form(MachineFormId id) const { return forms[id]; }
    const MachineOpcodeDesc& opcode(MachineOpcodeId id) const { return opcodes[id]; }
};

const MachineTarget& machineTarget();

/*
 * Selected instructions.
 */

struct MachineInst {
    LowerInst* inst = nullptr;
    MachineOpcodeId opcode = OpNone;
    MachineFormId form = 0;
};

// Every instruction of one function, with the opcode and form each was selected into. Keyed by the
// lower instruction because selection annotates the lower IR during this migration rather than
// replacing it: the allocator and the encoder still walk the lower instruction lists, and ask this
// for the facts they used to derive for themselves.
struct MachineFunction {
    HashMap<LowerInst*, MachineInst> insts;

    void select(LowerInst* inst, MachineOpcodeId opcode, MachineFormId form) {
        insts.add(inst, MachineInst { inst, opcode, form });
    }

    const MachineInst& operator [] (LowerInst* inst) const {
        auto found = insts.get(inst);
        assertTrue(found.isJust()); // an instruction that selection never reached
        return found.unwrap();
    }

    const MachineForm& formOf(LowerInst* inst) const {
        return machineTarget().form((*this)[inst].form);
    }

    MachineOpcodeId opcodeOf(LowerInst* inst) const { return (*this)[inst].opcode; }
};

// Chooses the opcode and form for one instruction. Called twice: once by the compare-folding
// peephole, which has to ask what an instruction does to the flags before the pipeline has finished
// deciding what is implicit, and once by selectMachineForms after everything has settled. The two
// may reach different forms - an immediate that becomes implicit turns a register form into an
// immediate one - but never forms that disagree about the flags; see MachineOpcodeDesc.
MachineFormId selectForm(LowerBase base, LowerInst* inst);

// Whether some form of `opcode` accepts an immediate as operand `index`, and if so how wide. This is
// the one statement of which operands can swallow a constant: the peephole that embeds immediates
// and the encoder that writes their bytes both read it.
ImmediateWidth immediateWidthFor(MachineOpcodeId opcode, Size index);

// The opcode an instruction selects, independent of which form it ends up in.
MachineOpcodeId opcodeFor(LowerInst* inst);

// Checks the form table: that every operand index exists, every tie joins compatible roles, fixed
// registers are allocatable members of their operand's class, at most one operand can occupy the
// single r/m field, and the forms of one opcode agree about the flags unless the opcode says they
// do not. Run once, when the table is built, in debug builds.
bool validateMachineForms(const MachineTarget& target);
