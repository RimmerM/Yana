#include "gen.h"
#include "x64_util.h"

/*
 * Every operand register (or stack slot) referenced by an instruction is resolved ahead of time
 * by allocateRegisters() into an InstRegs record (see gen.h) - LowerValue itself has no `.reg`
 * field, since the same value may live in different registers at different points once moves are
 * inserted. genInst/genControl below always receive the current instruction's InstRegs alongside
 * the instruction itself, and index into `regs.uses`/`regs.creates` positionally, matching the
 * order of `inst->used()`/`inst->created()`.
 */

// The physical register number an encoder writes into an instruction. A frame slot never reaches
// one: a value living in the frame is brought into a scratch register by genMoves before anything
// reads it, and taken back afterwards if it was written.
static U8 reg(RegId id) {
    assertTrue(getRegClass(id) != StackReg); // an encoder was handed a frame slot
    return U8(getRegIndex(id));
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
 * Every instruction that touches memory ends the same way - a ModRM byte, then whatever SIB and
 * displacement bytes the addressing mode called for - so the mode is computed once into an
 * AddressMode and then written out.
 */

struct AddressMode {
    U8 mod;
    U8 rm;
    bool hasSib = false;
    U8 sib = 0;
    U32 disp = 0;
    bool hasDisp = false;
    bool disp32 = false;
    bool rexB = false;
    bool rexX = false;
};

static void writeAddress(AsmModule& to, bool is64, U8 regField, const AddressMode& a) {
    if(is64 || needsRex(regField) || a.rexB || a.rexX) {
        to.buffer.writeByte(makeRex(is64, a.rexB ? 8 : 0, regField, a.rexX ? 8 : 0));
    }
}

// Writes the ModRM byte, and the SIB/displacement bytes the addressing mode calls for, that
// follow an opcode operating on `a`. Every memory-operand instruction ends the same way.
static void writeAddressOperand(AsmModule& to, U8 regField, const AddressMode& a) {
    to.buffer.writeByte(makeMod(a.mod, a.rm, regField));
    if(a.hasSib) to.buffer.writeByte(a.sib);
    if(a.hasDisp) {
        if(a.disp32) to.buffer.writeInt<LittleEndian>(a.disp);
        else to.buffer.writeByte(U8(a.disp));
    }
}

// [base + displacement], where `base` is whichever register frame layout chose to hang the frame
// off. This is the only way anything addresses a frame object: the layout owns the arithmetic and
// the encoders only ever see the answer.
static AddressMode frameAddress(U8 baseReg, I32 displacement) {
    AddressMode a {};
    auto fitsIn8 = displacement >= -128 && displacement <= 127;

    // rbp-based addressing has no displacement-free form - mod=0 with rm=101 means RIP-relative -
    // so a zero displacement there still has to be written out as a zero byte.
    auto isBpLike = (baseReg & 7) == 5;
    auto needsDisp = displacement != 0 || isBpLike;

    a.mod = !needsDisp ? 0 : (fitsIn8 ? 1 : 2);
    a.hasDisp = needsDisp;
    a.disp = U32(displacement);
    a.disp32 = needsDisp && !fitsIn8;
    a.rexB = needsRex(baseReg);

    if((baseReg & 7) == 4) {
        // rsp/r12 can only be addressed through a SIB byte (rm=100 is reserved for it).
        a.hasSib = true;
        a.rm = 4;
        a.sib = (0 << 6) | (4 << 3) | (baseReg & 7); // no index, scale is irrelevant
    } else {
        a.rm = baseReg & 7;
    }

    return a;
}

// Emits (if needed) a REX prefix, the opcode, and a register-direct ModRM byte for `op rm, reg`
// (dest=rm field) or `op reg, rm` (dest=reg field) depending on which opcode variant is passed -
// the direction is entirely determined by the caller's choice of opcode.
static void genRegReg(AsmModule& to, LowerType type, U8 rm, U8 regField, U8 opCode, U8 prefix = 0) {
    if(is64Bit(type) || needsRex(rm) || needsRex(regField)) {
        to.buffer.writeByte(makeRex(is64Bit(type), rm, regField, 0));
    }

    if(prefix) to.buffer.writeByte(prefix);
    to.buffer.writeByte(opCode);
    to.buffer.writeByte(makeMod(3, rm, regField));
}

// Emits a single-operand instruction where ModRM.reg is a fixed opcode extension (not a real
// register) - e.g. NEG/NOT/INC/DEC r/m, or a shift by 1/by CL.
static void genReg(AsmModule& to, LowerType type, U8 rm, U8 opCode, U8 ext, U8 prefix = 0) {
    if(is64Bit(type) || needsRex(rm)) {
        to.buffer.writeByte(makeRex(is64Bit(type), rm, ext, 0));
    }

    if(prefix) to.buffer.writeByte(prefix);
    to.buffer.writeByte(opCode);
    to.buffer.writeByte(makeMod(3, rm, ext));
}

// Emits `op r/m, imm8` or `op r/m, imm32` where ModRM.reg is a fixed opcode extension (group 1
// style: ADD/SUB/AND/OR/XOR/CMP with an immediate, dest = rm = src).
static void genRegImm(AsmModule& to, LowerType type, U8 rm, LowerImm* imm, U8 opCode8, U8 opCode32, U8 ext) {
    if(is64Bit(type) || needsRex(rm)) {
        to.buffer.writeByte(makeRex(is64Bit(type), rm, ext, 0));
    }

    if(auto imm8 = encodeImm8((LowerValue*)imm)) {
        to.buffer.writeByte(opCode8);
        to.buffer.writeByte(makeMod(3, rm, ext));
        to.buffer.writeByte(imm8.unwrap());
    } else if(auto imm32 = encodeImm32((LowerValue*)imm)) {
        to.buffer.writeByte(opCode32);
        to.buffer.writeByte(makeMod(3, rm, ext));
        to.buffer.writeInt<LittleEndian>(imm32.unwrap());
    } else {
        assertTrue("invalid immediate value" == nullptr);
    }
}

static void genIncReg(AsmModule& to, LowerType type, U8 rm, bool sub) {
    if(is64Bit(type) || needsRex(rm)) {
        to.buffer.writeByte(makeRex(is64Bit(type), rm, 0, 0));
    }

    to.buffer.writeByte(0xff);
    to.buffer.writeByte(makeMod(3, rm, sub ? 1 : 0));
}

static void genZeroReg(AsmModule& to, U8 reg, LowerType type) {
    if(is64Bit(type) || needsRex(reg)) {
        to.buffer.writeByte(makeRex(is64Bit(type), reg, reg, 0));
    }

    to.buffer.writeByte(0x31);
    to.buffer.writeByte(makeMod(3, reg, reg));
}

static AddressMode slotAddress(const FrameLayout& frame, RegId slot) {
    assertTrue(isSlot(slot));
    return frameAddress(U8(getRegIndex(frame.base)), frame.slotOffset[getRegIndex(slot)]);
}

// An instruction reading one operand straight out of the frame instead of out of a register: the
// memory form of a two-operand encoding, with `regField` the other operand (or an opcode extension,
// for the group-3 forms that have no second register).
//
// This is the whole of what a direct memory operand costs the encoder. Which operand may be one -
// and whether the slot is the right width for the access - was settled by memoryUseOperand before
// allocation, so an encoder that reaches here has already been told this form exists.
static void genSlotOperand(AsmModule& to, const FrameLayout& frame, LowerType type, U8 regField, RegId slot, U8 opCode, U8 prefix = 0) {
    auto a = slotAddress(frame, slot);

    writeAddress(to, is64Bit(type), regField, a);
    if(prefix) to.buffer.writeByte(prefix);
    to.buffer.writeByte(opCode);
    writeAddressOperand(to, regField, a);
}

// Emits a sequenced permutation of locations (fixed-register constraints, phi placement, the copy
// that feeds a destructive two-address encoding, a value moving between the frame and a register).
// The allocator has already ordered these, so they are emitted exactly as given; a `swap` entry is
// one it could only satisfy with an exchange.
//
// A frame slot at either end makes the move a load or a store, and this is the only place either is
// produced: no encoder below ever sees a stack location, because a value that lives in the frame is
// brought into a register by one of these before anything reads it.
//
// A load or store is exactly as wide as the slot it touches. Slots are packed by width, so a 4-byte
// value sits 4 bytes from its neighbour and a 64-bit move would take the neighbour with it. A 32-bit
// write also zeroes the rest of the register, which is what a 32-bit value wants anyway.
static void genMoves(AsmModule& to, const FrameLayout& frame, const FrameObjects& objects, const Array<RegMove>& moves) {
    for(auto& m: moves) {
        if(m.from == m.to) continue;

        auto fromSlot = getRegClass(m.from) == StackReg;
        auto toSlot = getRegClass(m.to) == StackReg;

        // Only an exchange between two registers can be encoded without somewhere to put a third
        // value, and only registers ever end up in a cycle - see sequenceMoves.
        assertTrue(!m.swap || (!fromSlot && !toSlot));

        // A transfer with a slot at both ends is expanded into a load and a store by sequenceMoves,
        // which owns the register it goes through, so none ever reaches an encoder.
        assertTrue(!(fromSlot && toSlot));

        auto slotIs64 = [&](RegId slot) {
            return objects.slots[getRegIndex(slot)].size > 4;
        };

        if(fromSlot) {
            auto a = slotAddress(frame, m.from);       // MOV r32/r64, r/m
            writeAddress(to, slotIs64(m.from), reg(m.to), a);
            to.buffer.writeByte(0x8b);
            writeAddressOperand(to, reg(m.to), a);
        } else if(toSlot) {
            auto a = slotAddress(frame, m.to);         // MOV r/m, r32/r64
            writeAddress(to, slotIs64(m.to), reg(m.from), a);
            to.buffer.writeByte(0x89);
            writeAddressOperand(to, reg(m.from), a);
        } else {
            // XCHG r/m64, r64 (0x87) - breaks a copy cycle without needing a scratch register.
            genRegReg(to, LowerType::Int64, reg(m.from), reg(m.to), m.swap ? 0x87 : 0x8b);
        }
    }
}

// Materializes a constant into a register, picking the shortest encoding that reproduces `imm`
// exactly at `type`'s width.
static void genMovImmValue(AsmModule& to, LowerType type, U64 imm, RegId destReg) {
    auto is64 = is64Bit(type);
    auto dest = reg(destReg);

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

static void genMovImm(AsmModule& to, LowerImm& i, RegId destReg) {
    // No need to generate anything for implicit immediates - they're always embedded into
    // whatever instruction uses them instead.
    if(isImplicit(&i.result)) return;

    genMovImmValue(to, i.result.type, i.i, destReg);
}

// MOV r, r/m (0x8b) / MOVSXD r, r/m (0x63): non-destructive loads where dest can differ from src.
static void genMovReg(AsmModule& to, LowerType type, RegId dest, RegId src) {
    genRegReg(to, type, reg(src), reg(dest), 0x8b);
}

static void genMovRegS(AsmModule& to, LowerType type, RegId dest, RegId src) {
    genRegReg(to, type, reg(src), reg(dest), 0x63);
}

// 2-address ALU op: dest = rm field = lhs (== regs.creates[0], guaranteed by the allocator),
// src = reg field = rhs. `rmRegOp` must be the r/m-destination variant (e.g. 0x01 ADD r/m,r,
// not 0x03 ADD r,r/m) so the result lands in lhs's register.
//
// `regRmOp` is the other direction of the same operation, for an rhs left in the frame: the memory
// operand has to occupy the r/m field, so the register - which is also the destination - moves into
// the reg field. `type` is the width the operation works at, which is not always the result's: a
// comparison produces an Int32 whatever it compared.
static void genCommonBinary(AsmModule& to, LowerBase base, LowerInstBinary& i, const InstRegs& regs, const FrameLayout& frame, LowerType type, U8 rmRegOp, U8 regRmOp, U8 immExt) {
    auto lhs = base[i.lhs];
    auto rhs = base[i.rhs];

    if(isSlot(regs.uses[1])) {
        genSlotOperand(to, frame, type, reg(regs.uses[0]), regs.uses[1], regRmOp);
    } else if(isReg(lhs) && isReg(rhs)) {
        genRegReg(to, type, reg(regs.uses[0]), reg(regs.uses[1]), rmRegOp);
    } else if(isReg(lhs) && isImm(rhs)) {
        genRegImm(to, type, reg(regs.uses[0]), (LowerImm*)rhs->inst(), 0x83, 0x81, immExt);
    } else {
        assertTrue("unsupported operands to binary instruction" == nullptr);
    }
}

static void genNop(AsmModule& to, LowerInst& i) {
    to.buffer.writeByte(0x90);
}

static void genAdd(AsmModule& to, LowerBase base, LowerInstBinary& i, const InstRegs& regs, const FrameLayout& frame) {
    auto rhs = base[i.rhs];
    if(isReg(base[i.lhs]) && isImm(rhs)) {
        auto rhsImm = (LowerImm*)rhs->inst();
        if(rhsImm->i == 1) return genIncReg(to, i.result.type, reg(regs.uses[0]), false);
        if(rhsImm->i == (U64)I64(-1)) return genIncReg(to, i.result.type, reg(regs.uses[0]), true);
    }

    genCommonBinary(to, base, i, regs, frame, i.result.type, 0x01, 0x03, 0);
}

static void genSub(AsmModule& to, LowerBase base, LowerInstBinary& i, const InstRegs& regs, const FrameLayout& frame) {
    auto rhs = base[i.rhs];
    if(isReg(base[i.lhs]) && isImm(rhs)) {
        auto rhsImm = (LowerImm*)rhs->inst();
        if(rhsImm->i == 1) return genIncReg(to, i.result.type, reg(regs.uses[0]), true);
        if(rhsImm->i == (U64)I64(-1)) return genIncReg(to, i.result.type, reg(regs.uses[0]), false);
    }

    genCommonBinary(to, base, i, regs, frame, i.result.type, 0x29, 0x2b, 5);
}

static void genOr(AsmModule& to, LowerBase base, LowerInstBinary& i, const InstRegs& regs, const FrameLayout& frame) {
    genCommonBinary(to, base, i, regs, frame, i.result.type, 0x09, 0x0b, 1);
}

static void genXor(AsmModule& to, LowerBase base, LowerInstBinary& i, const InstRegs& regs, const FrameLayout& frame) {
    genCommonBinary(to, base, i, regs, frame, i.result.type, 0x31, 0x33, 6);
}

static void genAnd(AsmModule& to, LowerBase base, LowerInstBinary& i, const InstRegs& regs, const FrameLayout& frame) {
    genCommonBinary(to, base, i, regs, frame, i.result.type, 0x21, 0x23, 4);
}

static void genShift(AsmModule& to, LowerBase base, LowerInstBinary& i, const InstRegs& regs, U8 onceOp, U8 immOp, U8 regOp, U8 ext) {
    auto lhs = base[i.lhs];
    auto rhs = base[i.rhs];

    if(isReg(lhs) && isReg(rhs)) {
        assertTrue(reg(regs.uses[1]) == (U8)IntRegister::rcx);
        genReg(to, i.result.type, reg(regs.uses[0]), regOp, ext);
    } else if(isReg(lhs) && isImm(rhs)) {
        auto rhsImm = (LowerImm*)rhs->inst();
        assertTrue(rhsImm->i <= 0x7f);

        if(rhsImm->i == 1) {
            genReg(to, i.result.type, reg(regs.uses[0]), onceOp, ext);
        } else {
            genRegImm(to, i.result.type, reg(regs.uses[0]), rhsImm, immOp, immOp, ext);
        }
    } else {
        assertTrue("unsupported operands to shift instruction" == nullptr);
    }
}

static void genShift(AsmModule& to, LowerBase base, LowerInstBinary& i, const InstRegs& regs, U8 ext) {
    genShift(to, base, i, regs, 0xd1, 0xc1, 0xd3, ext);
}

// IMUL r, r/m (0x0faf): dest is the reg field and doubles as a source operand (2-address), so
// dest must be lhs's register (== regs.creates[0]); rhs is the rm-field source.
static void genIMul(AsmModule& to, LowerBase base, LowerInstBinary& i, const InstRegs& regs, const FrameLayout& frame) {
    assertTrue(isInt(i.result.type));
    auto lhs = base[i.lhs];
    auto rhs = base[i.rhs];

    if(isSlot(regs.uses[1])) {
        // Already the reg-destination direction, so a memory source needs nothing but the address.
        genSlotOperand(to, frame, i.result.type, reg(regs.uses[0]), regs.uses[1], 0xaf, 0x0f);
    } else if(isReg(lhs) && isReg(rhs)) {
        genRegReg(to, i.result.type, reg(regs.uses[1]), reg(regs.uses[0]), 0xaf, 0x0f);
    } else if(isReg(lhs) && isImm(rhs)) {
        // IMUL r, r/m, imm8/imm32 (0x6b/0x69) is a true 3-operand form: dest (reg field) can
        // differ from src (rm field) - dest is regs.creates[0], src is lhs (regs.uses[0]).
        auto destReg = reg(regs.creates[0]);
        auto srcReg = reg(regs.uses[0]);
        auto type = i.result.type;

        if(is64Bit(type) || needsRex(destReg) || needsRex(srcReg)) {
            to.buffer.writeByte(makeRex(is64Bit(type), srcReg, destReg, 0));
        }

        if(auto imm8 = encodeImm8(rhs)) {
            to.buffer.writeByte(0x6b);
            to.buffer.writeByte(makeMod(3, srcReg, destReg));
            to.buffer.writeByte(imm8.unwrap());
        } else if(auto imm32 = encodeImm32(rhs)) {
            to.buffer.writeByte(0x69);
            to.buffer.writeByte(makeMod(3, srcReg, destReg));
            to.buffer.writeInt<LittleEndian>(imm32.unwrap());
        } else {
            assertTrue("invalid immediate value" == nullptr);
        }
    } else {
        assertTrue("unsupported operands to mul instruction" == nullptr);
    }
}

// The second operand of Mul/Div/IDiv, which the group-3 encoding (0xf7) takes as r/m with the
// ModRM.reg field carrying an opcode extension rather than a register - so a divisor or
// multiplicand can come straight out of the frame with no reload at all.
//
// The first operand needs no handling of its own: InstConstraints forces it into rax (and div/idiv
// clobber rdx) - see constraint.cpp - so it has already been placed there by the time this runs.
static void genGroup3(AsmModule& to, LowerType type, const InstRegs& regs, const FrameLayout& frame, U8 ext) {
    if(isSlot(regs.uses[1])) {
        genSlotOperand(to, frame, type, ext, regs.uses[1], 0xf7);
        return;
    }

    auto rhsReg = reg(regs.uses[1]);
    if(is64Bit(type) || needsRex(rhsReg)) {
        to.buffer.writeByte(makeRex(is64Bit(type), rhsReg, 0, 0));
    }

    to.buffer.writeByte(0xf7);
    to.buffer.writeByte(makeMod(3, rhsReg, ext));
}

static void genMul(AsmModule& to, LowerBase base, LowerInstBinary& i, const InstRegs& regs, const FrameLayout& frame) {
    assertTrue(reg(regs.uses[0]) == (U8)IntRegister::rax);
    genGroup3(to, i.result.type, regs, frame, 4);
}

static void genCqo(AsmModule& to, LowerType type) {
    if(is64Bit(type)) {
        to.buffer.writeByte(makeRex(true, 0, 0, 0));
    }

    to.buffer.writeByte(0x99);
}

static void genDiv(AsmModule& to, LowerBase base, LowerInstBinary& i, const InstRegs& regs, const FrameLayout& frame) {
    // div uses both rax and rdx, so we need to zero rdx first.
    // Since the division overwrites the flags anyway, it is safe to use xor here.
    assertTrue(isInt(i.result.type));
    assertTrue(reg(regs.uses[0]) == (U8)IntRegister::rax);
    genZeroReg(to, (U8)IntRegister::rdx, i.result.type);

    genGroup3(to, i.result.type, regs, frame, 6);
}

static void genIDiv(AsmModule& to, LowerBase base, LowerInstBinary& i, const InstRegs& regs, const FrameLayout& frame) {
    // idiv uses both rax and rdx, so we need to sign-extend rax into rdx first.
    assertTrue(isInt(i.result.type));
    assertTrue(reg(regs.uses[0]) == (U8)IntRegister::rax);
    genCqo(to, i.result.type);

    genGroup3(to, i.result.type, regs, frame, 7);
}

static void genNeg(AsmModule& to, LowerInstUnary& i, const InstRegs& regs) {
    genReg(to, i.result.type, reg(regs.uses[0]), 0xf7, 3);
}

static void genNot(AsmModule& to, LowerBase base, LowerInstUnary& i, const InstRegs& regs) {
    assertTrue(isInt(base[i.from]->type));
    genReg(to, i.result.type, reg(regs.uses[0]), 0xf7, 2);
}

static void genCopy(AsmModule& to, LowerBase base, LowerInstCopy& i, const InstRegs& regs) {
    // Which encoding this is was decided by transformFunction, and the register constraints were
    // derived from the same flag - re-deriving it here is what would let the two disagree.
    if(i.isUnrolled()) {
        auto n = ((LowerImm*)base[i.count]->inst())->i;

        auto toReg = reg(regs.uses[0]);
        auto fromReg = reg(regs.uses[1]);
        // r11 is reserved as a scratch register for this encoding - see movsb's `clobber` mask
        // in constraint.cpp, which guarantees no live value occupies it at this instruction.
        U8 scratch = (U8)IntRegister::r11;
        U64 offset = 0;

        while(offset < n) {
            auto remaining = n - offset;
            U8 width = remaining >= 8 ? 8 : remaining >= 4 ? 4 : remaining >= 2 ? 2 : 1;

            if(width == 2) to.buffer.writeByte(0x66);
            if(width == 8 || needsRex(fromReg) || needsRex(scratch)) {
                to.buffer.writeByte(makeRex(width == 8, fromReg, scratch, 0));
            }
            to.buffer.writeByte(width == 1 ? 0x8a : 0x8b);
            if(offset == 0) {
                to.buffer.writeByte(makeMod(0, fromReg, scratch));
            } else {
                to.buffer.writeByte(makeMod(1, fromReg, scratch));
                to.buffer.writeByte(U8(offset));
            }

            if(width == 2) to.buffer.writeByte(0x66);
            if(width == 8 || needsRex(toReg) || needsRex(scratch)) {
                to.buffer.writeByte(makeRex(width == 8, toReg, scratch, 0));
            }
            to.buffer.writeByte(width == 1 ? 0x88 : 0x89);
            if(offset == 0) {
                to.buffer.writeByte(makeMod(0, toReg, scratch));
            } else {
                to.buffer.writeByte(makeMod(1, toReg, scratch));
                to.buffer.writeByte(U8(offset));
            }

            offset += width;
        }
    } else {
        assertTrue(reg(regs.uses[0]) == (U8)IntRegister::rdi && reg(regs.uses[1]) == (U8)IntRegister::rsi && reg(regs.uses[2]) == (U8)IntRegister::rcx);
        to.buffer.writeByte(0xf3);
        to.buffer.writeByte(0xa4);
    }
}

static void genSetPattern(AsmModule& to, LowerBase base, LowerInstSetPattern& i, const InstRegs& regs) {
    if(i.isUnrolled()) {
        auto n = ((LowerImm*)base[i.count]->inst())->i;

        auto toReg = reg(regs.uses[0]);
        auto patReg = reg(regs.uses[2]);
        U64 offset = 0;

        while(offset < n) {
            auto remaining = n - offset;
            U8 width = remaining >= 8 ? 8 : remaining >= 4 ? 4 : remaining >= 2 ? 2 : 1;

            if(width == 2) to.buffer.writeByte(0x66);
            if(width == 8 || needsRex(patReg) || needsRex(toReg)) {
                to.buffer.writeByte(makeRex(width == 8, toReg, patReg, 0));
            }
            to.buffer.writeByte(width == 1 ? 0x88 : 0x89);
            if(offset == 0) {
                to.buffer.writeByte(makeMod(0, toReg, patReg));
            } else {
                to.buffer.writeByte(makeMod(1, toReg, patReg));
                to.buffer.writeByte(U8(offset));
            }

            offset += width;
        }
    } else {
        assertTrue(reg(regs.uses[0]) == (U8)IntRegister::rdi && reg(regs.uses[1]) == (U8)IntRegister::rcx && reg(regs.uses[2]) == (U8)IntRegister::rax);
        to.buffer.writeByte(0xf3);
        to.buffer.writeByte(0xaa);
    }
}

static void genBswap(AsmModule& to, LowerInstUnary& i, const InstRegs& regs) {
    auto r = reg(regs.uses[0]);
    auto is64 = is64Bit(i.result.type);

    if(is64 || needsRex(r)) {
        to.buffer.writeByte(makeRex(is64, r, 0, 0));
    }

    to.buffer.writeByte(0x0f);
    to.buffer.writeByte(0xc8 + (r & 7));
}

static void genRet(AsmModule& to) {
    to.buffer.writeByte(0xc3);
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

// `op r/m64, imm` for the group-1 opcodes, by ModRM.reg extension: 0 add, 4 and, 5 sub. Unlike
// genRegImm this takes a plain integer rather than a LowerImm, because the values here come from the
// frame layout rather than from the program.
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
    auto a = frameAddress(baseReg, displacement);
    writeAddress(to, true, dest, a);
    to.buffer.writeByte(0x8d);
    writeAddressOperand(to, dest, a);
}

// Establishes the frame the layout decided on: the caller's rbp is saved and rbp made to point at
// it (so a stack walk can follow the chain), then the callee-saved registers this function actually
// overwrites, then room for the locals and spill slots.
//
// Every part of this is skipped when the layout says it is not needed, so a leaf function that kept
// everything in caller-saved registers still starts at its first real instruction.
static void genPrologue(AsmModule& to, const FrameLayout& frame) {
    auto rbp = U8(IntRegister::rbp);
    auto rsp = U8(IntRegister::rsp);

    if(frame.framePointer) {
        genPushReg(to, rbp);
        genRegReg(to, LowerType::Int64, rsp, rbp, 0x8b); // mov rbp, rsp (rm = source, reg = dest)
    }

    for(Size i = 0; i < kRegCount; i++) {
        if(frame.savedRegs.has(makeRegId(GenReg, U16(i)))) genPushReg(to, U8(i));
    }

    genAddImm(to, rsp, I32(frame.fixedSize), true);
}

static void genEpilogue(AsmModule& to, const FrameLayout& frame) {
    auto rbp = U8(IntRegister::rbp);
    auto rsp = U8(IntRegister::rsp);

    U32 savedCount = 0;
    for(Size i = 0; i < kRegCount; i++) {
        if(frame.savedRegs.has(makeRegId(GenReg, U16(i)))) savedCount++;
    }

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

    for(Size i = kRegCount; i > 0; i--) {
        if(frame.savedRegs.has(makeRegId(GenReg, U16(i - 1)))) genPopReg(to, U8(i - 1));
    }

    if(frame.framePointer) genPopReg(to, rbp);
}

static void genPushValue(AsmModule& to, LowerValue* value, RegId valueReg) {
    if(isImm(value)) {
        if(auto imm8 = encodeImm8(value)) {
            to.buffer.writeByte(0x6a);
            to.buffer.writeByte(imm8.unwrap());
        } else if(auto imm32 = encodeImm32(value)) {
            to.buffer.writeByte(0x68);
            to.buffer.writeInt<LittleEndian>(imm32.unwrap());
        } else {
            assertTrue("invalid immediate value" == nullptr);
        }
    } else if(isReg(value)) {
        genPushReg(to, reg(valueReg));
    } else {
        assertTrue("unsupported operand to push instruction" == nullptr);
    }
}

/*
 * Outgoing stack arguments.
 *
 * An argument that did not fit in a register is written into the outgoing argument area, at the
 * offset its calling convention assigned it - which is the same offset the callee reads it back
 * from, because both sides asked classifyArgs.
 *
 * The area is the lowest part of the frame and is reserved once by the prologue, so it is addressed
 * through rsp and needs no set-up or tear-down around the call. Arguments are *written* into it
 * rather than pushed, because pushing would put the first argument at the highest address - the
 * reverse of what the callee's offsets mean.
 */
static void genPushArg(AsmModule& to, LowerBase base, LowerInstX86PushArg& i, const InstRegs& regs) {
    auto a = frameAddress(U8(IntRegister::rsp), I32(i.stackOffset));
    auto value = base[i.arg];

    if(isImm(value)) {
        // MOV r/m64, imm32 (REX.W c7 /0) - sign-extended, which is what a narrower argument
        // occupying a full 8-byte slot wants anyway.
        auto imm = encodeImm32(value);
        assertTrue(imm.isJust()); // a wider constant has to be materialized into a register first

        writeAddress(to, true, 0, a);
        to.buffer.writeByte(0xc7);
        writeAddressOperand(to, 0, a);
        to.buffer.writeInt<LittleEndian>(imm.unwrap());
    } else if(isReg(value)) {
        auto src = reg(regs.uses[0]);   // MOV r/m64, r64
        writeAddress(to, true, src, a);
        to.buffer.writeByte(0x89);
        writeAddressOperand(to, src, a);
    } else {
        assertTrue("unsupported operand to a stack-passed call argument" == nullptr);
    }
}

// Pushes a callee-saved register's contents (used by prologue/epilogue code, none of which is
// generated yet - see the "not wired into a caller" note on X86Push/X86Pop in lower_inst.h).
static void genPush(AsmModule& to, LowerBase base, LowerInstUnary& i, const InstRegs& regs) {
    genPushValue(to, base[i.from], regs.uses[0]);
}

static void genPop(AsmModule& to, RegId destReg) {
    genPopReg(to, reg(destReg));
}

// CMP r/m, reg (0x39): computes rm - reg = lhs - rhs and discards the result, only setting
// flags. This matches getCompareOp()'s condition codes, which assume flags = lhs - rhs.
// A comparison works at the width of the values compared, not at the width of what it produces: its
// result is an Int32 whatever went into it, so taking the width from the result would compare two
// 64-bit values as 32-bit ones.
static void genCmpToFlags(AsmModule& to, LowerBase base, LowerInstCmp& i, const InstRegs& regs, const FrameLayout& frame) {
    genCommonBinary(to, base, i, regs, frame, base[i.lhs]->type, 0x39, 0x3b, 7);
}

static LowerCmp negateCmp(LowerCmp cmp) {
    switch(cmp) {
        case LowerCmp::eq:
            return LowerCmp::neq;
        case LowerCmp::neq:
            return LowerCmp::eq;
        case LowerCmp::gt:
            return LowerCmp::le;
        case LowerCmp::ge:
            return LowerCmp::lt;
        case LowerCmp::lt:
            return LowerCmp::ge;
        case LowerCmp::le:
            return LowerCmp::gt;
        case LowerCmp::igt:
            return LowerCmp::ile;
        case LowerCmp::ige:
            return LowerCmp::ilt;
        case LowerCmp::ilt:
            return LowerCmp::ige;
        case LowerCmp::ile:
            return LowerCmp::igt;
    }

    assertTrue(false);
    return cmp;
}

static U8 getCompareOp(LowerCmp cmp) {
    switch(cmp) {
        case LowerCmp::eq:
            return 0x94;
        case LowerCmp::neq:
            return 0x95;
        case LowerCmp::gt:
            return 0x97;
        case LowerCmp::ge:
            return 0x93;
        case LowerCmp::lt:
            return 0x92;
        case LowerCmp::le:
            return 0x96;
        case LowerCmp::igt:
            return 0x9f;
        case LowerCmp::ige:
            return 0x9d;
        case LowerCmp::ilt:
            return 0x9c;
        case LowerCmp::ile:
            return 0x9e;
    }

    assertTrue(false);
    return 0;
}

// Jcc near (0x0f 0x8x) uses the same condition-code nibble as SETcc (0x0f 0x9x), offset by 0x10.
static U8 getJumpOp(LowerCmp cmp) {
    return getCompareOp(cmp) - 0x10;
}

// Materializes the flags into a real register, for a comparison whose result couldn't stay in the
// flags (see tryMergeCompare in transform.cpp). SETcc r/m8 (0f 90+cc /0) writes 1 or 0 into the
// register's low byte and leaves the other three untouched, so it is followed by MOVZX r32, r/m8
// (0f b6 /r) to clear them - the result is an ordinary 0-or-1 Int that later instructions can read
// in full.
static void genFlagsToReg(AsmModule& to, U8 reg, LowerCmp cmp) {
    // Encoding 4-7 as an 8-bit operand names ah/ch/dh/bh unless some REX prefix is present, which
    // switches them to spl/bpl/sil/dil - the registers the allocator's numbering actually means.
    auto byteRex = needsRex(reg) || (reg & 7) >= 4;

    if(byteRex) to.buffer.writeByte(makeRex(false, reg, 0, 0));
    to.buffer.writeByte(0x0f);
    to.buffer.writeByte(getCompareOp(cmp));
    to.buffer.writeByte(makeMod(3, reg, 0));

    if(byteRex) to.buffer.writeByte(makeRex(false, reg, reg, 0));
    to.buffer.writeByte(0x0f);
    to.buffer.writeByte(0xb6);
    to.buffer.writeByte(makeMod(3, reg, reg));
}

static void genCmp(AsmModule& to, LowerBase base, LowerInstCmp& i, const InstRegs& regs, const FrameLayout& frame) {
    auto type = base[i.lhs]->type;

    if(isInt(type) || isPtr(type)) {
        genCmpToFlags(to, base, i, regs, frame);
        if(!isImplicit(&i.result)) genFlagsToReg(to, reg(regs.creates[0]), i.getCmp());
    } else if(isFloat(type)) {
        // TODO: float comparisons are not implemented (out of scope for the int/pointer MVP).
        assertTrue("float comparisons are not implemented" == nullptr);
    } else {
        assertTrue("invalid comparison operands" == nullptr);
    }
}

static void genTestReg(AsmModule& to, LowerType type, U8 a, U8 b) {
    genRegReg(to, type, a, b, 0x85);
}

static U8 getSelectOp(LowerCmp cmp) {
    switch(cmp) {
        case LowerCmp::eq:
            return 0x44;
        case LowerCmp::neq:
            return 0x45;
        case LowerCmp::gt:
            return 0x47;
        case LowerCmp::ge:
            return 0x43;
        case LowerCmp::lt:
            return 0x42;
        case LowerCmp::le:
            return 0x46;
        case LowerCmp::igt:
            return 0x4f;
        case LowerCmp::ige:
            return 0x4d;
        case LowerCmp::ilt:
            return 0x4c;
        case LowerCmp::ile:
            return 0x4e;
    }

    assertTrue(false);
    return 0;
}

// CMOVcc r, r/m (0x0f4x): dest is the reg field and doubles as a source (if the condition is
// false, dest keeps its own value) - so dest must be lhs's register (== regs.creates[0]).
static void genSelect(AsmModule& to, LowerBase base, LowerInstSelect& i, const InstRegs& regs) {
    LowerCmp cmp;

    if(auto embedded = i.getEmbeddedCmp()) {
        // Flags are already set correctly by a preceding (implicit) Cmp.
        cmp = embedded.unwrap();
    } else if(isReg(base[i.cmp])) {
        // TEST r, r sets ZF exactly when the condition register is zero, so "condition holds" is
        // the *not-equal* case - matching how genControl tests a Je's non-embedded condition.
        // Using eq here would invert the select relative to the embedded-flags path above.
        genTestReg(to, LowerType::Int32, reg(regs.uses[2]), reg(regs.uses[2]));
        cmp = LowerCmp::neq;
    } else {
        assertTrue("unsupported operands to select instruction" == nullptr);
        return;
    }

    if(!isReg(base[i.lhs]) || !isReg(base[i.rhs])) {
        assertTrue("unsupported select operands" == nullptr);
    }

    // `select` yields lhs when the condition holds and rhs otherwise. The destructive-2-address
    // rule (see isDestructiveBinaryDest in register.cpp) has already placed lhs in the result
    // register, so the CMOV is the rhs case and therefore runs on the *negated* condition.
    assertTrue(reg(regs.creates[0]) == reg(regs.uses[0]));
    genRegReg(to, i.result.type, reg(regs.uses[1]), reg(regs.creates[0]), getSelectOp(negateCmp(cmp)), 0x0f);
}

/*
 * Memory addressing.
 */

static AddressMode genDirectAddress(RegId baseReg) {
    AddressMode a {};
    auto r = reg(baseReg);
    auto isBpLike = (r & 7) == 5;

    a.mod = isBpLike ? 1 : 0;
    a.hasDisp = isBpLike;
    a.disp = 0;
    a.disp32 = false;
    a.rexB = needsRex(r);

    if((r & 7) == 4) {
        // rsp/r12 can only be addressed through a SIB byte (rm=100 is reserved for it).
        a.hasSib = true;
        a.rm = 4;
        a.sib = (0 << 6) | (4 << 3) | (r & 7); // no index, scale is irrelevant
    } else {
        a.rm = r & 7;
    }

    return a;
}

static AddressMode genComputedAddress(LowerInstX86Address& addr, const InstRegs& addrRegs) {
    assertTrue(addr.base != nullptr); // index-only addressing (no base) is not supported

    AddressMode a {};
    auto baseReg = reg(addrRegs.uses[0]);
    auto hasIndex = addr.index != nullptr;
    auto isBpLike = (baseReg & 7) == 5;
    auto needDisp = addr.displacement != 0 || isBpLike;
    auto dispFits8 = addr.displacement <= 0x7f || addr.displacement >= 0xffffff80;

    a.mod = !needDisp ? 0 : (dispFits8 ? 1 : 2);
    a.hasDisp = needDisp;
    a.disp = addr.displacement;
    a.disp32 = needDisp && !dispFits8;
    a.rexB = needsRex(baseReg);

    if(hasIndex || (baseReg & 7) == 4) {
        // rsp cannot be used as an index: 100 in the SIB index field means "no index". r12 encodes
        // the same three bits and *is* a legal index, because REX.X tells the two apart.
        assertTrue(!hasIndex || reg(addrRegs.uses[1]) != U8(IntRegister::rsp));

        a.hasSib = true;
        a.rm = 4;

        U8 scale = addr.scale == 8 ? 3 : addr.scale == 4 ? 2 : addr.scale == 2 ? 1 : 0;
        U8 sibIndex = hasIndex ? (reg(addrRegs.uses[1]) & 7) : 4;
        a.sib = (scale << 6) | (sibIndex << 3) | (baseReg & 7);
        a.rexX = hasIndex && needsRex(reg(addrRegs.uses[1]));
    } else {
        a.rm = baseReg & 7;
    }

    return a;
}

// Resolves the address of a memory operand: either a plain register holding a pointer (the
// common case), or a X86Address instruction's base+index*scale+displacement computation
// (looked up by instruction identity in `addrRegs`, since it was resolved at its own position
// earlier in the same block).
static AddressMode resolveAddress(LowerBase base, LowerValue* addrValue, RegId directReg, const HashMap<LowerInst*, const InstRegs*>& addrRegs) {
    if(isMem(addrValue)) {
        auto addrInst = (LowerInstX86Address*)addrValue->inst();
        auto found = addrRegs.getValue(addrInst);
        assertTrue(found.isJust());
        return genComputedAddress(*addrInst, *found.unwrap());
    } else {
        return genDirectAddress(directReg);
    }
}

static void genLoad(AsmModule& to, LowerBase base, LowerInstLoad& i, const InstRegs& regs, const HashMap<LowerInst*, const InstRegs*>& addrRegs) {
    auto from = base[i.from];
    auto a = resolveAddress(base, from, regs.uses[0], addrRegs);
    auto dest = reg(regs.creates[0]);
    auto width = i.getWidth();

    // Narrow loads must extend into the full destination register rather than merging with
    // whatever it happened to hold, so they use MOVZX/MOVSX (0f b6/b7 and 0f be/bf) instead of a
    // plain MOV. A signed 4-byte load into a 64-bit result is the same situation one step up, and
    // is what MOVSXD (0x63) exists for.
    auto is64 = is64Bit(i.result.type);

    if(width == 1 || width == 2) {
        writeAddress(to, is64, dest, a);
        to.buffer.writeByte(0x0f);
        to.buffer.writeByte(i.isSigned() ? (width == 1 ? 0xbe : 0xbf) : (width == 1 ? 0xb6 : 0xb7));
    } else if(width == 4 && i.isSigned() && is64) {
        writeAddress(to, true, dest, a);
        to.buffer.writeByte(0x63);
    } else {
        // A 4-byte MOV into a 32-bit register zeroes the upper half on its own, so an unsigned
        // narrowing load needs no extra work.
        writeAddress(to, width >= 8, dest, a);
        to.buffer.writeByte(0x8b);
    }

    writeAddressOperand(to, dest, a);
}

static void genStore(AsmModule& to, LowerBase base, LowerInstStore& i, const InstRegs& regs, const HashMap<LowerInst*, const InstRegs*>& addrRegs) {
    auto to_ = base[i.to];
    auto value = base[i.value];
    assertTrue(isReg(value)); // storing an immediate directly is not supported yet

    auto a = resolveAddress(base, to_, regs.uses[0], addrRegs);
    auto src = reg(regs.uses[1]);
    auto width = i.getWidth();

    // A store only writes `width` bytes, so unlike a load it just needs the right operand size:
    // MOV r/m8 (0x88) for one byte, the 0x66 operand-size prefix for two, REX.W for eight.
    if(width == 2) to.buffer.writeByte(0x66);

    if(width == 1) {
        // Without a REX prefix, encoding 4-7 in an 8-bit operand means ah/ch/dh/bh rather than
        // spl/bpl/sil/dil. An empty REX selects the latter, which is what the register allocator's
        // numbering means.
        if(needsRex(src) || a.rexB || a.rexX || (src & 7) >= 4) {
            to.buffer.writeByte(makeRex(false, a.rexB ? 8 : 0, src, a.rexX ? 8 : 0));
        }
        to.buffer.writeByte(0x88);
    } else {
        writeAddress(to, width >= 8, src, a);
        to.buffer.writeByte(0x89);
    }

    writeAddressOperand(to, src, a);
}

// A stack allocation is one of two quite different things depending on whether its size is known.
//
// A compile-time size was turned into a frame object by the register allocator, so the frame is
// already the right size and all that is left is to take the object's address - one `lea`, and no
// change to the stack pointer at all.
//
// A size only known at run time has to move the stack pointer, which is why such a function is
// required to have a frame pointer (see frame.cpp): everything else in the frame is addressed
// through rbp and so does not care where rsp has ended up. The result register doubles as the
// scratch for rounding the size up, so the count operand survives for whatever else reads it.
static void genAlloca(AsmModule& to, LowerBase base, LowerInstAlloca& i, const InstRegs& regs, const FrameLayout& frame, const FrameObjects& objects) {
    auto dest = reg(regs.creates[0]);
    auto rsp = U8(IntRegister::rsp);

    if(auto ref = objects.references.getValue(&i)) {
        genLeaFrame(to, dest, U8(getRegIndex(frame.base)), frame.offsetOf(ref.unwrap()));
        return;
    }

    assertTrue(frame.framePointer); // guaranteed by FrameObjects::hasDynamicAlloca

    // The allocation has to keep the stack pointer on the boundary it was already on, so the size
    // is rounded up before it is subtracted rather than the result being masked afterwards -
    // masking would also undo any alignment the frame had established.
    auto alignment = I32(frame.dynamicAlignment);

    if(dest != reg(regs.uses[0])) genMovReg(to, LowerType::Pointer, regs.creates[0], regs.uses[0]);

    genAddImm(to, dest, alignment - 1, false);
    genAndImm(to, dest, -alignment);
    genRegReg(to, LowerType::Int64, rsp, dest, 0x29);  // sub rsp, dest

    // The allocation sits above the outgoing argument area, which stays at the bottom of the stack
    // so that the next call still finds its arguments where the callee looks for them. A function
    // that passes nothing on the stack has no area to step over, and the address is just rsp.
    if(frame.argAreaSize > 0) {
        genLeaFrame(to, dest, rsp, I32(frame.argAreaSize));
    } else {
        genMovReg(to, LowerType::Pointer, regs.creates[0], makeRegId(GenReg, rsp));
    }
}

// LEA reg, [address] (0x8d): materializes a computed address into a register without dereferencing it.
static void genLea(AsmModule& to, LowerBase base, LowerInstX86Address& i, const InstRegs& regs) {
    auto dest = reg(regs.creates[0]);
    auto a = genComputedAddress(i, regs);

    writeAddress(to, true, dest, a);
    to.buffer.writeByte(0x8d);
    writeAddressOperand(to, dest, a);
}

static void genCast(AsmModule& to, LowerBase base, LowerInstCast& i, const InstRegs& regs) {
    auto source = base[i.from]->type;
    auto target = i.result.type;

    if(isIntLike(source) && isIntLike(target)) {
        // An embedded immediate source has no register for regs.uses[0] to point at (see
        // isImplicit) - the cast is just a constant materialization, already narrowed/widened to
        // the target width by genMovImmValue's choice of encoding.
        if(isImm(base[i.from])) {
            genMovImmValue(to, target, ((LowerImm*)base[i.from]->inst())->i, regs.creates[0]);
        } else if(i.isSignedResult() && i.isSignedSource()) {
            genMovRegS(to, i.result.type, regs.creates[0], regs.uses[0]);
        } else {
            // Move at the narrower of the two widths. A 32-bit MOV always clears the upper half of
            // its destination, so the same encoding both truncates a 64-bit source and zero-extends
            // into a 64-bit destination - which is exactly what an unsigned cast means in either
            // direction. Using the result width instead would copy the source's upper half
            // unchanged when widening, propagating whatever garbage it held. The move is emitted
            // even when source and destination are the same register, since that clearing is the
            // entire point.
            auto width = is64Bit(source) && is64Bit(target) ? target : LowerType::Int32;
            genMovReg(to, width, regs.creates[0], regs.uses[0]);
        }
    } else if(isIntLike(source) && isFloat(target)) {
        assertTrue("int-to-float casts are not implemented" == nullptr);
    } else if(isFloat(source) && isIntLike(target)) {
        assertTrue("float-to-int casts are not implemented" == nullptr);
    } else {
        assertTrue("invalid cast operands" == nullptr);
    }
}

static void genBitcast(AsmModule& to, LowerBase base, LowerInstUnary& i, const InstRegs& regs) {
    auto source = base[i.from]->type;
    auto target = i.result.type;

    if(isIntLike(source) && isIntLike(target)) {
        // Same as genCast: an embedded immediate has no source register to move from.
        if(isImm(base[i.from])) {
            genMovImmValue(to, target, ((LowerImm*)base[i.from]->inst())->i, regs.creates[0]);
        } else if(regs.uses[0] != regs.creates[0]) {
            // If the result is in a different register, move it. Otherwise, there is nothing to do.
            genMovReg(to, i.result.type, regs.creates[0], regs.uses[0]);
        }
    } else if(isIntLike(source) && isFloat(target)) {
        assertTrue("int-to-float bitcasts are not implemented" == nullptr);
    } else if(isFloat(source) && isIntLike(target)) {
        assertTrue("float-to-int bitcasts are not implemented" == nullptr);
    } else {
        assertTrue("invalid cast operands" == nullptr);
    }
}

// RIP-relative LEA (0x8d, mod=00 rm=101) + a relocation against the target's eventual offset.
static void genLoadAddress(AsmModule& to, RegId destReg, LowerGlobal* global, LowerFunction* function) {
    auto dest = reg(destReg);

    to.buffer.writeByte(makeRex(true, 0, dest, 0));
    to.buffer.writeByte(0x8d);
    to.buffer.writeByte(makeMod(0, 5, dest));

    if(function) to.addRelocation(function);
    else to.addRelocation(global);
}

static void genInst(AsmModule& to, LowerBase base, LowerInst* inst, const InstRegs& regs, HashMap<LowerInst*, const InstRegs*>& addrRegs, const FrameLayout& frame, const FrameObjects& objects) {
    switch(inst->kind) {
        case LowerInst::Arg:
            // No code generation needed - argument registers are already in place on entry.
            break;
        case LowerInst::Global:
            genLoadAddress(to, regs.creates[0], base[((LowerInstGlobal*)inst)->target], nullptr);
            break;
        case LowerInst::Fun:
            // Elided when every use is a direct call, which encodes the target as a rel32 and never
            // reads this register - see tryElideDirectCallee.
            if(!isImplicit(&((LowerInstFun*)inst)->result)) {
                genLoadAddress(to, regs.creates[0], nullptr, base[((LowerInstFun*)inst)->target]);
            }
            break;
        case LowerInst::Imm:
            genMovImm(to, *(LowerImm*)inst, regs.creates[0]);
            break;

        case LowerInst::Nop:
            genNop(to, *inst);
            break;
        case LowerInst::Cast:
            genCast(to, base, *(LowerInstCast*)inst, regs);
            break;
        case LowerInst::Bitcast:
            genBitcast(to, base, *(LowerInstUnary*)inst, regs);
            break;
        case LowerInst::Set: {
            // MOV r, r/m either way: a source still in the frame is read in place rather than
            // reloaded into a register the copy would then read again.
            auto type = ((LowerInstUnary*)inst)->result.type;

            if(isSlot(regs.uses[0])) {
                genSlotOperand(to, frame, type, reg(regs.creates[0]), regs.uses[0], 0x8b);
            } else {
                genMovReg(to, type, regs.creates[0], regs.uses[0]);
            }
            break;
        }
        case LowerInst::Neg:
            genNeg(to, *(LowerInstUnary*)inst, regs);
            break;
        case LowerInst::Not:
            genNot(to, base, *(LowerInstUnary*)inst, regs);
            break;

        case LowerInst::Add:
            genAdd(to, base, *(LowerInstBinary*)inst, regs, frame);
            break;
        case LowerInst::Sub:
            genSub(to, base, *(LowerInstBinary*)inst, regs, frame);
            break;
        case LowerInst::Mul:
            genMul(to, base, *(LowerInstBinary*)inst, regs, frame);
            break;
        case LowerInst::IMul:
            genIMul(to, base, *(LowerInstBinary*)inst, regs, frame);
            break;
        case LowerInst::Div:
            genDiv(to, base, *(LowerInstBinary*)inst, regs, frame);
            break;
        case LowerInst::IDiv:
            genIDiv(to, base, *(LowerInstBinary*)inst, regs, frame);
            break;
        case LowerInst::Rem:
            genDiv(to, base, *(LowerInstBinary*)inst, regs, frame);
            break;
        case LowerInst::IRem:
            genIDiv(to, base, *(LowerInstBinary*)inst, regs, frame);
            break;

        case LowerInst::Shl:
            genShift(to, base, *(LowerInstBinary*)inst, regs, 4);
            break;
        case LowerInst::Shr:
            genShift(to, base, *(LowerInstBinary*)inst, regs, 5);
            break;
        case LowerInst::Sar:
            genShift(to, base, *(LowerInstBinary*)inst, regs, 7);
            break;
        case LowerInst::And:
            genAnd(to, base, *(LowerInstBinary*)inst, regs, frame);
            break;
        case LowerInst::Or:
            genOr(to, base, *(LowerInstBinary*)inst, regs, frame);
            break;
        case LowerInst::Xor:
            genXor(to, base, *(LowerInstBinary*)inst, regs, frame);
            break;

        case LowerInst::Cmp:
            genCmp(to, base, *(LowerInstCmp*)inst, regs, frame);
            break;
        case LowerInst::Select:
            genSelect(to, base, *(LowerInstSelect*)inst, regs);
            break;
        case LowerInst::Alloca:
            genAlloca(to, base, *(LowerInstAlloca*)inst, regs, frame, objects);
            break;
        case LowerInst::Load:
            genLoad(to, base, *(LowerInstLoad*)inst, regs, addrRegs);
            break;
        case LowerInst::Store:
            genStore(to, base, *(LowerInstStore*)inst, regs, addrRegs);
            break;
        case LowerInst::Copy:
            genCopy(to, base, *(LowerInstCopy*)inst, regs);
            break;
        case LowerInst::SetPattern:
            genSetPattern(to, base, *(LowerInstSetPattern*)inst, regs);
            break;

        case LowerInst::X86PushArg:
            genPushArg(to, base, *(LowerInstX86PushArg*)inst, regs);
            break;
        case LowerInst::Call:
            // Handled by genControl-adjacent logic in genFunction (calls are ordinary
            // instructions, not terminators, but need the same callee/relocation handling).
            assertTrue(false);
            break;

        case LowerInst::Je:
        case LowerInst::Jmp:
        case LowerInst::Ret:
            assertTrue("terminators are generated by genControl, not genInst" == nullptr);
            break;
        case LowerInst::Phi:
            // No code generation needed, this is done via moves in each incoming block's terminator.
            break;

        case LowerInst::X86Address:
            // Embedded into any uses - no code emitted at this instruction's own position.
            // Its resolved operand registers are recorded into addrRegs below for consumers.
            break;
        case LowerInst::X86Lea:
            genLea(to, base, *(LowerInstX86Address*)inst, regs);
            break;
        case LowerInst::X86Bswap:
            genBswap(to, *(LowerInstUnary*)inst, regs);
            break;
        case LowerInst::X86Push:
            genPush(to, base, *(LowerInstUnary*)inst, regs);
            break;
        case LowerInst::X86Pop:
            genPop(to, regs.creates[0]);
            break;
    }

    if(inst->kind == LowerInst::X86Address) {
        addrRegs.add(inst, &regs);
    }
}

// Resolves a call's callee operand into either a direct rel32 call (when the callee is a
// statically-known function, loaded via a Fun value) or an indirect call through a register.
static void genCall(AsmModule& to, LowerBase base, LowerInstCall& i, const InstRegs& regs) {
    auto callee = base[i.used()[0]];

    if(i.getCallType() == LowerCallType::Syscall) {
        // SYSCALL (0f 05). A syscall has no callee to resolve: used()[0] is the syscall number,
        // which allocateRegisters has already placed in rax like any other constrained argument.
        to.buffer.writeByte(0x0f);
        to.buffer.writeByte(0x05);
    } else if(callee->inst()->kind == LowerInst::Fun) {
        to.buffer.writeByte(0xe8);
        to.addRelocation(base[((LowerInstFun*)callee->inst())->target]);
    } else if(isReg(callee)) {
        auto r = reg(regs.uses[0]);
        if(needsRex(r)) to.buffer.writeByte(makeRex(false, r, 0, 0));
        to.buffer.writeByte(0xff);
        to.buffer.writeByte(makeMod(3, r, 2));
    } else {
        assertTrue("unsupported call target" == nullptr);
    }
}

static void genControl(AsmModule& to, LowerBase base, LowerInst* inst, const InstRegs& regs, LowerBlock* next, const FrameLayout& frame) {
    // Statically detect if new instruction were added that aren't being handled here.
    static_assert(LowerInst::LastTerminator - LowerInst::FirstTerminator == 2, "missing code generation for terminating instructions.");
    assertTrue(isTerminator(inst));

    switch(inst->kind) {
        case LowerInst::Je: {
            auto je = (LowerInstJe*)inst;
            auto cmp = je->getEmbeddedCmp();
            LowerCmp cond;

            if(cmp) {
                cond = cmp.unwrap();
            } else {
                genTestReg(to, LowerType::Int32, reg(regs.uses[0]), reg(regs.uses[0]));
                cond = LowerCmp::neq;
            }

            if(base[je->otherwise] == next) {
                to.buffer.writeByte(0x0f);
                to.buffer.writeByte(getJumpOp(cond));
                to.addRelocation(base[je->then]);
            } else if(base[je->then] == next) {
                to.buffer.writeByte(0x0f);
                to.buffer.writeByte(getJumpOp(negateCmp(cond)));
                to.addRelocation(base[je->otherwise]);
            } else {
                to.buffer.writeByte(0x0f);
                to.buffer.writeByte(getJumpOp(cond));
                to.addRelocation(base[je->then]);

                to.buffer.writeByte(0xe9);
                to.addRelocation(base[je->otherwise]);
            }
            break;
        }
        case LowerInst::Jmp: {
            auto jmp = (LowerInstJmp*)inst;
            if(base[jmp->then] != next) {
                to.buffer.writeByte(0xe9);
                to.addRelocation(base[jmp->then]);
            }
            break;
        }
        case LowerInst::Ret:
            // After the terminator's own moves, which have already placed the return values in the
            // registers the convention returns them in. Those and the saved registers are disjoint
            // by construction - a result register is one the call clobbers, and a saved register is
            // one it doesn't - so restoring here cannot overwrite a value about to be returned.
            genEpilogue(to, frame);
            genRet(to);
            break;
    }
}

void genFunction(Context& context, LowerBase base, AsmModule& to, LowerFunction& fun, FunctionRegs& regs, InstEmitCallback onInst, void* onInstCtx) {
    auto blocks = fun.blocks.contents(base);
    to.startFunction(&fun);

    // The stack is worked out in full before a byte is emitted: the prologue has to come first but
    // depends on things only the whole function decides (which registers were saved, whether rsp is
    // going to move, how much room the locals need), and every frame reference in the body needs
    // the same answers. See frame.cpp.
    auto frame = computeFrameLayout(context, base, fun, targetConstraints(), regs);
    assertTrue(verifyFrameLayout(context, fun, regs.frame, frame)); // debug builds only

    // The prologue belongs to the function rather than to any instruction in it, so it is reported
    // with a null instruction (see InstEmitCallback) and only when it emitted something. Its
    // counterpart is emitted by genControl at each return, where it falls inside the terminator's
    // byte range.
    auto prologueStart = U32(to.buffer.offset());
    genPrologue(to, frame);

    if(onInst && U32(to.buffer.offset()) != prologueStart) {
        InstRegs none;
        onInst(onInstCtx, nullptr, none, prologueStart, U32(to.buffer.offset()));
    }

    for(Size i = 0; i < blocks.size(); i++) {
        auto b = base[blocks[i]];
        to.startBlock(b);

        auto found = regs.blocks.get(b);
        assertTrue(found.isJust());
        auto& blockRegs = found.unwrap();
        auto insts = b->instructions.contents(base);

        assertTrue(blockRegs.insts.size() == insts.size() + 1);

        HashMap<LowerInst*, const InstRegs*> addrRegs;

        // Operand placement, the instruction itself, then result placement - uniform for every
        // instruction, so nothing that emits code has to remember to handle its own moves.
        for(Size j = 0; j < insts.size(); j++) {
            auto inst = base[insts[j]];
            auto& instRegs = blockRegs.insts[j];
            auto start = U32(to.buffer.offset());

            genMoves(to, frame, regs.frame, instRegs.moves);

            if(inst->kind == LowerInst::Call) {
                genCall(to, base, *(LowerInstCall*)inst, instRegs);
            } else {
                genInst(to, base, inst, instRegs, addrRegs, frame, regs.frame);
            }

            genMoves(to, frame, regs.frame, instRegs.postMoves);

            if(onInst) onInst(onInstCtx, inst, instRegs, start, U32(to.buffer.offset()));
        }

        // Keep track of the block that will be positioned immediately after this one,
        // which allows us to remove some unconditional jumps.
        auto next = i + 1 >= blocks.size() ? nullptr : base[blocks[i + 1]];
        auto termStart = U32(to.buffer.offset());
        auto terminator = base[b->terminator];
        auto& termRegs = blockRegs.insts[insts.size()];

        // A terminator's moves include the copies that feed the successor's phis, so they have to
        // land before the branch itself.
        genMoves(to, frame, regs.frame, termRegs.moves);
        genControl(to, base, terminator, termRegs, next, frame);
        genMoves(to, frame, regs.frame, termRegs.postMoves);

        if(onInst) onInst(onInstCtx, terminator, termRegs, termStart, U32(to.buffer.offset()));

        to.endBlock(b);
    }
}

void AsmModule::resolveRelocations() {
    for(auto& r: relocations) {
        U32 target;

        if(r.function) {
            auto o = functionOffsets.getValue(r.function);
            assertTrue(o.isJust());
            target = o.unwrap();
        } else if(r.global) {
            auto o = globalOffsets.getValue(r.global);
            assertTrue(o.isJust()); // a referenced global was never emitted via addGlobal()
            target = o.unwrap();
        } else {
            auto o = blockOffsets.getValue(r.block);
            assertTrue(o.isJust());
            target = blocks[o.unwrap()].startOffset;
        }

        auto rel = I32(target) - I32(r.siteOffset + 4);
        auto savedOffset = buffer.offset();

        buffer.offset(r.siteOffset);
        buffer.writeInt<LittleEndian>(U32(rel));
        buffer.offset(savedOffset);
    }
}
