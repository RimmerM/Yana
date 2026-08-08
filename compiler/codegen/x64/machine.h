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
 * them switches on an instruction name. Adding a regular instruction is a row of this table: a form
 * with its operand constraints and its encoding, and a line in selectForm saying when it applies.
 *
 * A MachineInst annotates the LowerInst it was selected from rather than replacing it, so the lower
 * IR remains the one instruction list and the selection is a second, target-owned view of it - see
 * MachineFunction below.
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
    OpMulHi,
    OpIMulHi,
    OpShl,
    OpShr,
    OpSar,
    OpAnd,
    OpOr,
    OpXor,
    OpCmp,

    // Floating point. Opcodes of their own rather than forms of the integer ones, because a float
    // add and an integer add are two target operations rather than two encodings of one: they read
    // different register files, take different operands (no SSE form carries an immediate), and
    // differ in what they do to the flags. Keeping them apart is what lets every opcode's forms
    // agree about the flags without any of them having to declare itself selective.
    OpFAdd,
    OpFSub,
    OpFMul,
    OpFDiv,
    OpFNeg,
    OpFCmp,

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
    OpJmp,
    OpJcc,
    OpRet,

    // The end of a block control never leaves - see FormNoReturn, which encodes to zero bytes.
    OpNoReturn,

    kDescribedOpcodeCount,
};

// The intrinsics take the opcodes above the described ones, one each, in the order the IR names them
// - see LowerIntrinsic. An intrinsic is a machine operation like any other; what is different about
// it is only that its description comes from the registry at the bottom of this file rather than
// from the table in machine.cpp, so that adding one is a row of data.
static constexpr Size kMachineOpcodeCount = kDescribedOpcodeCount + kLowerIntrinsicCount;

inline MachineOpcodeId opcodeForIntrinsic(LowerIntrinsic id) {
    return MachineOpcodeId(kDescribedOpcodeCount + Size(id));
}

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

// Whether a constant survives being written out as one byte, or as four, and read back
// sign-extended - which is what decides whether an encoding can carry it at all.
inline bool fitsImm8(U64 imm) {
    return (imm & 0xffffffffffffff80) == 0xffffffffffffff80 || (imm & 0x7f) == imm;
}

inline bool fitsImm32(U64 imm) {
    return (imm & 0xffffffff80000000) == 0xffffffff80000000 || (imm & 0x7fffffff) == imm;
}

// Whether `value` is a constant an operand of this width can actually carry. This is the one
// statement of immediate legality, and every stage asks it rather than the generic byte tests
// above: the peephole that decides to embed a constant, the query that says which forms could take
// it there, the selection verifier, and the check that runs before each instruction is emitted.
//
// Asking the generic test instead is how a value that fits in four bytes ends up embedded into a
// form that carries one - which is legal-looking everywhere until the encoder writes the bytes, by
// which point the operand has been removed from allocation and there is no register left to put it
// in.
inline bool fitsImmediate(ImmediateWidth width, U64 value) {
    switch(width) {
        case ImmediateWidth::None:        return false;
        case ImmediateWidth::Imm8:        return fitsImm8(value);
        case ImmediateWidth::Imm8OrImm32: return fitsImm32(value);
        case ImmediateWidth::Imm32:       return fitsImm32(value);
        case ImmediateWidth::Imm64:       return true;
    }

    return false;
}

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
 * Operand constraint shorthands.
 *
 * The vocabulary the form table is written in, shared with the intrinsic registry so that an
 * intrinsic states its operands the same way every other instruction does.
 */

inline MachineOperandConstraint anyReg(RegisterClassId cls = ClassGpr64) {
    return MachineOperandConstraint {
        .kind = OperandConstraintKind::Register,
        .regClass = cls,
    };
}

inline MachineOperandConstraint fixedReg(IntRegister reg) {
    return MachineOperandConstraint {
        .kind = OperandConstraintKind::FixedRegister,
        .regClass = ClassGpr64,
        .fixedReg = gpr(reg),
    };
}

// An operand the encoding can take from a frame slot: `add rax, [slot]` in place of a reload and an
// add. `access` says whether the slot is only read, or read and written in place - the latter is the
// read-modify-write direction, which removes the store as well.
inline MachineOperandConstraint regOrMem(MemoryAccessKind access, RegisterClassId cls = ClassGpr64) {
    return MachineOperandConstraint {
        .kind = OperandConstraintKind::RegisterOrMemory,
        .regClass = cls,
        .memoryAccess = access,
    };
}

inline MachineOperandConstraint immediate(ImmediateWidth width) {
    return MachineOperandConstraint {
        .kind = OperandConstraintKind::Immediate,
        .immediate = width,
    };
}

inline MachineOperandConstraint address() {
    return MachineOperandConstraint { .kind = OperandConstraintKind::Address };
}

// A result the encoding writes over one of its operands - the destructive two-address rule, which is
// the shape most of the AMD64 ALU takes.
inline MachineOperandConstraint tiedDef(U8 operand, RegisterClassId cls = ClassGpr64) {
    return MachineOperandConstraint {
        .role = OperandRole::Def,
        .timing = OperandTiming::LateDef,
        .kind = OperandConstraintKind::ReuseOperand,
        .regClass = cls,
        .tiedOperand = operand,
    };
}

inline MachineOperandConstraint def(RegisterClassId cls = ClassGpr64) {
    return MachineOperandConstraint {
        .role = OperandRole::Def,
        .timing = OperandTiming::LateDef,
        .kind = OperandConstraintKind::Register,
        .regClass = cls,
    };
}

// A result produced in one particular register, which is the shape most of the privileged and
// system intrinsics take: the machine writes what it writes, and the allocator copies it out.
inline MachineOperandConstraint fixedDef(IntRegister reg) {
    return MachineOperandConstraint {
        .role = OperandRole::Def,
        .timing = OperandTiming::LateDef,
        .kind = OperandConstraintKind::FixedRegister,
        .regClass = ClassGpr64,
        .fixedReg = gpr(reg),
    };
}

// A result that occupies nothing: a comparison consumed as flags, an elided direct callee, the
// result of an argument store that stands in for the argument and is never read.
inline MachineOperandConstraint noDef() {
    return MachineOperandConstraint {
        .role = OperandRole::Def,
        .timing = OperandTiming::LateDef,
        .kind = OperandConstraintKind::None,
    };
}

// An operand the encoding swallowed: it occupies no location, and nothing is copied anywhere for it.
inline MachineOperandConstraint folded() {
    return MachineOperandConstraint { .kind = OperandConstraintKind::None };
}

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
 * A form states the bytes it is emitted as, not merely which encoder to call. Emission is a walk of
 * this descriptor: the family says which shape the instruction has, the opcode fields say which
 * bytes, and the operand references say which resolved operand goes in the ModRM.reg field, which in
 * the r/m field, and which carries the immediate. The encoder chooses nothing.
 *
 * Only the forms whose byte layout is genuinely irregular - the ones that expand into several
 * instructions, or whose shape depends on the frame layout or the block order - keep a dedicated
 * encoder, and they name it here as well.
 */

// Which of an instruction's resolved operands an encoding field takes its register or value from.
// An index into the instruction's used() or created() buffer, which the legalizer's InstRegs is
// parallel to - so the encoder never has to work out which operand it is looking at.
struct OperandRef {
    I8 index = -1;       // -1 for a field this encoding does not have
    bool result = false; // an index into created()/creates rather than used()/uses

    bool isNone() const { return index < 0; }
    bool operator == (OperandRef other) const { return index == other.index && result == other.result; }
    bool operator != (OperandRef other) const { return !(*this == other); }
};

inline OperandRef useRef(U8 index) { return OperandRef { I8(index), false }; }
inline OperandRef defRef(U8 index) { return OperandRef { I8(index), true }; }

enum class EncodingFamily: U8 {
    None,        // emits nothing at all
    Opcode,      // opcode bytes and nothing else - the operands are all implicit registers
    RegRm,       // one register in ModRM.reg and one register or memory operand in r/m
    RmExt,       // one r/m operand, with an opcode extension in ModRM.reg
    RmExtImm,    // the same, plus an immediate
    RegRmImm,    // reg, r/m and an immediate - the three-operand `imul`
    MoveImm,     // a constant materialized into a register, at the shortest width that reproduces it
    Lea,         // an address materialized into a register
    LoadStore,   // a memory access, with the other operand in ModRM.reg
    Conditional, // an operation whose opcode carries a condition code: cmovcc
    OpcodeReg,   // the register is part of the opcode byte: bswap
    Pseudo,      // expanded by the dedicated encoder named below
};

// A step some r/m forms need before the operation itself, because the encoding reads a register the
// instruction does not name as an operand: an unsigned divide needs rdx cleared, a signed one needs
// rax sign-extended into it.
enum class EncodingPrelude: U8 {
    None,
    ZeroRdx,        // xor edx, edx
    SignExtendRax,  // cqo / cdq

    // `test r, r` on the instruction's last operand, which is how a condition that arrived in a
    // register rather than in the flags is turned into flags the conditional encoding can read.
    TestLastUse,
};

// The forms that keep an encoder of their own. Every one of them either expands into several
// instructions, or reads something only the frame layout or the block order knows.
enum class PseudoKind: U8 {
    None,
    Nop,
    CallDirect,
    CallIndirect,
    Syscall,
    Return,

    // Emits nothing. The block it ends has no successors and is never fallen out of, so there is
    // neither an epilogue to run nor a `ret` to run it before.
    NoReturn,

    Jump,
    Branch,
    AllocaFixed,
    AllocaDynamic,
    BlockCopyRep,
    BlockCopyUnrolled,
    BlockSetRep,
    BlockSetUnrolled,

    // The three floating-point operations AMD64 has no single scalar instruction for. Each expands
    // into two or three, and each is here rather than in the form table's regular families because
    // of that alone - what they read is still only their own resolved operands.
    FloatImm,    // a constant materialized in a general register and moved across
    FloatNeg,    // the sign bit toggled in a general register, for want of a vector sign mask
    FloatSelect, // a conditional register copy, for want of a vector cmov

    // The two intrinsics whose one operation is several instructions (§15.2 of the plan). Both
    // expand after allocation, which is only safe because every register and flag their expansion
    // touches is a fixed operand or a declared clobber of the form that names them - so nothing the
    // expansion does can surprise a placement that has already happened.
    RdTsc,   // the counter's two halves joined into the one value the intrinsic returns
    PortIn8, // a byte read from a port, zero-extended into the whole result
};

// The width an encoding operates at, which is not always the width of the value it produces: a
// comparison yields an Int32 whatever it compared, and an unsigned cast moves at the narrower of its
// two types because a 32-bit move clears the upper half of its destination either way.
//
// This is also what decides whether an operand may be read out of a frame slot in place: slots are
// packed by width, so an access of any other width would take a neighbouring value with it.
enum class OperationWidth: U8 {
    FromResult,  // the first result's type
    FromUse0,    // the first operand's type
    Narrowest,   // the narrower of the first operand and the result
    Fixed32,
    Fixed64,
};

struct EncodingDescriptor {
    EncodingFamily family = EncodingFamily::None;
    PseudoKind pseudo = PseudoKind::None;

    // The opcode bytes. `opcode` is the primary one; `opcodeAlt` is the alternative the same family
    // defines - for RegRm the direction with the destination in ModRM.reg, for the immediate
    // families the imm32 form of an opcode whose primary carries an imm8, and for a bare Opcode the
    // second byte of a three-byte encoding. Zero means the family has only the primary.
    U8 opcode = 0;
    U8 opcodeAlt = 0;

    U8 escape = 0;    // 0x0f, for the two-byte opcodes
    U8 prefix = 0;    // a mandatory prefix byte, which has to be written before REX
    U8 extension = 0; // the ModRM.reg opcode extension, for the r/m forms that have no second register

    // A shorter encoding of the same operation when the immediate is zero and the r/m operand is a
    // register, with both ModRM fields naming that register: `test r, r` for a comparison against
    // zero. Zero for a form that has no such equivalent. This is the one alternative chosen at
    // emission rather than at selection, because whether the operand is a register is placement's
    // answer and not the selector's.
    U8 zeroRegOpcode = 0;

    OperandRef regField;  // the operand in ModRM.reg
    OperandRef rmField;   // the operand in ModRM.r/m, which may be a register or a frame slot
    OperandRef immField;  // the operand carrying the immediate

    OperationWidth width = OperationWidth::FromResult;
    EncodingPrelude prelude = EncodingPrelude::None;

    // The encoding states its own width in the mandatory prefix rather than in REX.W, which is what
    // every SSE scalar form does: F3 is one float, F2 is one double, and no prefix at all is a
    // packed register. REX.W there either means nothing or means something else entirely - on
    // `cvtsi2sd` it is the *integer* operand's width - so the bit is not derived from the width.
    //
    // `width` above still answers how wide the data is, because the other thing it decides is
    // unaffected: whether a frame slot is exactly the size of the access that would read it in
    // place, which is a property of the value rather than of the prefix that names it.
    bool widthInPrefix = false;

    // How many bytes of the immediate the LoadStore family writes after the address. The other
    // families take the width from the value - one byte or four, according to which opcode was
    // chosen - but a store's immediate is as wide as the *access* rather than as wide as the number,
    // so `movb $0, [rax]` carries one byte and `movq $0, [rax]` carries four regardless of what is
    // being stored. Zero for the forms that carry no immediate at all.
    U8 immediateBytes = 0;

    bool byteRegField = false; // an 8-bit ModRM.reg operand, which needs REX to name spl/bpl/sil/dil
    bool omitWhenSame = false; // emits nothing when source and destination are already the same register

    // The encoding carries the negation of the selected condition, because the operand the tie
    // already placed in the destination is the one the *un*negated condition would have moved.
    bool negateCondition = false;

    // Materializes the flags into the instruction's first result afterwards - `setcc` and a
    // zero-extension - for a comparison whose result could not be left in the flags.
    bool materializeFlags = false;
};

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
    EncodingDescriptor encoding;
    TemporaryDemand temporaries;

    // The same operation reading its memory-capable operand from an *address* rather than from a
    // register or a frame slot: `add rax, [rdi + rcx*8]` where `add r/m, r` reads a second register.
    // Zero for a form that has no such twin.
    //
    // Which of the two an instruction takes is decided by the value in that operand and by nothing
    // else: an X86Address there is one a load fold put there (foldLoads in transform.cpp), and an
    // X86Address is the one value that can only ever be an address. `memorySourceOf` is the twin's
    // own back-pointer, and is what marks it as the one exception to the rule that every form of an
    // opcode names the same operand as its address (validateMachineForms).
    MachineFormId memorySource = 0;
    MachineFormId memorySourceOf = 0;

    // The operand that may be read directly out of a frame slot, and the one that may be read and
    // written there in place, as indices into `uses`. At most one of the two can be taken at any one
    // instruction: both want the r/m field, and there is one.
    I32 memoryUse() const { return findMemory(MemoryAccessKind::Read); }
    I32 memoryDef() const { return findMemory(MemoryAccessKind::ReadWrite); }

    // The operand this form reads as a memory *address* - the pointer a load dereferences, the line
    // a cache-control intrinsic names - as an index into `uses`, or -1 for a form that references
    // no memory of its own. This is what makes "which operand is an address" a property of the form
    // rather than of the instruction kind: the address folding, the legalizer's address resolution
    // and the selection verifier all ask here, so an instruction that references memory does not
    // have to be one of a handful of kinds each of them knows by name.
    I32 addressOperand() const {
        for(Size i = 0; i < uses.size(); i++) {
            if(uses[i].kind == OperandConstraintKind::Address) return I32(i);
        }

        return -1;
    }

    // How wide a constant this form's encoding can carry, or None for one that carries none. Taken
    // from the operand the encoding's immediate field names, so the width the encoder writes and the
    // width the allocator was promised are one statement - except for a constant materialization,
    // whose immediate is the value it *defines* rather than an operand, and which reproduces any
    // 64-bit value by construction.
    ImmediateWidth immediateWidth() const {
        auto& field = encoding.immField;
        if(field.isNone()) return ImmediateWidth::None;
        if(field.result) return ImmediateWidth::Imm64;
        return uses[field.index].immediate;
    }

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

    // Set when which form is chosen changes whether the instruction writes the flags. Every other
    // opcode's forms have to agree on it, which validateMachineForms checks - because the compare
    // folding in transform.cpp asks what an instruction does to the flags while the peephole passes
    // are still deciding which form it will take, and that question only has one answer if the forms
    // all give the same one.
    //
    // An opcode that sets this owes something instead, and there are two ways to pay it. The folding
    // walks forward from a comparison to its use asking each instruction in between whether it
    // writes the flags, where "yes" is the answer that blocks the fold - so the answer it gets has to
    // be one that will still be true when the bytes are emitted.
    //
    // **Either the answer is conservative while it can still change** - a peephole may then only move
    // the selection towards a form that writes *fewer* flags. Three obey it this way: an immediate
    // not yet embedded selects `xor r, r` (writes) and becomes implicit (writes nothing); a branch or
    // a select not yet given its comparison tests a register (writes) and becomes a read of the
    // flags; an alloca whose count is not yet embedded is the dynamic form (writes) and becomes the
    // `lea`.
    //
    // **Or the answer is already final when it is asked**, which is what the two sweeps of
    // selectMachineInstructions buy: every form decision a peephole makes is settled by the first
    // sweep, and the folding is the whole of the second. `cast` and `bitcast` pay this way and could
    // not pay the other - a constant source makes them the materializing `mov r, imm`, and zero makes
    // that `xor r, r` only once the constant is embedded, which is a *gain* of a flags effect.
    //
    // The second guarantee is a property of the pipeline rather than of those rows, so a pass
    // inserted between the two sweeps breaks it for everything that leans on it. See §3.5.2 of the
    // README, which states both and says which rows rely on which.
    bool flagsSelective = false;
};

/*
 * Intrinsics.
 *
 * An intrinsic is an operation the program asked for by name rather than one the lowering derived
 * (see LowerIntrinsic in lower_inst.h). Everything the backend needs in order to allocate and emit
 * one is a row of the registry below: a machine form like any other instruction's, the features the
 * encoding needs, what each operand and result has to be, and what the operation does beyond its
 * operands.
 *
 * The point of it being a registry is what does *not* appear elsewhere. Adding a single-instruction
 * intrinsic is a row here and a name in the IR's table - no case in placement, none in legalization,
 * none in the encoder, no new memory-operand query and no new constraint table. An intrinsic that
 * forces its operands into particular registers, one that can read an operand out of the frame, and
 * one that writes a register it does not name are each just different fields of the same row.
 */

// What an intrinsic requires of one operand or result beyond the register the form puts it in.
enum class IntrinsicOperandClass: U8 {
    Integer,    // any integer or pointer value
    Integer32,  // exactly a 32-bit integer, which is what the fixed-width machine registers take
    Integer64,
    Pointer,    // an address, which is the operand the encoding puts in a ModRM memory field
    Immediate,  // a constant, within the range the encoding accepts
};

struct IntrinsicOperandRule {
    IntrinsicOperandClass kind = IntrinsicOperandClass::Integer;
    U64 minImmediate = 0;
    U64 maxImmediate = 0;
};

// What an intrinsic does that its operands do not say. None of these are consulted by the allocator
// - every register effect is in the form - but the scheduling and folding a mid-level pass will do
// needs them stated, and stating them is free at the point the intrinsic is added rather than
// archaeology afterwards.
struct IntrinsicEffects {
    bool readsMemory = false;
    bool writesMemory = false;
    bool ordered = false;     // must not be moved across another ordered operation
    bool privileged = false;  // only valid in kernel mode
};

struct IntrinsicDescriptor {
    LowerIntrinsic id = LowerIntrinsic::Bswap;
    FeatureSet requiredFeatures = kFeatureBaseline;

    MachineOpcodeId opcode = OpNone;
    MachineFormId form = 0;

    // Parallel to the intrinsic's operands and results, in the order the IR names them.
    Array<IntrinsicOperandRule> operands;
    Array<IntrinsicOperandRule> results;

    IntrinsicEffects effects;
    bool defined = false;
};

/*
 * The form table.
 */

struct MachineTarget {
    MachineTarget();

    Array<MachineForm> forms;
    MachineOpcodeDesc opcodes[kMachineOpcodeCount];

    // One per intrinsic the IR can name, in that order. An intrinsic with no descriptor is one this
    // target cannot select, which selection rejects rather than emitting something for.
    IntrinsicDescriptor intrinsics[kLowerIntrinsicCount];

    const MachineForm& form(MachineFormId id) const { return forms[id]; }
    const MachineOpcodeDesc& opcode(MachineOpcodeId id) const { return opcodes[id]; }

    const IntrinsicDescriptor& intrinsic(LowerIntrinsic id) const {
        assertTrue(Size(id) < kLowerIntrinsicCount);
        return intrinsics[Size(id)];
    }
};

const MachineTarget& machineTarget();

// Fills in the intrinsic rows and their forms - see intrinsic.cpp. Called by MachineTarget's
// constructor once the described forms are in place, so that an intrinsic's form is an entry in the
// same table and is asked the same questions.
void addIntrinsics(MachineTarget& target);

// Checks the intrinsic registry: that every intrinsic the IR names has a descriptor, that its form
// belongs to its opcode, and that it has a rule for every operand and result the IR gives it.
bool validateIntrinsics(const MachineTarget& target);

// Whether the values one intrinsic was given match the rules its descriptor states. Asked where the
// form is selected, which is the last point at which a wrong operand is a compile error rather than
// a wrong instruction.
bool checkIntrinsicOperands(LowerBase base, const IntrinsicDescriptor& desc, LowerInstIntrinsic* inst);

/*
 * Selected instructions.
 */

struct MachineInst {
    LowerInst* inst = nullptr;
    MachineOpcodeId opcode = OpNone;
    MachineFormId form = 0;

    // The condition a conditional form was selected with, for the encodings whose opcode carries
    // one. Selected data like the form itself, so that emission reads it rather than reaching back
    // into the instruction for it.
    Maybe<LowerCmp> condition;
};

// Every instruction of one function, with the opcode and form each was selected into. Keyed by the
// lower instruction because selection annotates the lower IR rather than replacing it: the allocator
// and the encoder walk the lower instruction lists, and ask this for every target fact about what
// they find there.
struct MachineFunction {
    HashMap<LowerInst*, MachineInst> insts;

    /*
     * Empties this one for the next function, keeping the table it grew into.
     *
     * The map is one entry per instruction, so building a fresh one per function is an allocation
     * and a rehash per function - which is what a `MachineFunction` declared inside the loop over a
     * module's functions costs. Every caller hoists one out of that loop and resets it here
     * instead, on the same terms as the `RegScratch` and `FunctionRegs` beside it.
     */
    void reset() {
        insts.reset();
    }

    void select(LowerInst* inst, MachineOpcodeId opcode, MachineFormId form, Maybe<LowerCmp> condition) {
        insts.add(inst, MachineInst { inst, opcode, form, condition });
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

// The condition a conditional instruction was selected with: the comparison a branch or a select
// consumed out of the flags, or the one a comparison materializes into a register. Nothing for an
// instruction whose encoding carries no condition code.
Maybe<LowerCmp> selectCondition(LowerInst* inst);

// The width the operation this form describes works at - see OperationWidth. Asked by the encoder to
// choose the operand size, and by the memory-operand rules to decide whether a frame slot is exactly
// as wide as the access that would read it in place.
LowerType operationType(LowerBase base, const MachineForm& form, LowerInst* inst);

// The operand every form of `opcode` reads as a memory address, or -1 where none does. The same
// question MachineForm::addressOperand answers, asked where the final form is not settled yet: the
// address folding runs before the peepholes, and the forms of one opcode are required to agree about
// this (validateMachineForms) precisely so that it has an answer there.
I32 opcodeAddressOperand(MachineOpcodeId opcode);

// Whether some form of `opcode` can swallow `value` as operand `index`. Asked by the peephole that
// embeds immediates, which runs before the final form is chosen and so has to consider every form
// the opcode has - and which has to consider the value, not merely the position: a shift accepts an
// immediate count in an 8-bit field, so a count that only fits in four bytes has to stay in a
// register however embeddable the position is.
bool opcodeCanEmbedImmediate(MachineOpcodeId opcode, Size index, U64 value);

// The opcode an instruction selects, independent of which form it ends up in. Takes the base because
// an operation's opcode can depend on the type of an operand rather than of a result - a comparison
// yields an Int32 whether it compared integers or floats, and only its operands say which machine
// operation it is.
MachineOpcodeId opcodeFor(LowerBase base, LowerInst* inst);

// Checks the form table: that every operand index exists, every tie joins compatible roles, fixed
// registers are allocatable members of their operand's class, at most one operand can occupy the
// single r/m field, and the forms of one opcode agree about the flags unless the opcode says they
// do not. Run once, when the table is built, in debug builds.
bool validateMachineForms(const MachineTarget& target);
