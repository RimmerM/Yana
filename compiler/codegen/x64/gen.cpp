#include "gen.h"
#include "x64_util.h"

/*
 * Emission.
 *
 * This is the last stage, and it decides nothing. Which operation was selected, which form of it,
 * which register or frame slot every operand is in, which constant an immediate carries and what
 * address a memory operand names were all settled upstream - by selection (machine.cpp), by
 * placement (place.cpp) and by legalization (legalize.cpp) - and arrive here as a selected
 * MachineForm and an InstRegs of resolved operands.
 *
 * So there is no switch over instruction kinds here. Emission walks the selected form's encoding
 * descriptor: the family says which shape the instruction has, the opcode fields say which bytes,
 * and the operand references say which resolved operand goes in the ModRM.reg field, which in the
 * r/m field, and which carries the immediate. Adding a regular instruction is a row in the form
 * table; nothing below has to learn its name.
 *
 * The forms that keep an encoder of their own are the ones whose byte layout is genuinely irregular:
 * calls, allocas, the block operations, and the terminators - each of which either expands into
 * several instructions or reads something only the frame layout or the block order knows. They are
 * named by PseudoKind and gathered at the bottom, and they consume the same resolved operands: what
 * they are allowed to read that the families are not is their own instruction's *non-register* data,
 * a block target or a byte count, never where a value lives.
 *
 * checkFormOperands, run in debug builds before each instruction, is what makes "emission rediscovers
 * nothing" a checked claim rather than a convention: it asks the form what it required and compares
 * that against what the allocator produced.
 */

static bool checkFormOperands(const MachineForm& form, const InstRegs& regs) {
    // A call's operands come from the calling convention rather than from the form; the allocation
    // verifier checks those against classifyArgs, which is the only statement of where they go.
    if(form.conventionOperands) return true;

    for(Size i = 0; i < form.uses.size() && i < regs.uses.size(); i++) {
        auto& constraint = form.uses[i];
        auto at = regs.uses[i].at;

        if(constraint.kind == OperandConstraintKind::FixedRegister) {
            if(!at.isPhysical() || at.physicalReg() != constraint.fixedReg) return false;
        }

        // An operand the form says occupies nothing must not have been given a location, and one it
        // says needs a register must not have been left in the frame.
        if(constraint.kind == OperandConstraintKind::None && at.isValid()) return false;
        if(constraint.kind == OperandConstraintKind::Register && at.isStack()) return false;

        // An immediate operand carries a value rather than occupying a place, and one the form's
        // immediate field is actually wide enough for. Checked here rather than inside each encoder
        // so that every family gets it from the one place the form is asked what it required.
        if(constraint.kind == OperandConstraintKind::Immediate) {
            if(at.isValid()) return false;
            if(!regs.uses[i].isImmediate) return false;
            if(!fitsImmediate(constraint.immediate, regs.uses[i].immediate)) return false;
        }
    }

    for(Size i = 0; i < form.defs.size() && i < regs.creates.size(); i++) {
        auto& constraint = form.defs[i];
        auto at = regs.creates[i].at;

        if(constraint.kind == OperandConstraintKind::FixedRegister) {
            if(!at.isPhysical() || at.physicalReg() != constraint.fixedReg) return false;
        }

        // A result the form says occupies nothing - a comparison consumed as flags, an elided
        // direct callee - must not have been given a location.
        if(constraint.kind == OperandConstraintKind::None && at.isValid()) return false;

        // A tied result and the operand it is written over have to be in one place, which is the
        // whole content of the destructive two-address rule.
        if(constraint.tiedOperand != kNoTiedOperand) {
            if(constraint.tiedOperand >= regs.uses.size()) return false;
            if(at.isValid() && at != regs.uses[constraint.tiedOperand].at) return false;
        }
    }

    return true;
}

// The physical register number an encoder writes into an instruction. A frame slot never reaches
// one: a value living in the frame is brought into a scratch register by genMoves before anything
// reads it, and taken back afterwards if it was written.
//
// The number comes from the *view* the operand's class gives the register rather than from the
// location's index, which is what makes the class load-bearing: viewOf rejects a class that does not
// cover the location's bank, where an index read straight out of the location is the same number
// whichever register file it came from.
//
// The general and vector banks have encoders; the mask bank does not, since every `kmov` is
// VEX-encoded. A mask location reaching here is a legalization that produced a location this backend
// cannot emit, and failing loudly is the point: the register model describes banks the encoders do
// not implement, and silently writing an instruction with an unnameable register number in it is the
// one way that can go wrong quietly.
static U8 reg(MachineLocation at, RegisterClassId regClass) {
    assertTrue(!at.isStack());            // an encoder was handed a frame slot
    assertTrue(!at.isRemat());            // an encoder was handed a rematerialization recipe
    assertTrue(at.isPhysical());          // an encoder was handed no location at all
    assertTrue(at.bank != BankMask);      // no encoder emits the mask bank yet
    return targetRegisters().viewOf(regClass, at.physicalReg()).encoding;
}

static U8 reg(const ResolvedOperand& operand) {
    return reg(operand.at, operand.regClass);
}

// The same for a location that is already known to be a physical general register - the frame's
// base, a scratch register the encoder chose for itself. Registers of the other banks never name a
// base or a frame pointer, so the bank is part of what this asserts rather than a parameter.
static U8 reg(PhysicalReg at) {
    assertTrue(at.bank == BankGpr);
    return U8(at.index);
}

inline U8 makeMod(U8 mode, U8 rm, U8 regField) {
    return (mode << 6) | ((regField & 7) << 3) | (rm & 7);
}

inline U8 makeRex(bool is64, U8 rm, U8 regField, U8 index) {
    return 0b01000000 | (is64 ? 0b1000 : 0) | ((regField & 8) >> 1) | ((index & 8) >> 2) | ((rm & 8) >> 3);
}

/*
 * Memory operands.
 *
 * One address representation and one encoder. Every memory reference this backend emits - a frame
 * slot, a folded address, a pointer sitting in a register, the addresses inside an unrolled block
 * operation, an outgoing argument store, a RIP-relative global - is a MachineAddress (gen.h) and is
 * written out by the two functions below. Nothing else writes a ModRM byte for an address.
 *
 * That matters because the special cases are not obvious and are all silent when wrong: rsp and r12
 * can only be a base through a SIB byte, rbp and r13 have no displacement-free encoding, a missing
 * base is a SIB form of its own, and REX.B/REX.X extend the base and index independently. Each of
 * those used to be restated by every encoder that happened to touch memory, and an encoder that
 * restated one of them wrongly produced an instruction addressing something else entirely.
 */

// The ModRM/SIB/displacement bytes and REX bits one MachineAddress turns into.
struct EncodedAddress {
    U8 mod = 0;
    U8 rm = 0;
    U8 sib = 0;
    U32 disp = 0;
    bool hasSib = false;
    bool hasDisp = false;
    bool disp32 = false;
    bool rexB = false;
    bool rexX = false;

    LowerFunction* relocFunction = nullptr;
    LowerGlobal* relocGlobal = nullptr;
};

// The two-bit SIB scale field. Anything else has to have been turned into arithmetic before it got
// here - the addressing unit multiplies by 1, 2, 4 and 8 and by nothing else.
static U8 encodeScale(U8 scale) {
    switch(scale) {
        case 1: return 0;
        case 2: return 1;
        case 4: return 2;
        case 8: return 3;
        default: assertTrue("unencodable address scale" == nullptr); return 0;
    }
}

static EncodedAddress encodeAddress(const MachineAddress& a) {
    EncodedAddress out;
    out.relocFunction = a.relocFunction;
    out.relocGlobal = a.relocGlobal;

    // rsp cannot be an index: 100 in the SIB index field means "no index" instead. r12 encodes the
    // same three bits and *is* a legal index, because REX.X tells the two apart.
    assertTrue(!a.hasIndex || a.index != U8(IntRegister::rsp));
    assertTrue(!a.ripRelative || (!a.hasBase && !a.hasIndex));
    assertTrue(a.ripRelative || (!a.relocFunction && !a.relocGlobal));

    // [rip + disp32] is mod=00 rm=101 - which is exactly the encoding an rbp/r13 base would
    // otherwise take, and why one with no displacement still has to write a zero byte below.
    if(a.ripRelative) {
        out.mod = 0;
        out.rm = 5;
        out.hasDisp = true;
        out.disp32 = true;
        out.disp = U32(a.displacement);
        return out;
    }

    auto indexField = U8(a.hasIndex ? (a.index & 7) : 4);
    auto scaleField = U8(a.hasIndex ? encodeScale(a.scale) : 0);

    if(!a.hasBase) {
        // No base at all. The only encoding is the SIB one with base=101 and mod=00, which means a
        // bare disp32 - carrying an index if there is one, and an absolute address if there is not.
        out.mod = 0;
        out.rm = 4;
        out.hasSib = true;
        out.sib = U8((scaleField << 6) | (indexField << 3) | 5);
        out.hasDisp = true;
        out.disp32 = true;
        out.disp = U32(a.displacement);
        out.rexX = a.hasIndex && needsRex(a.index);
        return out;
    }

    auto fitsIn8 = a.displacement >= -128 && a.displacement <= 127;

    // rbp-based addressing has no displacement-free form - mod=0 with rm=101 means RIP-relative -
    // so a zero displacement there still has to be written out as a zero byte.
    auto isBpLike = (a.base & 7) == 5;
    auto needsDisp = a.displacement != 0 || isBpLike;

    out.mod = !needsDisp ? 0 : (fitsIn8 ? 1 : 2);
    out.hasDisp = needsDisp;
    out.disp = U32(a.displacement);
    out.disp32 = needsDisp && !fitsIn8;
    out.rexB = needsRex(a.base);

    if(a.hasIndex || (a.base & 7) == 4) {
        // rsp/r12 can only be addressed through a SIB byte (rm=100 is reserved for it), and an
        // index needs one in any case.
        out.hasSib = true;
        out.rm = 4;
        out.sib = U8((scaleField << 6) | (indexField << 3) | (a.base & 7));
        out.rexX = a.hasIndex && needsRex(a.index);
    } else {
        out.rm = a.base & 7;
    }

    return out;
}

// The REX prefix an address needs, combined with whatever the operand in the ModRM.reg field needs.
// `forceRex` is for the 8-bit forms: encoding 4-7 as a byte operand names ah/ch/dh/bh unless *some*
// REX prefix is present, which switches them to spl/bpl/sil/dil - the registers the allocator's
// numbering actually means.
static void writeAddressPrefix(AsmModule& to, bool is64, U8 regField, const EncodedAddress& a, bool forceRex = false) {
    if(is64 || forceRex || needsRex(regField) || a.rexB || a.rexX) {
        to.buffer.writeByte(makeRex(is64, a.rexB ? 8 : 0, regField, a.rexX ? 8 : 0));
    }
}

// Writes the ModRM byte, and the SIB/displacement bytes the addressing mode calls for, that
// follow an opcode operating on `a`. Every memory-operand instruction ends the same way.
static void writeAddressOperand(AsmModule& to, U8 regField, const EncodedAddress& a) {
    to.buffer.writeByte(makeMod(a.mod, a.rm, regField));
    if(a.hasSib) to.buffer.writeByte(a.sib);

    if(a.hasDisp) {
        // A symbolic displacement writes a placeholder and records where to patch it, which is the
        // same four bytes a disp32 would have occupied.
        if(a.relocFunction) to.addRelocation(a.relocFunction);
        else if(a.relocGlobal) to.addRelocation(a.relocGlobal);
        else if(a.disp32) to.buffer.writeInt<LittleEndian>(a.disp);
        else to.buffer.writeByte(U8(a.disp));
    }
}

// How one memory-operand instruction is encoded around its address, beyond the address itself.
struct MemForm {
    U8 opCode = 0;
    U8 escape = 0;             // 0x0f, for the two-byte opcodes
    U8 prefix = 0;             // a mandatory prefix (0x66, 0xf3), which has to come *before* REX
    bool is64 = false;         // REX.W
    bool byteRegField = false; // an 8-bit ModRM.reg operand, which needs REX to name spl/bpl/sil/dil
};

// The whole tail of a memory-operand instruction: prefixes, opcode, ModRM, SIB, displacement.
static void genMemory(AsmModule& to, const MachineAddress& address, U8 regField, const MemForm& form) {
    auto a = encodeAddress(address);

    if(form.prefix) to.buffer.writeByte(form.prefix);
    writeAddressPrefix(to, form.is64, regField, a, form.byteRegField && (regField & 7) >= 4);
    if(form.escape) to.buffer.writeByte(form.escape);
    to.buffer.writeByte(form.opCode);
    writeAddressOperand(to, regField, a);
}

/*
 * Register-operand primitives.
 *
 * The bytes every register-to-register shape is made of. Which of them a form takes, and with which
 * opcode, is the descriptor's answer - see the families below.
 */

// `op rm, reg` or `op reg, rm` with both operands in registers: the direction is entirely the
// caller's choice of opcode, since ModRM does not encode one.
static void genRegReg(AsmModule& to, bool is64, U8 rm, U8 regField, U8 opCode, U8 escape = 0, U8 prefix = 0) {
    if(prefix) to.buffer.writeByte(prefix);

    if(is64 || needsRex(rm) || needsRex(regField)) {
        to.buffer.writeByte(makeRex(is64, rm, regField, 0));
    }

    if(escape) to.buffer.writeByte(escape);
    to.buffer.writeByte(opCode);
    to.buffer.writeByte(makeMod(3, rm, regField));
}

// A single-operand instruction where ModRM.reg is a fixed opcode extension rather than a register -
// NEG/NOT/MUL/DIV/INC r/m, or a shift by one or by cl.
static void genRegExt(AsmModule& to, bool is64, U8 rm, U8 opCode, U8 ext, U8 escape = 0, U8 prefix = 0) {
    if(prefix) to.buffer.writeByte(prefix);

    if(is64 || needsRex(rm)) {
        to.buffer.writeByte(makeRex(is64, rm, ext, 0));
    }

    if(escape) to.buffer.writeByte(escape);
    to.buffer.writeByte(opCode);
    to.buffer.writeByte(makeMod(3, rm, ext));
}

static void genZeroReg(AsmModule& to, U8 reg, bool is64) {
    if(is64 || needsRex(reg)) {
        to.buffer.writeByte(makeRex(is64, reg, reg, 0));
    }

    to.buffer.writeByte(0x31);
    to.buffer.writeByte(makeMod(3, reg, reg));
}

// [base + displacement], where `base` is whichever register frame layout chose to hang the frame
// off. This is the only way anything addresses a frame object: the layout owns the arithmetic and
// the encoders only ever see the answer.
static MachineAddress slotAddress(const FrameLayout& frame, MachineLocation slot) {
    FrameReference ref { slot.stackSlot() };
    return MachineAddress::atOffset(reg(frame.baseOf(ref)), frame.offsetOf(ref));
}

// An instruction reading one operand straight out of the frame instead of out of a register: the
// memory form of a two-operand encoding, with `regField` the other operand (or an opcode extension,
// for the forms that have no second register).
//
// This is the whole of what a direct memory operand costs the encoder. Which operand may be one -
// and whether the slot is the right width for the access - was settled by the selected form before
// allocation, so an encoder that reaches here has already been told this form exists.
static void genSlotOperand(AsmModule& to, const FrameLayout& frame, bool is64, U8 regField, MachineLocation slot, U8 opCode, U8 escape = 0, U8 prefix = 0) {
    genMemory(to, slotAddress(frame, slot), regField, MemForm {
        .opCode = opCode, .escape = escape, .prefix = prefix, .is64 = is64,
    });
}

// Materializes a constant into a register, picking the shortest encoding that reproduces `imm`
// exactly at the operation's width.
static void genMovImmValue(AsmModule& to, bool is64, U64 imm, U8 dest) {
    // MOV r32, imm32 (0xb8+r) implicitly zeroes the upper 32 bits of the destination, which is
    // both the truncation a sub-64-bit type wants and a valid encoding of any 64-bit value that
    // happens to fit in a *non-negative* 32-bit one.
    if(!is64 || (imm & 0x7fffffff) == imm) {
        if(needsRex(dest)) to.buffer.writeByte(makeRex(false, dest, 0, 0));
        to.buffer.writeByte(0xb8 + (dest & 7));
        to.buffer.writeInt<LittleEndian>(U32(imm));
    } else if((imm & 0xffffffff80000000) == 0xffffffff80000000) {
        // Negative, but representable as a sign-extended imm32: MOV r/m64, imm32 (REX.W 0xc7 /0)
        // sign-extends rather than zero-extends, so it encodes these in 7 bytes instead of 10.
        // The 0xb8+r form above cannot - it would leave the upper half zeroed.
        to.buffer.writeByte(makeRex(true, dest, 0, 0));
        to.buffer.writeByte(0xc7);
        to.buffer.writeByte(makeMod(3, dest, 0));
        to.buffer.writeInt<LittleEndian>(U32(imm));
    } else {
        // MOV r64, imm64 (REX.W 0xb8+r) - the only encoding that carries all 64 bits.
        to.buffer.writeByte(makeRex(true, dest, 0, 0));
        to.buffer.writeByte(0xb8 + (dest & 7));
        to.buffer.writeLong<LittleEndian>(imm);
    }
}

// `op r/m64, imm` for the group-1 opcodes, by ModRM.reg extension: 0 add, 4 and, 5 sub. Unlike the
// immediate family below this takes a plain integer, because the values here come from the frame
// layout rather than from the program.
static void genGroup1Imm(AsmModule& to, U8 dest, I32 value, U8 ext) {
    to.buffer.writeByte(makeRex(true, dest, 0, 0));

    if(value >= -128 && value <= 127) {
        to.buffer.writeByte(0x83);
        to.buffer.writeByte(makeMod(3, dest, ext));
        to.buffer.writeByte(U8(value));
    } else {
        to.buffer.writeByte(0x81);
        to.buffer.writeByte(makeMod(3, dest, ext));
        to.buffer.writeInt<LittleEndian>(U32(value));
    }
}

// The stack-pointer adjustments the prologue and epilogue are made of. A zero-sized frame emits
// nothing rather than an `add rsp, 0`.
static void genAddImm(AsmModule& to, U8 dest, I32 amount, bool sub) {
    if(amount != 0) genGroup1Imm(to, dest, amount, sub ? 5 : 0);
}

static void genAndImm(AsmModule& to, U8 dest, I32 mask) {
    genGroup1Imm(to, dest, mask, 4);
}

// LEA r64, [base + disp] - used by the epilogue to put rsp back where the saved registers are
// without having to know how far it has wandered, and by an alloca to take a frame object's address.
static void genLeaFrame(AsmModule& to, U8 dest, U8 baseReg, I32 displacement) {
    genMemory(to, MachineAddress::atOffset(baseReg, displacement), dest, MemForm {
        .opCode = 0x8d, .is64 = true,
    });
}

// PUSH r64 (0x50+r) / POP r64 (0x58+r). Both are fixed at 64-bit operand size in long mode, so a
// REX prefix here only ever extends the register number - it never selects the width.
static void genPushReg(AsmModule& to, U8 r) {
    if(needsRex(r)) to.buffer.writeByte(makeRex(false, r, 0, 0));
    to.buffer.writeByte(0x50 + (r & 7));
}

static void genPopReg(AsmModule& to, U8 r) {
    if(needsRex(r)) to.buffer.writeByte(makeRex(false, r, 0, 0));
    to.buffer.writeByte(0x58 + (r & 7));
}

// TEST r/m, reg (0x85): computes rm & reg and discards the result, only setting flags.
static void genTestReg(AsmModule& to, bool is64, U8 a, U8 b) {
    genRegReg(to, is64, a, b, 0x85);
}

// CQO/CDQ (0x99): sign-extends rax into rdx, which is the dividend pair a signed divide reads.
static void genCqo(AsmModule& to, bool is64) {
    if(is64) to.buffer.writeByte(makeRex(true, 0, 0, 0));
    to.buffer.writeByte(0x99);
}

/*
 * Condition codes.
 *
 * One statement of the four-bit code each comparison takes, and three opcode bases that carry it:
 * SETcc is 0x90+cc, Jcc near is 0x80+cc, and CMOVcc is 0x40+cc. A form states its base; the
 * condition itself is selected data, recorded on the MachineInst.
 */

static U8 conditionCode(LowerCmp cmp) {
    switch(cmp) {
        case LowerCmp::eq:  return 0x4;
        case LowerCmp::neq: return 0x5;
        case LowerCmp::gt:  return 0x7;
        case LowerCmp::ge:  return 0x3;
        case LowerCmp::lt:  return 0x2;
        case LowerCmp::le:  return 0x6;
        case LowerCmp::igt: return 0xf;
        case LowerCmp::ige: return 0xd;
        case LowerCmp::ilt: return 0xc;
        case LowerCmp::ile: return 0xe;
    }

    assertTrue(false);
    return 0;
}

static LowerCmp negateCmp(LowerCmp cmp) {
    switch(cmp) {
        case LowerCmp::eq:  return LowerCmp::neq;
        case LowerCmp::neq: return LowerCmp::eq;
        case LowerCmp::gt:  return LowerCmp::le;
        case LowerCmp::ge:  return LowerCmp::lt;
        case LowerCmp::lt:  return LowerCmp::ge;
        case LowerCmp::le:  return LowerCmp::gt;
        case LowerCmp::igt: return LowerCmp::ile;
        case LowerCmp::ige: return LowerCmp::ilt;
        case LowerCmp::ilt: return LowerCmp::ige;
        case LowerCmp::ile: return LowerCmp::igt;
    }

    assertTrue(false);
    return cmp;
}

/*
 * Materializing the flags into a real register, for a comparison whose result could not stay in
 * them. SETcc r/m8 (0f 90+cc /0) writes 1 or 0 into the register's *low byte* and leaves the other
 * three exactly as they were, so something has to clear them before the result is an ordinary 0-or-1
 * Int that later instructions can read in full.
 *
 * There are two ways to do that and they are not equally good. Clearing them afterwards with
 * MOVZX r32, r/m8 (0f b6 /r) needs nothing of the register and works everywhere, which is why it is
 * the fallback - but the `setcc` still merges its byte into whatever the register held, so the
 * instruction depends on the last write to that register however unrelated it was, and on a machine
 * that renames registers whole that dependency is real. Zeroing the register *before* the comparison
 * with `xor r32, r32` costs the same instruction, one byte less, and no dependency at all: the
 * `setcc` writes into a register the same basic block just proved to be zero.
 *
 * The catch is that the zeroing has to go before the comparison, because it writes the flags - so it
 * is only available when the destination is not one of the registers the comparison reads. That is
 * `preZeroesFlagsResult` in genFunction below, and the two halves here are what it picks between.
 */
static void genSetCc(AsmModule& to, U8 reg, LowerCmp cmp) {
    // Encoding 4-7 as an 8-bit operand names ah/ch/dh/bh unless some REX prefix is present, which
    // switches them to spl/bpl/sil/dil - the registers the allocator's numbering actually means.
    auto byteRex = needsRex(reg) || (reg & 7) >= 4;

    if(byteRex) to.buffer.writeByte(makeRex(false, reg, 0, 0));
    to.buffer.writeByte(0x0f);
    to.buffer.writeByte(0x90 + conditionCode(cmp));
    to.buffer.writeByte(makeMod(3, reg, 0));
}

static void genFlagsToReg(AsmModule& to, U8 reg, LowerCmp cmp) {
    genSetCc(to, reg, cmp);

    auto byteRex = needsRex(reg) || (reg & 7) >= 4;

    if(byteRex) to.buffer.writeByte(makeRex(false, reg, reg, 0));
    to.buffer.writeByte(0x0f);
    to.buffer.writeByte(0xb6);
    to.buffer.writeByte(makeMod(3, reg, reg));
}

/*
 * The same, for a floating-point comparison, where two of the six need the parity flag as well.
 *
 * UCOMISS and UCOMISD set CF, ZF and PF together when either operand is a NaN, which is what makes
 * `ja` and `jae` read as ordered comparisons with no help - see orderFloatCompare, which puts every
 * ordering comparison into one of those two. Equality has no such code:
 *
 *   `a == b` is ZF and not PF - the setcc answered 1 for the NaN, and the correction zeroes it
 *   `a != b` is not ZF or PF  - the setcc answered 0 for the NaN, and the correction sets it to 1
 *
 * Written as a short forward jump over the correction rather than as a second setcc and a combining
 * `and`, because that needs a second register and this needs none: the register the result is
 * already in is the only one touched. The jump is over a known instruction whose length is measured
 * rather than predicted, exactly as emitFloatSelect does.
 *
 * The correction writes the flags, which is why this can only run where the comparison's flags are
 * not carried anywhere - and that is guaranteed, because tryMergeCompare refuses to fold a float
 * equality into a branch or a select at all.
 */
static void genFloatFlagsToReg(AsmModule& to, U8 reg, LowerCmp cmp, bool preZeroed) {
    if(preZeroed) genSetCc(to, reg, cmp);
    else genFlagsToReg(to, reg, cmp);

    if(cmp != LowerCmp::eq && cmp != LowerCmp::neq) return;

    // JNP rel8 (0x7b): the ordered case is the one the setcc above already answered, so it skips.
    to.buffer.writeByte(0x7b);
    to.buffer.writeByte(0);
    auto afterJump = to.buffer.offset();

    if(cmp == LowerCmp::eq) {
        // XOR r32, r32 (0x31 /r), which is the shortest zero and needs no immediate.
        genRegReg(to, false, reg, reg, 0x31);
    } else {
        // MOV r32, imm32 (0xb8+rd id). Not `xor` and an `inc`, which would be two instructions to
        // save three bytes on a path a NaN takes.
        if(needsRex(reg)) to.buffer.writeByte(makeRex(false, reg, 0, 0));
        to.buffer.writeByte(0xb8 + (reg & 7));
        to.buffer.writeInt<LittleEndian>(U32(1));
    }

    auto end = to.buffer.offset();
    to.buffer.offset(afterJump - 1);
    to.buffer.writeByte(U8(end - afterJump));
    to.buffer.offset(end);
}

/*
 * Parallel copies.
 *
 * A copy is register-file work rather than instruction work: what it takes to move a value between
 * two places depends on the class it lives in and not at all on what produced it. The encodings are a
 * table per *class* rather than per bank, because two classes over one register file need not move
 * with the same instruction or at the same width - and a class this backend cannot emit a move for
 * fails here rather than being written out as an integer instruction with a vector register number
 * in it.
 */

struct ClassMoveEncoding {
    U8 regToReg = 0;  // register to register
    U8 load = 0;      // register from a frame slot
    U8 store = 0;     // frame slot from a register
    U8 exchange = 0;  // the cycle break, for a class that has one

    U8 escape = 0;      // 0x0f, for the two-byte vector opcodes
    U8 copyPrefix = 0;  // the mandatory prefix a register copy needs
    U8 memPrefix = 0;   // the mandatory prefix a frame transfer needs

    // REX.W on a register-to-register copy, and on the exchange that breaks a cycle. Stated per
    // class rather than per bank, because the two GPR classes are exactly the two widths: a copy of
    // a 32-bit value at 64 bits carries a REX.W it does not need, which is a byte on every copy the
    // allocator emits, and there is one of those at the end of most functions.
    //
    // Narrowing it is safe in the one direction that matters. A 32-bit move clears the upper half of
    // its destination rather than preserving it, so the value the class holds arrives intact and
    // what is destroyed is whatever the register happened to have above it - which nothing can be
    // reading, since a location belongs to one web and a web to one class. The same goes for the
    // exchange: both ends of a transfer are of one class by construction, so `xchg r32, r32` clears
    // two halves that both belong to 32-bit values.
    bool wide = false;

    // The frame transfer's width is stated by `memPrefix` rather than by REX.W, which is how every
    // SSE move states it. A class whose transfers are sized by REX.W takes the width from the
    // *slot* instead - slots are packed by width, so a slot is exactly as wide as the value in it
    // and an access of any other width would take a neighbour with it.
    bool widthInPrefix = false;

    bool defined = false;
};

// One row per class rather than per bank, because two classes over one register file need not move
// with the same instruction: `movss`, `movsd` and `movups` are the same operation at three widths,
// and the width is in the prefix rather than in an operand-size bit.
//
// Ordered rather than designated, since the class ids run in this order and a compiler need not
// accept a designated initializer out of it.
static const ClassMoveEncoding kClassMoves[kRegisterClassCount] = {
    // ClassGpr32, ClassGpr64. MOV r, r/m either way, and XCHG r/m, r to break a cycle without
    // needing a scratch register at all. Each at its own width - see `wide`.
    { .regToReg = 0x8b, .load = 0x8b, .store = 0x89, .exchange = 0x87, .defined = true },
    { .regToReg = 0x8b, .load = 0x8b, .store = 0x89, .exchange = 0x87, .wide = true, .defined = true },

    // ClassFloat32, ClassFloat64: a scalar float in a vector register. Copied whole with MOVAPS
    // rather than with the scalar move, which would merge into the destination's upper bytes and
    // make the copy depend on whatever was there; transferred to and from the frame at the class's
    // own width, since that is all the slot holds.
    //
    // No exchange: the vector file has no XCHG, so a cycle in one goes through a scratch register.
    // That is why the move pool exists per bank rather than once - see TemporaryReserve.
    {
        .regToReg = 0x28, .load = 0x10, .store = 0x11,
        .escape = 0x0f, .memPrefix = 0xf3, .widthInPrefix = true, .defined = true,
    },
    {
        .regToReg = 0x28, .load = 0x10, .store = 0x11,
        .escape = 0x0f, .memPrefix = 0xf2, .widthInPrefix = true, .defined = true,
    },

    // ClassXmm128: a whole vector register, which is what a callee-saved one has to be preserved as
    // whatever narrower value this function put in it. MOVUPS rather than MOVAPS for the frame
    // transfer, because the region it lands in is only 8-byte aligned: the frame's alignment is
    // settled before the allocator knows whether anything will be saved there, and the unaligned
    // encoding is the same length.
    {
        .regToReg = 0x28, .load = 0x10, .store = 0x11,
        .escape = 0x0f, .widthInPrefix = true, .defined = true,
    },

    // ClassYmm256, ClassZmm512, ClassMask32, ClassMask64. Every one of these moves with a VEX- or
    // EVEX-encoded instruction, which this backend does not write - so a location in one reaches
    // genMoves as a loud failure rather than as a legacy encoding with an unnameable register
    // number in it. No IR type produces one; see the plan's stage C.
    {}, {}, {}, {},
};

bool classHasExchange(RegisterClassId regClass) {
    return kClassMoves[regClass].exchange != 0;
}

// Recreates a rematerialized value in `dest` - see Remat in gen.h. Defined below, next to the
// encoders it is made of; declared here because a recipe reaches the machine through genMoves.
static void genRemat(AsmModule& to, const FrameLayout& frame, const Remat& r, MachineLocation dest, RegisterClassId regClass);

// Emits a sequenced permutation of locations (fixed-register constraints, phi placement, the copy
// that feeds a destructive two-address encoding, a value moving between the frame and a register).
// The allocator has already ordered these, so they are emitted exactly as given; a `swap` entry is
// one it could only satisfy with an exchange.
//
// A frame slot at either end makes the move a load or a store, and this is the only place either is
// produced: no family encoder ever sees a stack location it was not told to expect, because a value
// that lives in the frame is brought into a register by one of these before anything reads it.
//
// A load or store is exactly as wide as the slot it touches. Slots are packed by width, so a 4-byte
// value sits 4 bytes from its neighbour and a 64-bit move would take the neighbour with it. A 32-bit
// write also zeroes the rest of the register, which is what a 32-bit value wants anyway.
static void genMoves(AsmModule& to, const FrameLayout& frame, const FrameObjects& objects, const Array<Remat>& remats, SmallBuffer<RegMove> moves) {
    for(auto& m: moves) {
        if(m.from == m.to) continue;

        // A recipe as the source is not a copy at all: nothing holds the value anywhere, so it is
        // recreated straight into the destination. It is never a destination - there is nothing to
        // write to - and so can never be part of a cycle either.
        if(m.from.isRemat()) {
            assertTrue(!m.swap && m.to.isPhysical());
            genRemat(to, frame, remats[m.from.rematId()], m.to, m.regClass);
            continue;
        }

        auto fromSlot = m.from.isStack();
        auto toSlot = m.to.isStack();

        // The move is the class's, not the location's: a slot belongs to the value in it rather than
        // to a register file, so both ends of a transfer are of one class by construction.
        auto& encoding = kClassMoves[m.regClass];
        assertTrue(encoding.defined); // a move between locations of a class this backend cannot emit

        // Only an exchange can be encoded without somewhere to put a third value, and only where the
        // class has one; a cycle through a slot, or through a class that has no exchange, was already
        // broken with a scratch register by sequenceMoves.
        assertTrue(!m.swap || (encoding.exchange != 0 && !fromSlot && !toSlot));

        // A transfer with a slot at both ends is expanded into a load and a store by sequenceMoves,
        // which owns the register it goes through, so none ever reaches an encoder.
        assertTrue(!(fromSlot && toSlot));

        // An integer transfer is as wide as the *slot*, not as the class: slots are packed by width,
        // so a 4-byte value sits 4 bytes from its neighbour and a wider access would take it along.
        // A vector one states its width in the prefix instead and REX.W means nothing there, so the
        // class already decided and the slot has nothing left to say.
        auto slotIs64 = [&](MachineLocation slot) {
            return !encoding.widthInPrefix && objects.slots[slot.stackSlot()].size > 4;
        };

        if(fromSlot) {
            genSlotOperand(to, frame, slotIs64(m.from), reg(m.to, m.regClass), m.from,
                encoding.load, encoding.escape, encoding.memPrefix);
        } else if(toSlot) {
            genSlotOperand(to, frame, slotIs64(m.to), reg(m.from, m.regClass), m.to,
                encoding.store, encoding.escape, encoding.memPrefix);
        } else if(m.swap) {
            genRegReg(to, encoding.wide, reg(m.from, m.regClass), reg(m.to, m.regClass),
                encoding.exchange, encoding.escape, encoding.copyPrefix);
        } else {
            // A register copy names the destination in ModRM.reg for every class described: MOV and
            // MOVAPS are both `op reg, r/m` with the source in r/m.
            genRegReg(to, encoding.wide, reg(m.from, m.regClass), reg(m.to, m.regClass),
                encoding.regToReg, encoding.escape, encoding.copyPrefix);
        }
    }
}

/*
 * The frame.
 */

// Establishes the frame the layout decided on: the caller's rbp is saved and rbp made to point at
// it (so a stack walk can follow the chain), then the callee-saved registers this function actually
// overwrites, then room for the locals and spill slots.
//
// Every part of this is skipped when the layout says it is not needed, so a leaf function that kept
// everything in caller-saved registers still starts at its first real instruction.
// The callee-saved vector registers, stored into (or reloaded out of) their region of the frame in
// ascending register order. Whole registers rather than the class of whatever this function put in
// them: the caller may have been holding a packed value there, and giving back only the low half
// would be silent corruption of a value nothing in this function ever named.
static void genVectorSaves(AsmModule& to, const FrameLayout& frame, bool restore) {
    auto& encoding = kClassMoves[ClassXmm128];
    assertTrue(frame.savedVectors.isEmpty() || encoding.defined); // a bank with no way to preserve it

    U32 offset = 0;

    frame.savedVectors.iterate([&](PhysicalReg saved) {
        auto address = MachineAddress::atOffset(reg(frame.vectorSaveBase),
            frame.vectorSaveOffset + I32(offset));

        genMemory(to, address, U8(saved.index), MemForm {
            .opCode = restore ? encoding.load : encoding.store,
            .escape = encoding.escape,
            .prefix = encoding.memPrefix,
        });

        offset += kVectorSaveSize;
    });
}

static void genPrologue(AsmModule& to, const FrameLayout& frame) {
    auto rbp = U8(IntRegister::rbp);
    auto rsp = U8(IntRegister::rsp);

    if(frame.framePointer) {
        genPushReg(to, rbp);
        genRegReg(to, true, rsp, rbp, 0x8b); // mov rbp, rsp (rm = source, reg = dest)
    }

    // Ascending register order, which is the order the epilogue pops them back in.
    frame.savedRegs.iterate([&](PhysicalReg saved) { genPushReg(to, reg(saved)); });

    // The realignment goes above everything the frame reserves - see the picture in gen.h: the locals
    // and the argument area are then both measured from an rsp the mask has aligned, and the incoming
    // arguments stay where rbp can still reach them.
    if(frame.realignsStack) genAndImm(to, rsp, -I32(frame.dynamicAlignment));

    genAddImm(to, rsp, I32(frame.fixedSize), true);

    // After the reservation, since the region they land in is part of it. A vector register cannot
    // be pushed, so preserving one is a store into the frame like any other - through the same
    // whole-register encoding a 128-bit spill would take.
    genVectorSaves(to, frame, false);
}

static void genEpilogue(AsmModule& to, const FrameLayout& frame) {
    auto rbp = U8(IntRegister::rbp);
    auto rsp = U8(IntRegister::rsp);

    auto savedCount = U32(frame.savedRegs.count());

    // Before rsp moves. Without a frame pointer the region is addressed through rsp, so reloading
    // after the reservation had been released would read whatever the next call wrote there.
    genVectorSaves(to, frame, true);

    if(frame.framePointer) {
        if(savedCount == 0) {
            // LEAVE (0xc9) is `mov rsp, rbp ; pop rbp` in one byte, which is the whole epilogue
            // when there is nothing below rbp to pop back first.
            to.buffer.writeByte(0xc9);
            return;
        }

        // Recovered from rbp rather than by undoing the prologue's arithmetic, which is the point
        // of having a frame pointer: rsp may have been moved since by a dynamic alloca, and the
        // epilogue has no way to know by how much.
        genLeaFrame(to, rsp, rbp, -I32(8 * savedCount));
    } else {
        genAddImm(to, rsp, I32(frame.fixedSize), false);
    }

    // Inline: this is a register set turned into an order, so it is bounded by the machine.
    SmallArray<PhysicalReg, 16> saved;
    frame.savedRegs.iterate([&](PhysicalReg r) { saved.push(r); });
    for(Size i = saved.size(); i > 0; i--) genPopReg(to, reg(saved[i - 1]));

    if(frame.framePointer) genPopReg(to, rbp);
}

// RIP-relative LEA (0x8d, mod=00 rm=101) + a relocation against the target's eventual offset. The
// relocation is written by the shared address encoder in place of the disp32, which is what keeps
// this from being another handwritten ModRM byte.
static void genLoadAddress(AsmModule& to, MachineLocation destReg, RegisterClassId regClass, LowerGlobal* global, LowerFunction* function) {
    genMemory(to, MachineAddress::atSymbol(function, global), reg(destReg, regClass), MemForm {
        .opCode = 0x8d, .is64 = true,
    });
}

// Recreates a rematerialized value in `dest`. This is the whole of what a recipe costs at the point
// it is needed, and it stands in for two instructions rather than one: the definition that no longer
// emits anything and the reload that a frame home would have needed here.
static void genRemat(AsmModule& to, const FrameLayout& frame, const Remat& r, MachineLocation dest, RegisterClassId regClass) {
    switch(r.kind) {
        case Remat::Immediate:
            genMovImmValue(to, is64Bit(r.type), r.imm, reg(dest, regClass));
            break;
        case Remat::GlobalAddress:
            genLoadAddress(to, dest, regClass, r.global, nullptr);
            break;
        case Remat::FunctionAddress:
            genLoadAddress(to, dest, regClass, nullptr, r.function);
            break;
        case Remat::FrameAddress:
            genLeaFrame(to, reg(dest, regClass), reg(frame.baseOf(r.frame)), frame.offsetOf(r.frame));
            break;
    }
}

/*
 * Emission.
 *
 * What one instruction is emitted from: its selected form, its resolved operands, and - for the
 * pseudos alone - the frame layout and the block that follows this one.
 */

struct Emitter {
    AsmModule& to;
    LowerBase base;
    const MachineFunction& machine;
    const FrameLayout& frame;
    const FrameObjects& objects;

    // The block laid out immediately after the one being emitted, which is what lets a branch to it
    // become a fallthrough. Null at the end of the function. This is the next block that is
    // *emitted*, so a block skipped by the bypass below is never something to fall into.
    LowerBlock* next = nullptr;

    // Where a branch naming each block actually has to land, indexed by block index - see the
    // empty-block note in genFunction. Identity for every block that is emitted, and null for a
    // block whose whole content is a return the shared epilogue has taken over (§7.2).
    Buffer<LowerBlock*> bypass;

    LowerBlock* branchTarget(LowerPtr<LowerBlock> block) {
        return bypass[base[block]->index];
    }

    // Every jump this function emits, in the order they were written - the input to relaxBranches.
    SmallArray<AsmBranch, 32> branches;

    // Whether the returns of this function share one epilogue (§7.2), and whether it is what follows
    // the block being emitted - which is the fallthrough rule for it, exactly as `next` is for a
    // block. The two are separate questions: the epilogue sits behind one chosen return rather than
    // at the end, so the last emitted block is usually not the one it follows.
    bool sharedEpilogue = false;
    bool epilogueNext = false;

    // And whether *this* return declined to use it, because the jump it would cost is taken more
    // often than the bytes it would save are worth - see §7.2.2. Per block, since the question is
    // about one return path rather than about the function.
    bool ownEpilogue = false;

    /*
     * A jump, written long with its short form recorded beside it.
     *
     * Neither the distance nor the direction is known here: a forward branch names a block that has
     * not been emitted, and even a backward one is measured against a function that is still going
     * to shrink. So every jump is four bytes of displacement and a note, and relaxBranches settles
     * the whole function's worth at once - see §7.1.
     *
     * `target` is null for the shared epilogue, which is where a branch aimed at a bypassed return
     * block lands. A jump whose target is what comes next is not emitted at all, and that includes
     * the epilogue - but which question that is depends on the target, since a null `next` means
     * "nothing follows" where a null target means "the epilogue does".
     */
    void emitJump(LowerBlock* target) {
        if(target ? target == next : epilogueNext) return;

        auto start = U32(to.buffer.offset());
        to.buffer.writeByte(0xe9); // JMP rel32

        branches.push(AsmBranch { .start = start, .site = U32(to.buffer.offset()),
                                  .block = target, .shortOpcode = 0xeb });
        to.buffer.writeInt<LittleEndian>(0);
    }

    void emitJumpIf(LowerCmp cmp, LowerBlock* target) {
        auto start = U32(to.buffer.offset());
        auto cc = conditionCode(cmp);

        to.buffer.writeByte(0x0f); // Jcc rel32
        to.buffer.writeByte(0x80 + cc);

        branches.push(AsmBranch { .start = start, .site = U32(to.buffer.offset()),
                                  .block = target, .shortOpcode = U8(0x70 + cc) });
        to.buffer.writeInt<LittleEndian>(0);
    }

    // The resolved operand an encoding field names. Emission indexes these and nothing else: which
    // operand is which was decided when the form was selected.
    static const ResolvedOperand& field(const InstRegs& regs, OperandRef ref) {
        assertTrue(!ref.isNone()); // an encoding reading a field its descriptor does not have
        auto& list = ref.result ? regs.creates : regs.uses;
        assertTrue(Size(ref.index) < list.size());
        return list[ref.index];
    }

    /*
     * The encoding families.
     */

    // One register in ModRM.reg and one register or frame slot in r/m. An operand left in the frame
    // has to occupy the r/m field, so if the operand this encoding puts *there* is a register and
    // the other one is in the frame, the same operation in its other direction puts them the right
    // way round - which is what `opcodeAlt` is.
    void emitRegRm(const EncodingDescriptor& e, const InstRegs& regs, bool is64) {
        auto& regOp = field(regs, e.regField);
        auto& rmOp = field(regs, e.rmField);

        if(regOp.isStack()) {
            assertTrue(e.opcodeAlt != 0); // an operand in the frame in a direction that does not exist
            genSlotOperand(to, frame, is64, reg(rmOp), regOp.at, e.opcodeAlt, e.escape, e.prefix);
        } else if(rmOp.isStack()) {
            genSlotOperand(to, frame, is64, reg(regOp), rmOp.at, e.opcode, e.escape, e.prefix);
        } else if(!e.omitWhenSame || regOp.at != rmOp.at) {
            genRegReg(to, is64, reg(rmOp), reg(regOp), e.opcode, e.escape, e.prefix);
        }
    }

    // One r/m operand with an opcode extension in the ModRM.reg field.
    void emitRmExt(const EncodingDescriptor& e, const InstRegs& regs, bool is64) {
        auto& rmOp = field(regs, e.rmField);

        if(rmOp.isStack()) {
            genSlotOperand(to, frame, is64, e.extension, rmOp.at, e.opcode, e.escape, e.prefix);
        } else {
            genRegExt(to, is64, reg(rmOp), e.opcode, e.extension, e.escape, e.prefix);
        }
    }

    // The same, with an immediate the encoding carries. The shorter opcode is taken whenever the
    // value fits in a byte; a form with no imm32 encoding of its own requires that it does, which is
    // exactly what its declared immediate width says.
    void emitRmExtImm(const MachineForm& form, const InstRegs& regs, bool is64) {
        auto& e = form.encoding;
        auto& rmOp = field(regs, e.rmField);
        auto& immOp = field(regs, e.immField);
        assertTrue(immOp.isImmediate); // an immediate field naming an operand that carries no value

        // A comparison against zero has a shorter equivalent that names the register twice.
        if(e.zeroRegOpcode && immOp.immediate == 0 && rmOp.isPhysical()) {
            genRegReg(to, is64, reg(rmOp), reg(rmOp), e.zeroRegOpcode, e.escape);
            return;
        }

        auto value = immOp.immediate;
        assertTrue(fitsImmediate(form.immediateWidth(), value)); // an immediate this form cannot carry

        auto isImm8 = fitsImm8(value);
        assertTrue(isImm8 || e.opcodeAlt != 0); // a wider immediate in a direction that does not exist

        auto opcode = isImm8 ? e.opcode : e.opcodeAlt;

        if(rmOp.isStack()) {
            genSlotOperand(to, frame, is64, e.extension, rmOp.at, opcode, e.escape, e.prefix);
        } else {
            genRegExt(to, is64, reg(rmOp), opcode, e.extension, e.escape, e.prefix);
        }

        if(isImm8) to.buffer.writeByte(U8(value));
        else to.buffer.writeInt<LittleEndian>(U32(value));
    }

    // reg, r/m and an immediate: the three-operand `imul`, whose destination can differ from both of
    // its sources.
    void emitRegRmImm(const MachineForm& form, const InstRegs& regs, bool is64) {
        auto& e = form.encoding;
        auto& regOp = field(regs, e.regField);
        auto& rmOp = field(regs, e.rmField);
        auto& immOp = field(regs, e.immField);
        assertTrue(immOp.isImmediate);

        auto value = immOp.immediate;
        assertTrue(fitsImmediate(form.immediateWidth(), value)); // an immediate this form cannot carry

        auto isImm8 = fitsImm8(value);
        assertTrue(isImm8 || e.opcodeAlt != 0);

        auto destReg = reg(regOp);
        auto srcReg = reg(rmOp);

        if(is64 || needsRex(destReg) || needsRex(srcReg)) {
            to.buffer.writeByte(makeRex(is64, srcReg, destReg, 0));
        }

        if(e.escape) to.buffer.writeByte(e.escape);
        to.buffer.writeByte(isImm8 ? e.opcode : e.opcodeAlt);
        to.buffer.writeByte(makeMod(3, srcReg, destReg));

        if(isImm8) to.buffer.writeByte(U8(value));
        else to.buffer.writeInt<LittleEndian>(U32(value));
    }

    // A constant materialized into a register.
    void emitMoveImm(const EncodingDescriptor& e, const InstRegs& regs, bool is64) {
        auto& immOp = field(regs, e.immField);
        assertTrue(immOp.isImmediate);

        genMovImmValue(to, is64, immOp.immediate, reg(field(regs, e.regField)));
    }

    // An address materialized into a register rather than dereferenced.
    void emitLea(const EncodingDescriptor& e, const InstRegs& regs, bool is64) {
        assertTrue(regs.hasAddress); // an address encoding with no address resolved for it

        genMemory(to, regs.address, reg(field(regs, e.regField)), MemForm {
            .opCode = e.opcode, .escape = e.escape, .prefix = e.prefix, .is64 = is64,
        });
    }

    // A memory access, with the other operand in ModRM.reg - or an opcode extension there, for the
    // store of an immediate, which has no second register.
    void emitLoadStore(const MachineForm& form, const InstRegs& regs, bool is64) {
        auto& e = form.encoding;
        assertTrue(regs.hasAddress); // a memory access with no address resolved for it

        auto regField = e.regField.isNone() ? e.extension : reg(field(regs, e.regField));

        genMemory(to, regs.address, regField, MemForm {
            .opCode = e.opcode, .escape = e.escape, .prefix = e.prefix,
            .is64 = is64, .byteRegField = e.byteRegField,
        });

        if(!e.immField.isNone()) {
            auto& immOp = field(regs, e.immField);
            assertTrue(immOp.isImmediate);
            assertTrue(fitsImmediate(form.immediateWidth(), immOp.immediate)); // a wider constant needs a register

            // As wide as the access rather than as wide as the number - see `immediateBytes`. The
            // narrow widths truncate, which is what the store does anyway; the 64-bit one writes
            // four bytes the processor sign-extends, which is what Imm32 above is the constraint for.
            switch(e.immediateBytes) {
                case 1: to.buffer.writeByte(U8(immOp.immediate)); break;
                case 2: to.buffer.writeShort<LittleEndian>(U16(immOp.immediate)); break;
                case 4: to.buffer.writeInt<LittleEndian>(U32(immOp.immediate)); break;
                default: assertTrue("no store encoding carries an immediate of this width" == nullptr);
            }
        }
    }

    // Opcode bytes and nothing else: every operand the instruction has is a register the encoding
    // names for itself, which the form states as fixed operands so that the allocator still knows.
    void emitOpcode(const EncodingDescriptor& e, bool is64) {
        if(e.prefix) to.buffer.writeByte(e.prefix);
        if(is64) to.buffer.writeByte(makeRex(true, 0, 0, 0));
        if(e.escape) to.buffer.writeByte(e.escape);

        to.buffer.writeByte(e.opcode);
        if(e.opcodeAlt) to.buffer.writeByte(e.opcodeAlt);
    }

    // An operation whose opcode carries a condition code: CMOVcc r, r/m.
    void emitConditional(const EncodingDescriptor& e, const MachineInst& selected, const InstRegs& regs, bool is64) {
        assertTrue(selected.condition.isJust()); // a conditional form selected without a condition
        auto condition = selected.condition.unwrap();
        if(e.negateCondition) condition = negateCmp(condition);

        genRegReg(to, is64, reg(field(regs, e.rmField)), reg(field(regs, e.regField)),
            e.opcode + conditionCode(condition), e.escape);
    }

    // The register is part of the opcode byte rather than of a ModRM one.
    void emitOpcodeReg(const EncodingDescriptor& e, const InstRegs& regs, bool is64) {
        auto r = reg(field(regs, e.rmField));

        if(is64 || needsRex(r)) to.buffer.writeByte(makeRex(is64, r, 0, 0));
        if(e.escape) to.buffer.writeByte(e.escape);
        to.buffer.writeByte(e.opcode + (r & 7));
    }

    /*
     * The pseudos.
     *
     * Each one either expands into several instructions or reads something the encoding descriptor
     * cannot state: the frame layout, the block order, or a compile-time byte count. They consume
     * the same resolved operands as everything else.
     */

    // One step of an unrolled block operation: a MOV of `width` bytes between `regField` and the
    // address `[base + offset]`. Both directions and every width go through the shared address
    // encoder, so the block operations get the rsp/r12 SIB byte, the rbp/r13 displacement and the
    // byte-register REX rule from the same place every other memory access does.
    void emitBlockStep(U8 baseReg, U64 offset, U8 width, U8 regField, bool store) {
        static const U8 loadOps[2] = { 0x8a, 0x8b };  // MOV r8, r/m8 and MOV r16/32/64, r/m
        static const U8 storeOps[2] = { 0x88, 0x89 }; // MOV r/m8, r8 and MOV r/m, r16/32/64

        genMemory(to, MachineAddress::atOffset(baseReg, I32(offset)), regField, MemForm {
            .opCode = (store ? storeOps : loadOps)[width == 1 ? 0 : 1],
            .prefix = U8(width == 2 ? 0x66 : 0),
            .is64 = width == 8,
            .byteRegField = width == 1,
        });
    }

    // The widest move that still fits in what is left to copy. Descending powers of two, so a size
    // that is not one is finished off by progressively narrower moves rather than by a byte loop.
    static U8 blockStepWidth(U64 remaining) {
        return remaining >= 8 ? 8 : remaining >= 4 ? 4 : remaining >= 2 ? 2 : 1;
    }

    // A stack allocation is one of two quite different things depending on whether its size is known.
    //
    // A compile-time size was turned into a frame object by placement, so the frame is already the
    // right size and all that is left is to take the object's address - one `lea`, and no change to
    // the stack pointer at all.
    //
    // A size only known at run time has to move the stack pointer, which is why such a function is
    // required to have a frame pointer (see frame.cpp): everything else in the frame is addressed
    // through rbp and so does not care where rsp has ended up. The result register doubles as the
    // scratch for rounding the size up, so the count operand survives for whatever else reads it.
    //
    // An allocation that wants more alignment than the frame keeps rsp on is aligned in the *result*
    // rather than by masking rsp: masking rsp would undo the alignment the prologue established and
    // move the outgoing argument area out from under the stack pointer, where every callee looks for
    // it. So the size is padded by the extra the rounding can cost and the address is rounded up
    // inside the region that pays for it.
    void emitAlloca(LowerInst* inst, const InstRegs& regs, bool dynamic) {
        auto dest = reg(regs.creates[0]);
        auto rsp = U8(IntRegister::rsp);
        auto wanted = I32(((LowerInstAlloca*)inst)->alignment);

        if(!dynamic) {
            // The frame object carries the alignment, and frame layout has already put it on a
            // boundary that satisfies it - verifyFrameLayout checks exactly that - so the address is
            // one `lea` however strongly aligned it is.
            auto ref = objects.references.getValue(inst);
            assertTrue(ref.isJust()); // a fixed allocation placement created no frame object for
            genLeaFrame(to, dest, reg(frame.baseOf(ref.unwrap())), frame.offsetOf(ref.unwrap()));
            return;
        }

        assertTrue(frame.framePointer); // guaranteed by FrameObjects::hasDynamicAlloca

        // The allocation has to keep the stack pointer on the boundary it was already on, so the
        // size is rounded up before it is subtracted rather than the result being masked afterwards.
        auto stackAlignment = I32(frame.dynamicAlignment);

        // Room for rounding the result up, when it is going to be. `wanted - 1` is the most that
        // rounding can move the address, so a region that much larger still holds the whole
        // allocation above it.
        auto slack = wanted > stackAlignment ? wanted - 1 : 0;

        if(dest != reg(regs.uses[0])) genRegReg(to, true, reg(regs.uses[0]), dest, 0x8b);

        genAddImm(to, dest, slack + stackAlignment - 1, false);
        genAndImm(to, dest, -stackAlignment);
        genRegReg(to, true, rsp, dest, 0x29);  // sub rsp, dest

        // The allocation sits above the outgoing argument area, which stays at the bottom of the
        // stack so that the next call still finds its arguments where the callee looks for them. A
        // function that passes nothing on the stack has no area to step over, and the address is
        // just rsp.
        if(frame.argAreaSize > 0) {
            genLeaFrame(to, dest, rsp, I32(frame.argAreaSize));
        } else {
            genRegReg(to, true, rsp, dest, 0x8b);
        }

        if(slack != 0) {
            genAddImm(to, dest, wanted - 1, false);
            genAndImm(to, dest, -wanted);
        }
    }

    /*
     * The floating-point expansions.
     *
     * Three operations AMD64 has no single scalar instruction for. Each reads only its own resolved
     * operands and the width its form states; none of them looks at the IR.
     */

    // MOVD/MOVQ between a vector register and a general one. `toVector` picks the direction; the
    // opcode is the only thing that differs, since both are `66 [REX.W] 0f xx /r` with the vector
    // register in ModRM.reg and the general one in r/m.
    void emitMoveAcrossBanks(U8 vectorReg, U8 generalReg, bool is64, bool toVector) {
        genRegReg(to, is64, generalReg, vectorReg, toVector ? 0x6e : 0x7e, 0x0f, 0x66);
    }

    // A floating-point constant. No SSE encoding carries one and there is no constant pool to load
    // it from, so the bit pattern is materialized in r11 - which the form declares as a clobber, so
    // nothing live is in it here - and moved across.
    void emitFloatImm(LowerInst* inst, const InstRegs& regs, bool is64) {
        auto imm = (LowerImm*)inst;
        auto scratch = U8(IntRegister::r11);

        // The IR keeps every float constant as a double, so a single-precision one is rounded to
        // what it will actually be before its bits are taken.
        auto bits = U64(0);
        if(is64) {
            F64 value = imm->f;
            copyMem(&value, &bits, sizeof(value));
        } else {
            float value = float(imm->f);
            U32 narrow = 0;
            copyMem(&value, &narrow, sizeof(value));
            bits = narrow;
        }

        // Positive zero is the one pattern that needs neither register nor bank crossing: `xorps`
        // against itself clears the whole vector, which is one two-byte instruction where the pair
        // below is ten. Negative zero is *not* this - its sign bit is set - and the bit test says so
        // rather than a comparison against 0.0, which would read the two as equal.
        auto destination = reg(regs.creates[0]);
        if(bits == 0) {
            genRegReg(to, false, destination, destination, 0x57, 0x0f); // xorps xmm, xmm
            return;
        }

        genMovImmValue(to, is64, bits, scratch);
        emitMoveAcrossBanks(destination, scratch, is64, true);
    }

    // Negation. The sign bit is toggled in a general register rather than exclusive-ored against a
    // vector sign mask, which would need either a constant pool or a second vector register - see
    // the form table. r11 is the form's declared clobber.
    void emitFloatNeg(const InstRegs& regs, bool is64) {
        auto value = reg(regs.creates[0]);
        auto scratch = U8(IntRegister::r11);

        emitMoveAcrossBanks(value, scratch, is64, false);

        // BTC r/m, imm8 (0f ba /7 ib): complement one bit and leave the rest alone.
        genRegExt(to, is64, scratch, 0xba, 7, 0x0f);
        to.buffer.writeByte(is64 ? 63 : 31);

        emitMoveAcrossBanks(value, scratch, is64, true);
    }

    // A select between two vector registers, for want of a CMOVcc that can name one. The tie has
    // already put the first operand in the destination, so what is left is to skip the copy of the
    // second when the condition holds - which is a forward jump over exactly that one instruction.
    void emitFloatSelect(const EncodingDescriptor& e, const MachineInst& selected, const InstRegs& regs) {
        assertTrue(selected.condition.isJust()); // a conditional form selected without a condition

        auto dest = reg(field(regs, e.regField));
        auto source = reg(field(regs, e.rmField));

        // Nothing to choose between: both arms would write the register the value is already in.
        if(dest == source) return;

        // Jcc rel8 (0x70+cc), whose displacement is the length of the copy that follows it. Written
        // as a placeholder and patched rather than predicted, so that the jump stays correct however
        // the copy is encoded.
        to.buffer.writeByte(0x70 + conditionCode(selected.condition.unwrap()));
        to.buffer.writeByte(0);
        auto afterJump = to.buffer.offset();

        genRegReg(to, false, source, dest, e.opcode, e.escape, e.prefix);

        auto end = to.buffer.offset();
        to.buffer.offset(afterJump - 1);
        to.buffer.writeByte(U8(end - afterJump));
        to.buffer.offset(end);
    }

    /*
     * The intrinsic expansions.
     *
     * The two intrinsics whose one operation is several instructions. Both are here for the same
     * reason the floating-point expansions above are - there is no single instruction that does it -
     * and both are safe to expand this late for the reason §15.2 of the plan gives: every register
     * and flag they touch is a fixed operand or a declared clobber of the form that selected them,
     * so placement had already been told about all of it.
     */

    // RDTSC (0f 31) answers in edx:eax, and the intrinsic's result is the whole counter - so the
    // halves are joined here. rdx is the form's clobber and the shift is why it declares the flags;
    // rax is its result, which the tie-free fixed def already put in place.
    void emitRdTsc() {
        to.buffer.writeByte(0x0f);
        to.buffer.writeByte(0x31);

        // SHL rdx, 32 (REX.W c1 /4 ib), then OR rax, rdx.
        genRegExt(to, true, U8(IntRegister::rdx), 0xc1, 4);
        to.buffer.writeByte(32);
        genRegReg(to, true, U8(IntRegister::rax), U8(IntRegister::rdx), 0x09);
    }

    // IN al, dx (ec) writes only the low byte of eax and leaves the rest of it holding whatever it
    // held, where the intrinsic's result is a whole Int. MOVZX eax, al (0f b6 c0) fills the rest,
    // and touches no register but the one the result is already in - which is why this expansion
    // needs neither a clobber nor a scratch register to be legal here.
    void emitPortIn8() {
        to.buffer.writeByte(0xec);
        genRegReg(to, false, U8(IntRegister::rax), U8(IntRegister::rax), 0xb6, 0x0f);
    }

    void emitBranch(LowerInst* inst, const MachineInst& selected, const InstRegs& regs) {
        auto je = (LowerInstJe*)inst;
        auto condition = selected.condition.unwrap();

        auto whenTrue = branchTarget(je->then);
        auto whenFalse = branchTarget(je->otherwise);

        // Two arms that land in the same place are one jump, or none. The two edges were distinct
        // when the branch was built; what makes them the same address is a bypassed block on one of
        // them, which is also the case where the condition has nothing left to decide.
        if(whenTrue == whenFalse) {
            emitJump(whenTrue);
            return;
        }

        // Whichever successor the block order put next is the one that costs nothing to fall into,
        // so the branch is emitted around it. When neither is next, the conditional branch is
        // followed by an unconditional one.
        if(whenFalse == next) {
            emitJumpIf(condition, whenTrue);
        } else if(whenTrue == next) {
            emitJumpIf(negateCmp(condition), whenFalse);
        } else {
            emitJumpIf(condition, whenTrue);
            emitJump(whenFalse);
        }
    }

    void emitPseudo(PseudoKind kind, LowerInst* inst, const MachineInst& selected, const InstRegs& regs) {
        // A pseudo works out its own widths, but three of them are one operation at two widths and
        // take it from the form like any family encoder would - see PseudoKind.
        auto& form = machineTarget().form(selected.form);
        auto pseudoIs64 = [&] { return is64Bit(operationType(base, form, inst)); };

        switch(kind) {
            case PseudoKind::None:
                assertTrue("a pseudo form with no encoder" == nullptr);
                break;

            case PseudoKind::Nop:
                to.buffer.writeByte(0x90);
                break;

            case PseudoKind::CallDirect: {
                // The callee's address is a rel32 in the instruction rather than a value in a
                // register, which is why the Fun that produced it was elided.
                auto callee = base[inst->used()[0]];
                to.buffer.writeByte(0xe8);
                to.addRelocation(base[((LowerInstFun*)callee->inst())->target]);
                break;
            }

            case PseudoKind::CallIndirect: {
                // CALL r/m64 (0xff /2). Always 64-bit in long mode, so REX only extends the number.
                auto r = reg(regs.uses[0]);
                if(needsRex(r)) to.buffer.writeByte(makeRex(false, r, 0, 0));
                to.buffer.writeByte(0xff);
                to.buffer.writeByte(makeMod(3, r, 2));
                break;
            }

            case PseudoKind::Syscall:
                // SYSCALL (0f 05). There is no callee to resolve: used()[0] is the syscall number,
                // which the convention placed in rax like any other constrained argument.
                to.buffer.writeByte(0x0f);
                to.buffer.writeByte(0x05);
                break;

            case PseudoKind::Return:
                // After the terminator's own moves, which have already placed the return values in
                // the registers the convention returns them in. Those and the saved registers are
                // disjoint by construction - a result register is one the call clobbers, and a saved
                // register is one it doesn't - so restoring here cannot overwrite a returned value.
                //
                // A shared epilogue (§7.2) is placed directly behind one return, which is the one
                // that costs nothing: it falls into it. Every other return jumps, unless §7.2.2
                // decided this one's jump is taken too often to be worth the bytes.
                if(sharedEpilogue && epilogueNext) {
                    assertTrue(regs.postMoves.size() == 0); // a copy between the return and the epilogue
                    break;
                }

                if(sharedEpilogue && !ownEpilogue) {
                    emitJump(nullptr);
                    break;
                }

                genEpilogue(to, frame);
                to.buffer.writeByte(0xc3);
                break;

            // Nothing at all - see FormNoReturn. The parallel copies in front of this terminator
            // have already been emitted by the caller, which is correct rather than wasteful: a
            // block with no successors carries no phi arguments, so there are none.
            case PseudoKind::NoReturn:
                break;

            case PseudoKind::Jump:
                emitJump(branchTarget(((LowerInstJmp*)inst)->then));
                break;

            case PseudoKind::Branch:
                emitBranch(inst, selected, regs);
                break;

            case PseudoKind::AllocaFixed:
                emitAlloca(inst, regs, false);
                break;

            case PseudoKind::AllocaDynamic:
                emitAlloca(inst, regs, true);
                break;

            case PseudoKind::BlockCopyRep:
                to.buffer.writeByte(0xf3);
                to.buffer.writeByte(0xa4);
                break;

            case PseudoKind::BlockSetRep:
                to.buffer.writeByte(0xf3);
                to.buffer.writeByte(0xaa);
                break;

            case PseudoKind::BlockCopyUnrolled: {
                auto copy = (LowerInstCopy*)inst;
                auto n = ((LowerImm*)base[copy->count]->inst())->i;

                auto toReg = reg(regs.uses[0]);
                auto fromReg = reg(regs.uses[1]);

                // r11 is reserved as a scratch register for this encoding - the unrolled form
                // declares it as both a temporary and a clobber (machine.cpp), which is what
                // guarantees no live value occupies it at this instruction.
                U8 scratch = (U8)IntRegister::r11;
                U64 offset = 0;

                while(offset < n) {
                    auto width = blockStepWidth(n - offset);

                    emitBlockStep(fromReg, offset, width, scratch, false);
                    emitBlockStep(toReg, offset, width, scratch, true);

                    offset += width;
                }
                break;
            }

            case PseudoKind::BlockSetUnrolled: {
                auto set = (LowerInstSetPattern*)inst;
                auto n = ((LowerImm*)base[set->count]->inst())->i;

                auto toReg = reg(regs.uses[0]);
                auto patReg = reg(regs.uses[2]);
                U64 offset = 0;

                while(offset < n) {
                    auto width = blockStepWidth(n - offset);
                    emitBlockStep(toReg, offset, width, patReg, true);
                    offset += width;
                }
                break;
            }

            case PseudoKind::FloatImm:
                emitFloatImm(inst, regs, pseudoIs64());
                break;

            case PseudoKind::FloatNeg:
                emitFloatNeg(regs, pseudoIs64());
                break;

            case PseudoKind::FloatSelect:
                emitFloatSelect(machineTarget().form(selected.form).encoding, selected, regs);
                break;

            case PseudoKind::RdTsc:
                emitRdTsc();
                break;

            case PseudoKind::PortIn8:
                emitPortIn8();
                break;
        }
    }

    /*
     * One instruction.
     */

    // Whether a comparison that has to materialize its result can have that register zeroed ahead of
    // the comparison itself rather than extended afterwards - see genSetCc.
    //
    // The zeroing goes *before* the comparison, because it writes the flags, so the one thing that
    // rules it out is the comparison reading the register it would destroy. Every place the encoding
    // could name one is checked: the two operands, wherever legalization resolved them to, and the
    // base and index of the one address the instruction may reference. A prelude is refused outright
    // rather than reasoned about - no form that materializes the flags has one today, and one that
    // did would be establishing state of its own before the encoding runs.
    //
    // An operand in a frame slot names no register of its own: it is addressed through the frame
    // base, which is rsp - never allocatable - or rbp, which a function addressing its frame that
    // way does not hand out either.
    bool preZeroesFlagsResult(const EncodingDescriptor& e, const InstRegs& regs) {
        if(!e.materializeFlags || e.prelude != EncodingPrelude::None) return false;

        auto at = regs.creates[0].at;
        assertTrue(at.isPhysical()); // a materialized comparison with nowhere to write its result

        for(auto& use: regs.uses) {
            if(use.at.isPhysical() && use.at.physicalReg() == at.physicalReg()) return false;
        }

        if(regs.hasAddress && at.bank == BankGpr) {
            auto& address = regs.address;
            if(address.hasBase && address.base == at.index) return false;
            if(address.hasIndex && address.index == at.index) return false;
        }

        return true;
    }

    // The step some forms need before the operation itself, because the encoding reads a register
    // the instruction does not name as an operand.
    void emitPrelude(const EncodingDescriptor& e, const InstRegs& regs, bool is64) {
        switch(e.prelude) {
            case EncodingPrelude::None:
                break;

            case EncodingPrelude::ZeroRdx:
                // A divide reads rdx:rax as its dividend. Zeroing rdx with `xor` is safe even though
                // it writes the flags, since the divide overwrites them anyway.
                genZeroReg(to, U8(IntRegister::rdx), is64);
                break;

            case EncodingPrelude::SignExtendRax:
                genCqo(to, is64);
                break;

            case EncodingPrelude::TestLastUse: {
                // `test r, r` sets ZF exactly when the register is zero, which is what turns a
                // condition that arrived in a register into flags the conditional encoding reads.
                assertTrue(regs.uses.size() > 0);
                auto r = reg(regs.uses[regs.uses.size() - 1]);
                genTestReg(to, false, r, r);
                break;
            }
        }
    }

    void emitInst(LowerInst* inst, const InstRegs& regs) {
        auto& selected = machine[inst];
        auto& form = machineTarget().form(selected.form);
        auto& e = form.encoding;

        // Debug builds only: what the selected form required is what the allocator produced.
        assertTrue(checkFormOperands(form, regs));

        // A rematerialized value is recreated wherever it is read and kept nowhere in between, so
        // the instruction that would have defined it emits nothing at all. Every recipe kind is free
        // of side effects by the same rule that made it rematerializable.
        if(inst->createdCount == 1 && regs.creates[0].isRemat()) return;

        // The operand size, for the families that have one. A form that emits nothing has no
        // operands to take it from, and a pseudo's expansion works out its own widths - a `ret`
        // would otherwise be asked about the width of a result it does not have.
        //
        // An SSE form states its width in the mandatory prefix, and REX.W there either means nothing
        // or means the width of an *integer* operand, so the bit is not derived from the operation's
        // type at all. What the type still decides for those forms is whether an operand may be read
        // out of a frame slot, which is directMemoryOperands' question rather than this one.
        auto hasWidth = e.family != EncodingFamily::None && e.family != EncodingFamily::Pseudo;
        auto is64 = hasWidth && !e.widthInPrefix && is64Bit(operationType(base, form, inst));

        // The zeroing half of materializing a comparison into a register, which has to stand ahead of
        // the comparison because it writes the flags - see genSetCc. `is64` is not this instruction's
        // width: `xor r32, r32` clears the whole register, so the narrow encoding is right whatever
        // the comparison was of.
        auto preZeroed = preZeroesFlagsResult(e, regs);
        if(preZeroed) genZeroReg(to, reg(regs.creates[0]), false);

        emitPrelude(e, regs, is64);

        switch(e.family) {
            case EncodingFamily::None:        break;
            case EncodingFamily::Opcode:      emitOpcode(e, is64); break;
            case EncodingFamily::RegRm:       emitRegRm(e, regs, is64); break;
            case EncodingFamily::RmExt:       emitRmExt(e, regs, is64); break;
            case EncodingFamily::RmExtImm:    emitRmExtImm(form, regs, is64); break;
            case EncodingFamily::RegRmImm:    emitRegRmImm(form, regs, is64); break;
            case EncodingFamily::MoveImm:     emitMoveImm(e, regs, is64); break;
            case EncodingFamily::Lea:         emitLea(e, regs, is64); break;
            case EncodingFamily::LoadStore:   emitLoadStore(form, regs, is64); break;
            case EncodingFamily::Conditional: emitConditional(e, selected, regs, is64); break;
            case EncodingFamily::OpcodeReg:   emitOpcodeReg(e, regs, is64); break;
            case EncodingFamily::Pseudo:      emitPseudo(e.pseudo, inst, selected, regs); break;
        }

        // And the `setcc` itself, which is all that is left where the register was zeroed above and
        // is followed by the zero-extension where it was not. A floating-point comparison reads the
        // parity flag as well for the two of its six that need it - see genFloatFlagsToReg.
        if(e.materializeFlags) {
            auto condition = selected.condition.unwrap();

            if(machineTarget().form(selected.form).opcode == OpFCmp) {
                genFloatFlagsToReg(to, reg(regs.creates[0]), condition, preZeroed);
            } else if(preZeroed) {
                genSetCc(to, reg(regs.creates[0]), condition);
            } else {
                genFlagsToReg(to, reg(regs.creates[0]), condition);
            }
        }
    }
};

/*
 * Blocks that turned out to emit nothing.
 *
 * Splitting an edge (§3.2) gives a phi transfer somewhere to live, and it has to be done before the
 * allocator runs, so it is done wherever a transfer *could* be needed. What is left once the
 * allocator has coalesced what it can is, in most cases, a block whose entire content is a jump - and
 * a jump to a jump is a branch that could have gone to the second one directly.
 *
 * That was free while the extra block sat on a path taken once. Loop rotation puts one on the back
 * edge of every rotated loop, where the second jump is paid every iteration and is exactly the one
 * the rotation exists to remove, so it stops being free.
 *
 * The answer is not to emit them. A block with no instructions, no moves and an unconditional jump
 * contributes no bytes and no label anything needs: every branch naming it is pointed past it, and
 * nothing can fall into it, since a fallthrough goes to the next block that *is* emitted and a
 * terminator that wanted to reach it emits a real jump when that block is not the one. The entry
 * block is excluded, because what falls into it is the prologue rather than a terminator, and so
 * there is nothing to redirect.
 *
 * `bypass` is filled in with the block a branch naming each block should name instead: the block
 * itself wherever it is emitted, and otherwise the end of the chain of skipped blocks it starts.
 *
 * A shared epilogue (§7.2) adds one case to the same rule. A block whose entire content is a return
 * emits exactly one jump once the epilogue has been lifted out of it, so it is a block that emits
 * nothing by the definition above - and what a branch naming it should name instead is the epilogue,
 * which is not a block. Null is that answer, and it is why every consumer of a branch target here
 * takes a null one as meaning the epilogue rather than as meaning nothing. This is where the "four
 * failure conditions branch straight to the shared exit" shape comes from: the conditional branch
 * that already existed is retargeted, and no jump is added at all.
 *
 * That is the case where a return block emits *nothing*. The one where it emits the same thing as
 * another return block is §7.2.1, and it is decided after emission rather than here, because what
 * two blocks emit is a question about bytes.
 */
static bool emitsNothing(LowerBase base, FunctionRegs& regs, LowerBlock* block, bool sharedEpilogue) {
    if(block->instructions.isNotEmpty()) return false;

    auto kind = base[block->terminator]->kind;
    if(kind != LowerInst::Jmp && !(sharedEpilogue && kind == LowerInst::Ret)) return false;

    auto found = regs.legalized.blocks.get(block);
    assertTrue(found.isJust());

    // The terminator is the only entry a block with no instructions has, and a phi transfer is what
    // would be in its `moves` - see BlockRegs. For a return those moves are the copies that place
    // the returned values, which is exactly what makes such a block one that still emits something.
    auto& termRegs = found.unwrap().insts[0];
    return termRegs.moves.size() == 0 && termRegs.postMoves.size() == 0;
}

// The block a chain of skipped blocks ends at, or null where it ends at the shared epilogue. A
// return has no successor to follow, so it is where the walk stops.
static LowerBlock* endOfChain(LowerBase base, SmallArray<bool, 64>& skipped, LowerBlock* block) {
    while(block && skipped[block->index]) {
        if(base[block->terminator]->kind == LowerInst::Ret) return nullptr;
        block = base[block->outgoing[0]];
    }

    return block;
}

/*
 * Whether the block emitted in front of block `i` reaches it by falling into it.
 *
 * Which is the whole of what decides whether a return block may be taken out (§7.2): the branch
 * that named it is retargeted at the epilogue and costs exactly what it cost before, but a
 * *fallthrough* has nothing to retarget and would become a jump that was not there. `j` is the
 * block emitted before `i`, so everything between them is already skipped and `i` is what `j`
 * falls into if it falls into anything.
 */
template<class Blocks>
static bool fallenInto(LowerBase base, Blocks blocks, SmallArray<bool, 64>& skipped, Size i) {
    Size j = i;
    while(j > 0 && skipped[--j]) {}
    if(skipped[j]) return false;

    // Both slots, since a block with one successor leaves the second null rather than absent.
    auto target = base[blocks[i]];
    for(auto successor: base[blocks[j]]->outgoing) {
        if(successor && endOfChain(base, skipped, base[successor]) == target) return true;
    }

    return false;
}

// Whether any block is emitted after this one. Where none is, the epilogue is what a fallthrough
// reaches either way, so a return here survives being retargeted at it.
template<class Blocks>
static bool anythingAfter(Blocks blocks, SmallArray<bool, 64>& skipped, Size i) {
    for(Size k = i + 1; k < blocks.size(); k++) if(!skipped[k]) return true;
    return false;
}

template<class Blocks>
static void computeBypass(LowerBase base, FunctionRegs& regs, Blocks blocks, bool sharedEpilogue,
                          SmallArray<bool, 64>& skipped, SmallArray<LowerBlock*, 64>& bypass)
{
    for(Size i = 0; i < blocks.size(); i++) {
        // Indexed by block index throughout, which the layout has already made equal to the
        // position - see InvariantBlocksOrdered.
        assertTrue(base[blocks[i]]->index == BlockIndex(i));
        skipped.push(i != 0 && emitsNothing(base, regs, base[blocks[i]], false));
    }

    // A cycle of blocks that all emit nothing is a loop with an empty body, and skipping all of them
    // would leave the jump that closes it with nowhere to land. One block of each cycle is kept,
    // which is what makes the chain below terminate.
    for(Size i = 0; i < blocks.size(); i++) {
        if(!skipped[i]) continue;

        auto block = base[blocks[i]];
        Size steps = 0;

        while(skipped[block->index] && steps <= blocks.size()) {
            block = base[block->outgoing[0]];
            steps++;
        }

        if(steps > blocks.size()) skipped[i] = false;
    }

    // The return blocks, after the jump chains and in layout order - each answer depends on which
    // blocks in front of it are emitted, and a return is never part of a chain, so the two rounds do
    // not interfere. A block declined here keeps its own epilogue like any other return.
    if(sharedEpilogue) {
        for(Size i = 1; i < blocks.size(); i++) {
            if(skipped[i] || !emitsNothing(base, regs, base[blocks[i]], true)) continue;
            skipped[i] = !anythingAfter(blocks, skipped, i) || !fallenInto(base, blocks, skipped, i);
        }
    }

    for(Size i = 0; i < blocks.size(); i++) {
        bypass.push(endOfChain(base, skipped, base[blocks[i]]));
    }
}

/*
 * §7.2.2 What a jump on a return path is worth in bytes.
 *
 * There is no principled number here. It is an exchange rate between bytes and executed
 * instructions, which are not the same unit, so it is stated once - in one place, with the
 * measurement that set it beside it - rather than implied by a rule somewhere.
 *
 * At 32: a return taken on every call of its function has to save 32 bytes before it will jump to a
 * shared epilogue, and one taken on an eighth of them has to save four. That is what separates the
 * two shapes §15.2 of `test/bench/findings.md` measured. `allocateHeap`'s failure arms are taken
 * almost never and share a six-byte saving each; `Tree`'s `build` has two returns, both taken on
 * half its calls, and sharing an eleven-byte saving between them cost 17 ms - which is the whole of
 * what a size-only rule got wrong.
 */
static constexpr U64 kBytesPerReturnJump = 28;

// Whether a return emitting `jmp <epilogue>` rather than its own copy is worth it, given how often
// that jump runs. The saving is what the epilogue costs less the two bytes of jump; the cost is one
// jump per execution of this block, stated relative to one execution of the function.
static bool worthJumpingToEpilogue(const FunctionFrequencyInfo& frequency, BlockIndex block, U32 epilogueSize) {
    auto saved = U64(epilogueSize - 2);
    return saved * kEntryFrequency >= kBytesPerReturnJump * frequency.frequencyOf(block);
}

/*
 * §7.1 Branch relaxation.
 *
 * AMD64 has two encodings of every jump - a one-byte displacement and a four-byte one - and the
 * short one is four bytes smaller for a conditional and three for an unconditional. Which one a
 * branch may take is not something the encoder can answer: a forward branch names a block that has
 * not been emitted, and a backward one is measured against a function that every branch below it is
 * still going to shrink. So every jump is written long (see Emitter::emitJump) and all of them are
 * settled here at once, when the function is finished and nothing else will move it.
 *
 * The fixpoint starts optimistic - every branch short - and only ever takes one back to its long
 * form. That direction is what makes it terminate and what makes it good: shrinking a branch brings
 * every other branch's target closer, so an assignment reached from below can only be improved on by
 * another pass, where one reached from above would need a branch to be lengthened after the fact.
 * At most one pass per branch is possible; one or two is what functions actually take.
 *
 * The rewrite afterwards is a compaction rather than a patch. Spans between branches are copied
 * verbatim - they contain no offset measured from anything that moved, since a RIP-relative
 * displacement is resolved from its recorded site and a jump is exactly what is being rewritten -
 * and everything recorded against the old layout is remapped through `moved`: the block offsets, the
 * relocation sites inside this function, and the byte ranges reported to `onInst`.
 *
 * The three hand-written rel8 jumps in the expansions above (emitFloatSelect, genFloatFlagsToReg)
 * are not branches in this sense and must never become ones: each measures a distance to the end of
 * the instruction it is part of, so it stays correct precisely because its whole span moves
 * together. A jump that crosses another instruction has to be recorded here instead.
 *
 * A block target is resolved here and never reaches resolveRelocations. It cannot be deferred, since
 * the whole point is that its size is the thing being decided, and it need not be: both ends are
 * inside a function that is already placed.
 */

// One emitted thing's byte range, held until relaxation has moved it - a range is only worth
// reporting once it is final. `inst` is null for the prologue and the shared epilogue.
struct EmittedRange {
    LowerInst* inst;
    const InstRegs* regs;
    U32 start;
    U32 end;
};

/*
 * §7.2.1 A return tail already emitted, which a later one may turn out to be a copy of.
 *
 * `start` is the block's first byte and `content` runs to just before its jump to the shared
 * epilogue - the jump is excluded because the block hosting the epilogue does not have one, and it
 * is the best thing to merge into: a tail aimed at it reaches the epilogue by falling into it.
 */
struct ReturnTail {
    LowerBlock* block;
    U32 start;
    U32 contentEnd;
};

static bool equalBytes(const Byte* a, const Byte* b, U32 count) {
    for(U32 i = 0; i < count; i++) if(a[i] != b[i]) return false;
    return true;
}

// What a function occupies in the module being built, which is what relaxation has to move.
struct FunctionExtent {
    U32 codeStart;    // its first emitted byte, after any alignment padding and prefix data
    Size firstBlock;  // its first entry in AsmModule::blocks
    Size firstReloc;  // its first entry in AsmModule::relocations
    U32 epilogue;     // where the shared epilogue landed; unused when there is none
};

static void relaxBranches(AsmModule& to, SmallArray<AsmBranch, 32>& branches,
                          const FunctionExtent& extent, SmallArray<EmittedRange, 128>& ranges)
{
    auto count = branches.size();
    if(count == 0) return;

    // What each branch's target is in the layout as first written. These do not change: it is the
    // mapping onto the new layout that does, and it is computed from these.
    auto reserved = U32(count);
    SmallArray<U32, 32> targets(reserved);
    SmallArray<U32, 32> ends(reserved);

    for(Size i = 0; i < count; i++) {
        auto block = branches[i].block;

        if(block) {
            auto found = to.blockOffsets.getValue(block);
            assertTrue(found.isJust()); // a branch naming a block that was never emitted
            targets.push(to.blocks[found.unwrap()].startOffset);
        } else {
            targets.push(extent.epilogue);
        }

        ends.push(branches[i].site + 4);
    }

    // The bytes removed by the first k branches, rebuilt whenever the assignment changes.
    SmallArray<U32, 33> removed(reserved + 1);
    for(Size i = 0; i <= count; i++) removed.push(0);

    auto rebuild = [&] {
        for(Size i = 0; i < count; i++) {
            auto longSize = branches[i].site + 4 - branches[i].start;
            removed[i + 1] = removed[i] + (branches[i].isShort ? longSize - 2 : 0);
        }
    };

    // Where an offset in the old layout ends up in the new one. A branch ending exactly at it counts
    // as being in front of it: everything from a branch's last byte onwards moves with the branch.
    auto moved = [&](U32 at) {
        Size low = 0, high = count;

        while(low < high) {
            auto middle = (low + high) / 2;
            if(ends[middle] <= at) low = middle + 1;
            else high = middle;
        }

        return at - removed[low];
    };

    for(Size i = 0; i < count; i++) branches[i].isShort = true;

    for(bool changed = true; changed;) {
        changed = false;
        rebuild();

        for(Size i = 0; i < count; i++) {
            if(!branches[i].isShort) continue;

            // Measured from the byte after the short encoding, which is where the CPU counts from.
            auto delta = I64(moved(targets[i])) - I64(moved(branches[i].start) + 2);
            if(delta >= -128 && delta <= 127) continue;

            branches[i].isShort = false;
            changed = true;
        }
    }

    rebuild();

    /*
     * The compaction. `write` never runs ahead of `read`, since a branch either keeps its length or
     * loses bytes, so the spans move towards the front of the buffer and overlap in the safe
     * direction - `moveMem` states that rather than relying on it.
     */
    auto bytes = to.buffer.buffer;
    auto emitted = U32(to.buffer.offset());
    U32 read = extent.codeStart;
    U32 write = extent.codeStart;

    for(Size i = 0; i < count; i++) {
        auto& branch = branches[i];

        if(auto span = branch.start - read) {
            moveMem(bytes + read, bytes + write, span);
            write += span;
        }

        assertTrue(write == moved(branch.start)); // the mapping and the rewrite disagree
        to.buffer.offset(write);

        auto target = moved(targets[i]);

        if(branch.isShort) {
            to.buffer.writeByte(branch.shortOpcode);
            to.buffer.writeByte(U8(I8(I64(target) - I64(write + 2))));
        } else if(branch.shortOpcode == 0xeb) {
            to.buffer.writeByte(0xe9);
            to.buffer.writeInt<LittleEndian>(U32(I32(target) - I32(write + 5)));
        } else {
            to.buffer.writeByte(0x0f);
            to.buffer.writeByte(U8(0x80 + (branch.shortOpcode - 0x70)));
            to.buffer.writeInt<LittleEndian>(U32(I32(target) - I32(write + 6)));
        }

        write = U32(to.buffer.offset());
        read = branch.site + 4;
    }

    if(auto tail = emitted - read) {
        moveMem(bytes + read, bytes + write, tail);
        write += tail;
    }

    to.buffer.offset(write);

    // Everything the old layout was recorded in. Nothing outside this function is in any of them:
    // the extent's three indices are where it started, and a function is never revisited.
    for(Size i = extent.firstBlock; i < to.blocks.size(); i++) {
        to.blocks[i].startOffset = moved(to.blocks[i].startOffset);
        to.blocks[i].endOffset = moved(to.blocks[i].endOffset);
    }

    for(Size i = extent.firstReloc; i < to.relocations.size(); i++) {
        to.relocations[i].siteOffset = moved(to.relocations[i].siteOffset);
    }

    for(auto& range: ranges) {
        range.start = moved(range.start);
        range.end = moved(range.end);
    }
}

void genFunction(Context& context, LowerBase base, AsmModule& to, LowerFunction& fun, const MachineFunction& machine, FunctionRegs& regs, InstEmitCallback onInst, void* onInstCtx) {
    auto blocks = fun.blocks.contents(base);
    to.startFunction(base, &fun);

    // The two halves of the allocation the encoders read: the abstract frame objects and recipes
    // placement produced, and the resolved locations legalization produced against them.
    auto& objects = regs.placement.frame;
    auto& remats = regs.placement.remats;

    // The stack is worked out in full before a byte is emitted: the prologue has to come first but
    // depends on things only the whole function decides (which registers were saved, whether rsp is
    // going to move, how much room the locals need), and every frame reference in the body needs
    // the same answers. See frame.cpp.
    auto frame = computeFrameLayout(context, base, fun, targetConstraints(), regs);
    assertTrue(verifyFrameLayout(context, fun, objects, frame)); // debug builds only

    FunctionExtent extent {
        .codeStart = U32(to.buffer.offset()),
        .firstBlock = to.blocks.size(),
        .firstReloc = to.relocations.size(),
    };

    /*
     * §7.2 One epilogue per function.
     *
     * Every `Return` used to restore the frame for itself, so a function with five of them carried
     * five copies of the same pops - 94 such sites across the program corpus, 899 bytes of them.
     * Lifting the epilogue to the end of the function and sending the returns there removes all but
     * one copy, at the cost of a jump per return that has to be sent.
     *
     * That jump executes, and *where* it is charged is what decides whether the trade is a good one.
     * Putting the epilogue at the end of the function gives the free fallthrough to the block laid
     * out last, which block ordering has already made the least likely one - so the hot return pays
     * and the cold return does not. On the program corpus that was **+31 ms**, nearly all of it on
     * the two programs whose hot path is a call per element.
     *
     * So the epilogue goes behind the *first* return in layout order instead. That is the return on
     * the likely path, since §3.2 lays a branch's likely successor out as its fallthrough, and it is
     * the one that now costs nothing. The jumps land on the arms that are laid out later, which are
     * the arms taken less often - and most of them are not emitted at all: a block whose whole
     * content is a return emits nothing once the epilogue has left it, so computeBypass takes it out
     * and the branch that named it names the epilogue instead. `fallenInto` is the one case that
     * looks free and is not, a branch that reached such a block by falling into it having nothing to
     * retarget.
     */
    auto epilogueSize = [&] {
        // Measured by writing it rather than predicted from the frame: the bytes are the exact
        // answer, and a second statement of what genEpilogue emits is a second thing to keep in
        // step. Nothing is written past here yet, so rewinding over it leaves the buffer as it was.
        auto at = U32(to.buffer.offset());
        genEpilogue(to, frame);
        auto size = U32(to.buffer.offset()) - at + 1; // with the `ret` it is followed by
        to.buffer.offset(at);
        return size;
    }();

    /*
     * Who uses the shared epilogue, which decides whether there is one.
     *
     * Two rounds, because the question is circular in one direction only: whether a return block is
     * *taken out* depends on there being an epilogue for its branches to name, and whether there is
     * an epilogue depends on how many returns would use it. So it is answered optimistically and the
     * bypass recomputed if the answer turns out to be nobody.
     *
     * `ownEpilogue` is §7.2.2's per-return decision and the reason a use has to be counted rather
     * than assumed: a return that declines to jump is not a user, and a function whose only other
     * return declines has one user and therefore no epilogue to share.
     */
    SmallArray<bool, 64> skipped;
    SmallArray<LowerBlock*, 64> bypass;
    SmallArray<bool, 64> ownEpilogue;

    auto sharedEpilogue = epilogueSize > 2;
    auto host = blocks.size();

    for(auto attempt = 0; attempt < 2; attempt++) {
        skipped.clear();
        bypass.clear();
        ownEpilogue.clear();
        host = blocks.size();

        computeBypass(base, regs, blocks, sharedEpilogue, skipped, bypass);
        for(Size i = 0; i < blocks.size(); i++) ownEpilogue.push(false);

        if(!sharedEpilogue) break;

        // The block the epilogue is placed behind: the first emitted return there is. A function
        // whose every return block was taken out has none, and the epilogue then goes at the end,
        // where only the branches that name it reach it. A bypassed return is a user either way -
        // it reaches the epilogue through a branch that already existed.
        Size users = 0;

        for(Size i = 0; i < blocks.size(); i++) {
            if(base[base[blocks[i]]->terminator]->kind != LowerInst::Ret) continue;

            if(skipped[i]) { users++; continue; }
            if(host == blocks.size()) { host = i; users++; continue; }

            // Only a return that carries work of its own. A block holding nothing but the return is
            // one the bypass rule above already makes free wherever it can, and declining to share
            // it writes the whole epilogue out to save a jump that is the entire block - measured at
            // 184 bytes across the corpus and no time at all.
            ownEpilogue[i] = base[blocks[i]]->instructions.isNotEmpty() &&
                !worthJumpingToEpilogue(regs.frequency, BlockIndex(i), epilogueSize);
            if(!ownEpilogue[i]) users++;
        }

        if(users > 1) break;

        // Nobody, so the return blocks it took out have to come back and every return writes its own.
        sharedEpilogue = false;
    }

    Emitter emitter { to, base, machine, frame, objects };
    emitter.bypass = Buffer<LowerBlock*> { bypass.pointer(), bypass.size() };
    emitter.sharedEpilogue = sharedEpilogue;

    // Held rather than reported as they are emitted: relaxation below rewrites the function, so an
    // offset is only final once it has run. The prologue and the shared epilogue belong to the
    // function rather than to any instruction in it and are reported with a null one, in that order.
    SmallArray<EmittedRange, 128> ranges;
    InstRegs none;

    // The return tails emitted so far, which is what §7.2.1 compares a new one against.
    SmallArray<ReturnTail, 16> tails;

    auto prologueStart = U32(to.buffer.offset());
    genPrologue(to, frame);

    if(onInst && U32(to.buffer.offset()) != prologueStart) {
        ranges.push(EmittedRange { nullptr, &none, prologueStart, U32(to.buffer.offset()) });
    }

    for(Size i = 0; i < blocks.size(); i++) {
        if(skipped[i]) continue;

        auto b = base[blocks[i]];

        /*
         * Where this block began, and what the lists holding its side effects looked like - which is
         * what §7.2.1 rewinds to if the block turns out to be a copy of one already emitted. The
         * three marks are the whole of what a block contributes besides bytes: the jumps it wrote,
         * the relocations it wrote, and the ranges it reported.
         */
        auto blockStart = U32(to.buffer.offset());
        auto branchMark = U32(emitter.branches.size());
        auto relocationMark = U32(to.relocations.size());
        auto rangeMark = U32(ranges.size());

        to.startBlock(b);

        auto found = regs.legalized.blocks.get(b);
        assertTrue(found.isJust());
        auto& blockRegs = found.unwrap();
        auto insts = b->instructions.contents(base);

        assertTrue(blockRegs.insts.size() == insts.size() + 1);

        // Keep track of the block that will be positioned immediately after this one, which is what
        // lets a terminator branching to it emit no jump at all. The next block *emitted*: one that
        // was skipped is not somewhere control can arrive.
        emitter.next = nullptr;
        for(Size j = i + 1; j < blocks.size(); j++) {
            if(!skipped[j]) { emitter.next = base[blocks[j]]; break; }
        }

        // The one block the epilogue is placed behind, whose return therefore falls into it. Nothing
        // else about this block's layout changes: a block that ends in a return has no fallthrough
        // of its own for the epilogue to have got in the way of. A function with no return to host
        // it puts it at the end, which is the last emitted block having nothing after it.
        emitter.epilogueNext = sharedEpilogue &&
            (i == host || (host == blocks.size() && emitter.next == nullptr));
        emitter.ownEpilogue = ownEpilogue[i];

        // Operand placement, the instruction itself, then result placement - uniform for every
        // instruction, so nothing that emits code has to remember to handle its own moves.
        for(Size j = 0; j < insts.size(); j++) {
            auto inst = base[insts[j]];
            auto& instRegs = blockRegs.insts[j];
            auto start = U32(to.buffer.offset());

            genMoves(to, frame, objects, remats, instRegs.moves);
            emitter.emitInst(inst, instRegs);
            genMoves(to, frame, objects, remats, instRegs.postMoves);

            if(onInst) ranges.push(EmittedRange { inst, &instRegs, start, U32(to.buffer.offset()) });
        }

        auto termStart = U32(to.buffer.offset());
        auto terminator = base[b->terminator];
        auto& termRegs = blockRegs.insts[insts.size()];

        // A terminator's moves include the copies that feed the successor's phis, so they have to
        // land before the branch itself. For a return under a shared epilogue they are the copies
        // placing the returned values, and everything up to here is the block's *content* - what
        // §7.2.1 compares, with the jump to the epilogue left out of it.
        genMoves(to, frame, objects, remats, termRegs.moves);
        auto contentEnd = U32(to.buffer.offset());
        emitter.emitInst(terminator, termRegs);
        genMoves(to, frame, objects, remats, termRegs.postMoves);

        if(onInst) ranges.push(EmittedRange { terminator, &termRegs, termStart, U32(to.buffer.offset()) });

        to.endBlock(b);

        /*
         * §7.2.1 The same return, written twice.
         *
         * Sharing the epilogue leaves the *setup* duplicated: four arms that answer zero are four
         * copies of `mov eax, 0` and four jumps to one epilogue, where LLVM keeps one copy and aims
         * the four branches at it. Which two of those are the same is a question about bytes, and the
         * bytes exist only once the block has been emitted - so it is emitted, compared, and rewound
         * over where it turns out to be a copy. Nothing structural is compared and nothing about an
         * encoding is restated: equal bytes is the strongest form of the question and the cheapest.
         *
         * The block stays in `AsmModule::blocks` and is pointed at the copy that survives, which is
         * what redirects every branch naming it - `relaxBranches` resolves a target through that
         * table, so nothing else has to learn about the merge.
         *
         * Three conditions, and each is a way the rewind could lose something the bytes do not carry:
         * a relocation written inside the block would be resolved against a site that no longer
         * exists; a block reached by falling into it has no branch to retarget; and the block hosting
         * the epilogue is the one thing here that must stay where it is.
         */
        auto isTail = base[b->terminator]->kind == LowerInst::Ret && termRegs.postMoves.size() == 0;
        auto merged = false;

        // A return that kept its own epilogue (§7.2.2) is still something a later copy can be
        // merged into - landing there runs the epilogue inline and is one jump *cheaper* - but it is
        // never merged away itself, since the block it landed on might be one that jumps.
        if(sharedEpilogue && isTail && i != host && !ownEpilogue[i] &&
           to.relocations.size() == relocationMark && !fallenInto(base, blocks, skipped, i))
        {
            for(auto& tail: tails) {
                if(tail.contentEnd - tail.start != contentEnd - blockStart) continue;

                auto a = to.buffer.buffer + tail.start;
                auto c = to.buffer.buffer + blockStart;
                if(!equalBytes(a, c, contentEnd - blockStart)) continue;

                to.buffer.offset(blockStart);
                emitter.branches.resize(branchMark);
                ranges.resize(rangeMark);

                auto at = to.blockOffsets.getValue(b);
                assertTrue(at.isJust());
                to.blocks[at.unwrap()].startOffset = tail.start;
                to.blocks[at.unwrap()].endOffset = tail.start;

                merged = true;
                break;
            }
        }

        if(isTail && !merged) tails.push(ReturnTail { b, blockStart, contentEnd });

        // The shared epilogue, immediately behind the return that falls into it and outside every
        // block - it belongs to the function, and a block it were inside would be one whose extent
        // depended on where the epilogue went.
        if(emitter.epilogueNext) {
            extent.epilogue = U32(to.buffer.offset());
            genEpilogue(to, frame);
            to.buffer.writeByte(0xc3);

            if(onInst) ranges.push(EmittedRange { nullptr, &none, extent.epilogue, U32(to.buffer.offset()) });
        }
    }

    relaxBranches(to, emitter.branches, extent, ranges);

    for(auto& range: ranges) onInst(onInstCtx, range.inst, *range.regs, range.start, range.end);
}

void AsmModule::resolveRelocations(LowerGlobal* anchor) {
    // Looked up once: every table slot in the module is measured from the same label, which is the
    // whole reason a table built on a frame can hold the same encoding a constant one holds.
    auto anchorAt = anchor ? globalOffsets.getValue(anchor) : Maybe<U32>(Nothing());
    auto anchorOffset = anchorAt ? anchorAt.unwrap() : 0u;

    // The data sites first, since they only need the target's offset and every symbol is known by
    // now. What an *absolute* one cannot have yet is the load address, which applyDataRelocations
    // supplies; a self-relative one never needs it and is finished here.
    for(auto& r: pendingData) {
        U32 target;

        if(r.function) {
            auto o = functionOffsets.getValue(r.function);
            assertTrue(o.isJust());
            target = o.unwrap();
        } else {
            auto o = globalOffsets.getValue(r.global);
            assertTrue(o.isJust());
            target = o.unwrap();
        }

        /*
         * A table slot, written now and never again.
         *
         * Both offsets are inside this image, so their difference is the same number wherever the
         * image is later mapped - which is the whole reason a slot is anchor-relative rather than
         * absolute, and why the JIT and an ELF executable can hold identical bytes. See
         * repr/table.h.
         */
        if(r.anchorRelative) {
            // A slot without an anchor to measure from would be written as an offset from zero,
            // which is a wrong address rather than a missing one - so say so instead.
            assertTrue(anchor != nullptr);
            auto savedOffset = buffer.offset();

            buffer.offset(r.siteOffset);
            buffer.writeInt<LittleEndian>(U32(I32(target) - I32(anchorOffset)));
            buffer.offset(savedOffset);
            continue;
        }

        dataRelocations.push(AsmDataRelocation { .siteOffset = r.siteOffset, .targetOffset = target });
    }

    pendingData.clear();

    for(auto& r: relocations) {
        U32 target;

        // Only the two that cross a symbol boundary reach here: a jump within a function was
        // resolved by relaxBranches, which is where its size was decided (§7.1).
        if(r.function) {
            auto o = functionOffsets.getValue(r.function);
            assertTrue(o.isJust());
            target = o.unwrap();
        } else {
            auto o = globalOffsets.getValue(r.global);
            assertTrue(o.isJust()); // a referenced global was never emitted via addGlobal()
            target = o.unwrap();
        }

        auto rel = I32(target) - I32(r.siteOffset + 4);
        auto savedOffset = buffer.offset();

        buffer.offset(r.siteOffset);
        buffer.writeInt<LittleEndian>(U32(rel));
        buffer.offset(savedOffset);
    }
}
