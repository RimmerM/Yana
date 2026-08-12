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

    /*
     * Packed vector arithmetic.
     *
     * Opcodes of their own rather than forms of the scalar ones, for the reason the floating-point
     * set above is: `add` over two `i32x4` and `add` over two `Int` are two target operations
     * reading two register files, and keeping them apart is what lets every form of one opcode agree
     * about the flags. None of these touches the flags at all.
     *
     * One opcode per *operation* and a form per lane type, which is where the widths live: `paddb`,
     * `paddw`, `paddd`, `paddq` and `addps` are five forms of OpVAdd, chosen by the lane the
     * instruction is typed with.
     */
    OpVAdd,
    OpVSub,
    OpVMul,
    OpVDiv,
    OpVAnd,
    OpVOr,
    OpVXor,
    OpVAndNot,
    OpVShl,
    OpVShr,
    OpVSar,
    OpVCmp,

    /*
     * Lanes rearranged within one vector, by a pattern the encoding carries as a trailing byte.
     *
     * One opcode rather than one per lane width, because unlike the arithmetic the *machine's* lane
     * width here need not be the IR's: `pshufd` permutes 32-bit lanes and is what an `i32x4`, an
     * `f32x4` and an `i64x2` shuffle all reach, the last by moving its two halves as pairs of
     * 32-bit ones. What that costs is a domain crossing on a float vector, which `shufps` would
     * avoid and which is a second form of this opcode rather than a second opcode.
     *
     * Two-source shuffles are not here. `pshufd` reads one register, so a pattern naming lanes of
     * both sources needs a sequence, and `selectPackedForm` refuses one rather than picking a
     * source and being quietly wrong about the other.
     */
    OpVShuffle,

    /*
     * Every lane the same scalar.
     *
     * A pseudo rather than a form, because what it expands to depends on which *bank* the scalar
     * arrived in and the two are not the same length. A float lane is already in a vector register,
     * so the broadcast is the shuffle alone; an integer lane is in a general one and needs `movd` or
     * `movq` across the boundary first. Expressing that as two IR instructions would need a kind for
     * "this scalar as lane zero", which the lower IR does not have and which LLVM and JS - both of
     * which lower a splat perfectly well already - would then have to be taught for nothing.
     */
    OpVBroadcast,

    /*
     * One lane read out of a vector.
     *
     * Two shapes that share an opcode because they are one operation: a float lane stays in the
     * vector bank and gets to lane zero by a shuffle, an integer lane crosses to a general register.
     * The second is where SSE4.1 earns its place - `pextrd`/`pextrq` do it in one instruction at any
     * index, where the baseline can only take lane zero (`movd`/`movq`) and would otherwise need a
     * shuffle into a scratch vector register that nothing here can reserve.
     */
    OpVExtract,

    /*
     * One lane written into a vector, which is the extract's mirror and is *not* its shape.
     *
     * Every form is two-address - the destination is the vector being written and every lane but one
     * is carried through it - so where the extract is non-destructive at every width, this one ties
     * and the allocator copies a vector that is still live afterwards. That asymmetry is the machine's
     * and not a choice here: `pinsr` writes its first operand and there is no three-operand spelling
     * of it before VEX.
     *
     * The bank the *scalar* arrives in decides the family rather than the encoding. An integer lane
     * is `pinsr`, whose r/m operand is a general register; a float lane never leaves the vector bank
     * and is `insertps` at a 32-bit lane, `movsd` or `unpcklpd` at a 64-bit one - which are the two
     * halves of a two-lane vector and so cover it exactly, with no feature at all.
     *
     * Where SSE4.1 earns its place is the 32- and 64-bit integer lane. `pinsrw` is the baseline's
     * only insert and reaches a *word*, so a wider lane is the pair or the quadruple of words it is
     * made of - which is `lowerLaneInserts` in transform.cpp, the mirror of `lowerLaneExtracts`.
     */
    OpVInsert,

    /*
     * A lane-wise select: every lane of the result taken from one of two vectors by a mask.
     *
     * A pseudo, and the only vector operation here that needs a scratch register of its own. What
     * the baseline has is `pand`, `pandn` and `por` and no conditional move of a vector at all, so
     * the operation is `(mask & a) | (~mask & b)` - three instructions over two values, the second
     * of which has to be computed somewhere that is neither the destination nor the mask. A clobber
     * is what supplies that, exactly as the float-immediate pseudo's r11 does: it keeps a live value
     * out of one register at this one instruction, where a declared temporary
     * (`MachineForm::temporaries`) would hold one back from the whole function.
     *
     * Written as a pseudo rather than expanded into IR - which is what the reduction and the lane
     * accesses each are - because the expansion needs the mask read as a *vector*, and a bitcast
     * between the two is exactly what the lower validator refuses (a mask is `lanes` booleans on
     * JavaScript, so the reinterpretation means nothing there). The three instructions are one
     * machine operation here and nothing above this backend has to hear about them.
     */
    OpVBlend,

    // A vector or a mask complemented. A pseudo for the same reason `OpVBlend` is: there is no
    // packed `not`, so it is an exclusive-or against an all-ones vector, and producing all ones
    // without a constant pool is a comparison of a scratch register with itself.
    OpVNot,

    // A vector negated, which is a subtraction from zero at an integer lane and a sign bit toggled
    // at a float one. Both need a constant this backend has no pool for and both can build the one
    // they need out of a scratch register, so it is a pseudo rather than a refusal.
    OpVNeg,

    /*
     * The square root and the fused multiply-add, which are one opcode each across both banks.
     *
     * `sqrtss`, `sqrtsd`, `sqrtps` and `sqrtpd` differ in a mandatory prefix and in nothing else, so
     * a scalar square root and a packed one are the same machine operation at different widths -
     * which is what makes this the one arithmetic opcode here that is *not* split into a scalar
     * `OpF-` and a packed `OpV-` pair. The same is true of `vfmadd213`.
     *
     * Neither is two-address in the shape the rest of this table is. `sqrt` writes a destination
     * that need not be its source at every feature level; `vfmadd213` is three-operand by
     * construction, being VEX-only, and ties its destination to the *first* source the way its
     * `213` suffix says.
     */
    OpSqrt,
    OpFma,

    // `vzeroupper`, which a function that wrote a ymm or zmm register owes before every call to one
    // that might not have and before returning - see §5.4 of Implementation-Vector.md. It takes no
    // operands and produces no value; what it does is to the upper halves of the register file.
    OpVZeroUpper,

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

    // Every lane the same scalar: a bank crossing where the lane is an integer, and a shuffle in
    // both cases. See OpVBroadcast for why the pair is not two IR instructions.
    VecBroadcast,

    /*
     * The two vector constants this machine makes out of nothing, which is what keeps them out of
     * the pool - Implementation-Vector.md §5.7.
     *
     * A splat of zero is `pxor r, r` and a splat of all-ones is `pcmpeqd r, r`: one instruction each,
     * no memory, no `.rodata` entry and no general register on the way. They are the two patterns
     * `poolVectorConstants` declines for that reason, and they are the reason it declines anything.
     *
     * The scalar operand is `folded()` - it is not encoded anywhere, the opcode being the whole of
     * the value - so it occupies no location and the immediate that fed it is marked Implicit.
     */
    VecZero,
    VecOnes,

    // A lane-wise select, which the baseline spends as `(mask & a) | (~mask & b)` through a
    // clobbered scratch vector register. See OpVBlend.
    VecSelect,

    // The two that need an all-ones vector, which is a register compared with itself: a mask
    // complemented, and the three signed relations the machine has only the complement of.
    VecNot,
    VecCompareInverted,

    // A 32-bit lane product without SSE4.1's `pmulld`: `pmuludq` multiplies the even lanes only, so
    // the odd ones are shuffled down, multiplied, and the two sets of low halves interleaved back.
    VecMul32,

    // A vector negated: subtracted from a zero the expansion makes, or its sign bits toggled against
    // a mask the expansion makes. Both out of the same scratch.
    VecNegate,

    /*
     * A lane read out of, or written into, a **256-bit** vector.
     *
     * Its own pair rather than a wide twin of the lane forms, because at this width the operation is
     * a different one and not the same instruction re-encoded. Every lane access AMD64 has names a
     * lane inside *one* 128-bit register: `vpextrd` reads one of four dwords whatever `L` says, and
     * `vpinsrd` writes one of four and zeroes everything above the register it wrote. So a lane in
     * the upper half is reached by bringing that half down with `vextracti128`, doing the 128-bit
     * access, and - for a write - putting the half back with `vinserti128`.
     *
     * Two to three instructions and one scratch, which is what makes them pseudos on exactly the
     * terms `VecSelect` is one: the expansion needs a register that is neither operand, and a form
     * cannot declare a temporary (`validateMachineForms`). The float extract is the one of the four
     * that needs no scratch - its destination is a vector register of its own - and it is written as
     * a pseudo anyway, so that "how a lane is reached at this width" is one place rather than two.
     */
    VecWideLane,
    VecWideWithLane,

    // The two intrinsics whose one operation is several instructions (§15.2 of the plan). Both
    // expand after allocation, which is only safe because every register and flag their expansion
    // touches is a fixed operand or a declared clobber of the form that names them - so nothing the
    // expansion does can surprise a placement that has already happened.
    RdTsc,   // the counter's two halves joined into the one value the intrinsic returns
    PortIn8, // a byte read from a port, zero-extended into the whole result
};

/*
 * How the bytes in front of an opcode are written.
 *
 * `Legacy` is the shape every form had before there was a choice: a mandatory prefix byte, then REX
 * if the width or a register number needs one, then the opcode's escape byte. The other two fold all
 * three into one prefix and add what the ModRM byte has no room for - a second source register, so
 * that the operation stops being two-address, and a vector length.
 *
 * It is a property of the *form* rather than of the opcode, which is the whole point: `addsd` and
 * `vaddsd` do the same thing to the same registers and differ in how they are written and in whether
 * the destination has to be one of the sources. So they are two forms of one machine opcode, listed
 * with the feature each needs, and selectForm takes whichever the target can encode.
 */
enum class PrefixEncoding: U8 {
    Legacy,
    Vex,  // the two- and three-byte forms, chosen by which fields are needed
    Evex, // the four-byte form, and the only one that can name xmm16-31 or a mask register
};

// The opcode map a VEX or EVEX prefix names in place of the escape bytes a legacy encoding writes.
enum : U8 {
    kOpcodeMap0F = 1,
    kOpcodeMap0F38 = 2,
    kOpcodeMap0F3A = 3,
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

    // The second operand's type, which is what a store's width is: operand zero is the address it
    // writes through and says nothing about how much of it is written.
    FromUse1,

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

    /*
     * The second source register, which only a VEX or EVEX prefix has anywhere to put.
     *
     * This is the field that makes the difference between `addsd xmm1, xmm2` and
     * `vaddsd xmm1, xmm2, xmm3` a matter of the descriptor rather than of the encoder: with it, the
     * three registers an operation names are ModRM.reg, VEX.vvvv and ModRM.r/m, and the destination
     * no longer has to be one of the sources. The form drops its `tiedDef` accordingly, and the copy
     * the allocator was inserting to satisfy the tie disappears with it.
     *
     * Absent for a form that genuinely has two operands - a compare, a conversion - which is encoded
     * as vvvv naming register zero, since the field is inverted and every such encoding requires the
     * inverted value to be all ones.
     */
    OperandRef vvvvField;

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

    /*
     * Which prefix shape the bytes above are written inside.
     *
     * `prefix` and `escape` keep their meanings under all three: a VEX prefix does not write them as
     * bytes, it carries the same two facts in its `pp` and map fields, so one descriptor describes
     * both spellings of an operation and the encoder reads the same two fields either way. `map` is
     * what an escape of more than one byte would otherwise need a second field for, and is ignored
     * by the legacy shape.
     *
     * `vectorLength` is VEX.L and EVEX.L'L: zero for a scalar operation and for a 128-bit packed
     * one, which are the same encoding and differ only in opcode.
     */
    PrefixEncoding prefixEncoding = PrefixEncoding::Legacy;
    U8 opcodeMap = kOpcodeMap0F;
    U8 vectorLength = 0;

    // How many bytes of the immediate the LoadStore family writes after the address. The other
    // families take the width from the value - one byte or four, according to which opcode was
    // chosen - but a store's immediate is as wide as the *access* rather than as wide as the number,
    // so `movb $0, [rax]` carries one byte and `movq $0, [rax]` carries four regardless of what is
    // being stored. Zero for the forms that carry no immediate at all.
    U8 immediateBytes = 0;

    bool byteRegField = false; // an 8-bit ModRM.reg operand, which needs REX to name spl/bpl/sil/dil
    bool omitWhenSame = false; // emits nothing when source and destination are already the same register

    /*
     * The form writes part of its destination register and leaves the rest of it alone, and its
     * vector-prefixed spelling therefore names a *third* operand to supply what it did not write.
     *
     * True of the scalar instructions that operate on one lane of a register: `cvtss2sd xmm1, xmm2`
     * writes 64 bits and leaves bits 64-127 as it found them, and `vcvtss2sd xmm1, xmm2, xmm3`
     * says where those bits come from instead. Every VEX form marked NDS in the manual is one of
     * these, and the derivation reads this to name the destination there - which is what the legacy
     * encoding did with the bits it left alone, so the twin merges from the same place.
     *
     * It cannot be inferred from the operands, and getting it wrong in either direction is silent
     * or fatal rather than approximate: `sqrtss` and `sqrtps` are one opcode picked apart by a
     * mandatory prefix, the scalar one names a merge source and the packed one requires `vvvv` to
     * be all-ones or the processor raises #UD. So it is stated per form.
     *
     * Only meaningful for a form with no tie. A two-address operation already names its first
     * source, and that is where `dropTie` puts it.
     */
    bool mergesIntoDestination = false;

    // The encoding carries the negation of the selected condition, because the operand the tie
    // already placed in the destination is the one the *un*negated condition would have moved.
    bool negateCondition = false;

    // Materializes the flags into the instruction's first result afterwards - `setcc` and a
    // zero-extension - for a comparison whose result could not be left in the flags.
    bool materializeFlags = false;

    /*
     * The encoding ends in a one-byte *predicate* taken from the selected condition.
     *
     * `cmpps` is one instruction for all eight relations and says which in a trailing byte, where
     * every other comparison here says it in the opcode or leaves it in the flags. That byte is
     * neither an immediate operand - the IR has no value there - nor part of the opcode, so it is
     * its own field: the condition is already selected data (`MachineInst::condition`), and this
     * says the encoder writes it.
     *
     * It is not `immediateBytes`, which says how wide an immediate *operand* the LoadStore family
     * writes. The two never appear together and validateMachineForms says so.
     */
    bool conditionImmediate = false;

    /*
     * The encoding ends in a one-byte lane *pattern* taken from the instruction itself.
     *
     * The same shape as `conditionImmediate` and for the same reason - a trailing byte no operand
     * supplies - but read from a different place. A shuffle's pattern is a field of the lower
     * instruction rather than selected data, because it was decided when the IR was built and
     * nothing between there and here has an opinion about it; `packedShufflePattern` is what turns
     * the IR's one-entry-per-result-lane form into the byte `pshufd` wants.
     *
     * Never set together with `conditionImmediate`: an encoding has one trailing byte or none, and
     * validateMachineForms says so.
     */
    bool patternImmediate = false;
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

    /*
     * §3.5.2.2 Whether ZF answers "is the result zero" after this form has run.
     *
     * A much narrower claim than `flagsEffect`, and it has to be its own field because most of what
     * writes the flags does not support it. `imul` sets CF and OF and leaves SF and ZF *undefined*;
     * `mul`, `div` and `idiv` do the same; a shift by a count of zero leaves every flag exactly as it
     * found it, so `shl r/m, cl` answers about whatever ran before it. Each of those has
     * `flagsEffect = Def` and would be read as an answer about its result by anything that asked the
     * coarser question.
     *
     * What it is for is the comparison a form like this has already performed - see
     * `tryElideCompare` in transform.cpp, which is the only reader.
     */
    bool resultInFlags = false;

    /*
     * §3.5.2.2 And whether SF against OF answers "is the result negative" as well.
     *
     * The second table the field above refers to, and it is a strictly smaller set. Every form with
     * this has `resultInFlags` too, because both are statements that the flags describe the result -
     * but a signed comparison reads SF against OF where an equality reads ZF alone, and an addition
     * that overflowed sets OF to say something about the *operation*: `sub a, b; jl` is `a < b` and
     * not `a - b < 0`.
     *
     * `and`, `or` and `xor` clear OF outright, so on their result `jl` is SF and `jl` is exactly the
     * sign bit. `neg`, `inc`, `dec` and the group-1 arithmetic all set OF from the operation and
     * carry only the coarser claim.
     */
    bool signInFlags = false;

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

    /*
     * The same operation written with a vector prefix, where the target can encode one.
     *
     * Chosen exactly as `memorySource` is - a swap made by selectForm from a fact that is not the
     * instruction's - and the two compose: the alternative is taken first and its own memory twin
     * after it, so a VEX form reading a folded address is one lookup and not a fourth entry.
     *
     * It is not merely a shorter spelling. The VEX forms of the scalar operations are
     * *three-operand*: the destination is named on its own instead of being one of the sources, so
     * the alternative drops the tie and the copy the allocator was inserting to satisfy it. That is
     * the whole reason to prefer it, and it is why this is a different form rather than a different
     * encoding of the same one - the operand constraints differ, and the allocator reads those.
     *
     * `alternativeOf` is the back-pointer, and exists for the same reason `memorySourceOf` does:
     * validateMachineForms has rules about the forms of one opcode agreeing, and an alternative is
     * the one kind that deliberately does not.
     */
    MachineFormId alternative = 0;
    MachineFormId alternativeOf = 0;

    /*
     * The same operation over a 256-bit register.
     *
     * Deliberately *not* an `alternative`. An alternative is one operation encoded better, chosen by
     * what the target can encode and interchangeable with its source at every call site; this is a
     * different operation - it reads and writes twice as many bytes - and which of the two an
     * instruction takes is decided by its own type rather than by the feature set. §5's note about
     * `pextrd` not being an alternative of `movd` is the same distinction, and this is the case it
     * was drawing the line for.
     *
     * The consequence worth stating is that selection asks for it by width and never falls back:
     * `packedForm` reads the 128-bit row, and a 32-byte type takes this link or the operation is
     * refused. A wide value that quietly took its narrow form would write half a vector.
     *
     * VEX makes almost every one of them three-operand, so the twin usually drops the tie its source
     * carried - which is where the wide tier gets shorter as well as wider, since the copy the
     * allocator inserted for that tie disappears with it.
     */
    MachineFormId wide = 0;
    MachineFormId wideOf = 0;

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

    // The names of the derived VEX forms, which are the only ones this table does not read out of a
    // string literal: a twin's name is its source's with a `v` in front, so the text has to be built
    // and held somewhere for the StringViews to point into. Reserved to its final size before the
    // first view is taken, so that nothing here moves once something points at it.
    Array<char> derivedNames;

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
     * The sixteen-byte sign masks a float negation exclusive-ors against, at each width, or null
     * where this function negates nothing of that width.
     *
     * Here rather than on the instruction because a mask is not an operand: `xorps xmm, [rip + m]`
     * names it in the encoding, so nothing about it reaches the allocator and there is no value for
     * the IR to carry. Interned into the module's pool by `poolSignMasks`, which is the one place
     * that has both the context to name a global and the machine record to write it on.
     */
    LowerGlobal* signMask32 = nullptr;
    LowerGlobal* signMask64 = nullptr;

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
        signMask32 = nullptr;
        signMask64 = nullptr;
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

/*
 * The instruction a shuffle's lane pattern is, and the trailing byte it carries.
 *
 * Asked twice for the same reason `selectForm` is: `selectPackedForm` needs to know whether any form
 * applies at all before the allocator runs, and the encoder needs the byte itself afterwards. One
 * function so that "which shuffle is this" and "which control byte" cannot answer differently - and
 * one *answer*, because at more than one candidate instruction the two questions stop being
 * separable: `[0, 4, 1, 5]` is a `punpckldq` and carries no byte, and `[0, 1, 4, 5]` is a `shufps`
 * and carries 0x44, and nothing but the pattern tells them apart.
 *
 * Nothing is returned where no single instruction expresses the pattern, which is now a narrow set:
 * a lane width with no shuffle at all (8 and 16 bits, outside the interleaves), or a two-source
 * pattern that is neither an interleave nor `shufps`'s "two lanes from each side" shape.
 */
struct PackedShuffleChoice {
    MachineFormId form = 0;

    // The control byte, for the two forms that carry one. `hasByte` rather than a zero test: 0x00 is
    // the byte a broadcast of lane zero writes, and a form that carries no byte at all must not have
    // one written after it.
    U8 byte = 0;
    bool hasByte = false;
};

Maybe<PackedShuffleChoice> packedShuffleChoice(LowerInst* inst);

/*
 * The relation a packed comparison is actually emitted at, which is not always the one it names.
 *
 * Four of the eight have no instruction and no predicate of their own and are reached by exchanging
 * the operands instead: `gt` is `lt` read backwards, `ge` is `le`, and `ilt` is `igt`. That exchange
 * is `orderPackedCompare`'s, in transform.cpp.
 *
 * Asked twice for the reason `packedShufflePattern` is, and the reason is sharper here because the
 * two askers stand on opposite sides of the pass that makes the change: `checkVectorSupported` runs
 * at the top of `transformFunction` and sees the written relation, and `selectPackedForm` runs after
 * canonicalization and sees the exchanged one. Read separately, the first refuses a `cmp_ilt` the
 * second would have emitted perfectly well.
 */
LowerCmp packedCompareRelation(LowerCmp cmp);

/*
 * The trailing byte an instruction supplies for the form selected for it - see `patternImmediate`.
 *
 * Three instructions carry one and they carry different things: a shuffle its lane pattern, a lane
 * access its index. One function because the encoder's question is the same for all of them - what
 * byte goes after the encoding - and because a form declaring `patternImmediate` for a kind that has
 * no answer should fail here rather than write a stale byte.
 */
U8 packedTrailingByte(LowerInst* inst);

// The `pshufd` control byte that puts lane `index` of a vector of this lane width into every lane of
// the result. The splat's byte with the index generalized, and the one a float lane extract needs:
// both want a chosen lane spread, and neither cares what lands where beyond lane zero.
U8 broadcastLaneByte(LowerType type, U8 index);

/*
 * The vector operations this backend cannot emit, reported where a program can still be stopped.
 *
 * The same split - and for the same reason - as `checkFrameSupported`, whose comment says it
 * outright: a gap stated as an `assertTrue` inside form selection compiles away in a release build,
 * so the program that stopped in a debug build emits something that is not it in the configuration
 * users actually build. `test/resolve/VecOps.yana` was the demonstration, exiting 255 natively where
 * the same fixture answers 2042 under Node.
 *
 * So the refusals are asked here, before anything is selected or emitted, and the assertions in
 * `opcodeFor` and `selectPackedForm` stay as the backstop that catches this function drifting out of
 * agreement with them. Asking twice is deliberate: the two answers are checked against each other by
 * every debug build of every fixture that reaches one.
 *
 * Returns false having reported. Emission stops on the error count either way; the pipeline still
 * runs, since a half-transformed function is worse to reason about than a whole one.
 */
/*
 * Whether this value is a constant vector that `poolVectorConstants` will replace with a load.
 *
 * Implemented in transform.cpp, beside the pass it speaks for, and read by the refusals above it:
 * an instruction that pass removes is not one this backend has to have a form for. See the comment
 * there for why the two have to ask one function rather than each deciding.
 */
bool isPooledVectorConstant(LowerBase base, LowerValue* value);

bool checkVectorSupported(Context& ctx, LowerBase base, LowerFunction& fun);

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

/*
 * Whether a splat is one of the two constants this machine makes out of nothing - §5.7 of
 * Implementation-Vector.md.
 *
 * Asked by `canEmbedImm`, which is what marks the scalar Implicit: the forms `pxor r, r` and
 * `pcmpeqd r, r` take the operand as `folded()`, so the opcode *is* the value and nothing about the
 * scalar is encoded - and a scalar left occupying a location is a `mov r15d, 0` in front of a `pxor`
 * that does not read it. Exported rather than left static because the question is asked once at
 * selection and once at embedding, and the two must not answer differently - which is the same
 * arrangement `packedShufflePattern` and `packedCompareRelation` already have.
 */
bool splatIsMachineConstant(LowerBase base, LowerInst* inst);

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
