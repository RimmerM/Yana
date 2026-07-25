#pragma once

#include "../../lower/lower_inst.h"
#include "gen.h"

enum class IntRegister: U8 {
    rax = 0, rcx, rdx, rbx, rsp, rbp, rsi, rdi,
    r8, r9, r10, r11, r12, r13, r14, r15,
};

enum class XmmRegister: U8 {
    xmm0 = 0x10, xmm1, xmm2, xmm3, xmm4, xmm5, xmm6, xmm7,
    xmm8, xmm9, xmm10, xmm11, xmm12, xmm13, xmm14, xmm15,
};

static constexpr Size kRegCount = 16;

// rsp and rbp are never handed out: rsp is required for push/pop/call/ret to work at all, and rbp
// is conventionally reserved for a frame pointer even though this MVP generates no prologue that
// sets one up yet.
static constexpr U64 kReservedRegs =
    (U64(1) << (Size)IntRegister::rsp) | (U64(1) << (Size)IntRegister::rbp);

// The general registers a value can be given. Anything outside this set is either reserved above or
// does not exist, so a calling convention's preserved set is stated relative to it - the registers a
// function has to give back are exactly the ones it could have taken in the first place.
static constexpr U64 kAllocatableRegs = ((U64(1) << kRegCount) - 1) & ~kReservedRegs;

// The bit this register occupies in a general-register mask (a clobber set, an avoid set, a
// convention's preserved set). Anything that isn't a general register - an xmm register, a stack
// slot, kInvalidReg - contributes nothing: those masks describe general registers only.
inline U64 regBit(RegId reg) {
    return getRegClass(reg) == GenReg ? U64(1) << getRegIndex(reg) : 0;
}

inline bool isImm(LowerValue* v) {
    return v->inst()->kind == LowerInst::Imm && (v->flags & LowerValue::Implicit);
}

inline bool isMem(LowerValue* v) {
    return v->inst()->kind == LowerInst::X86Address;
}

inline bool isReg(LowerValue* v) {
    return !isImm(v) && !isMem(v);
}

// True for values that don't need a physical register/stack slot allocated at all:
// embedded immediates, comparisons folded into flags, and pushed call arguments
// (LowerInstPushArg's result, which only exists to record push ordering).
inline bool isImplicit(LowerValue* v) {
    return v->flags & LowerValue::Implicit;
}

inline bool needsRex(U8 reg) {
    return reg & 8;
}

inline bool needsRex(RegId reg) {
    return needsRex(U8(getRegIndex(reg)));
}

inline bool is64Bit(LowerType type) {
    return type == LowerType::Int64 || type == LowerType::Float64 || type == LowerType::Pointer;
}

// `v` is a LowerValue* pointing at a LowerImm's *embedded* `result` field, not at the start of
// the enclosing LowerImm object - `v->inst()` (not a raw `(LowerImm*)v` cast) is required to
// recover the real LowerImm* (it undoes the `result` field's offset via LowerValue::inset).
inline Maybe<U8> encodeImm8(LowerValue* v) {
    assertTrue(v->inst()->kind == LowerInst::Imm);

    auto imm = ((LowerImm*)v->inst())->i;
    if((imm & 0xffffffffffffff80) == 0xffffffffffffff80 || (imm & 0x7f) == imm) {
        return Just(U8(imm));
    } else {
        return Nothing();
    }
}

inline Maybe<U32> encodeImm32(LowerValue* v) {
    assertTrue(v->inst()->kind == LowerInst::Imm);

    auto imm = ((LowerImm*)v->inst())->i;
    if((imm & 0xffffffff80000000) == 0xffffffff80000000 || (imm & 0x7fffffff) == imm) {
        return Just(U32(imm));
    } else {
        return Nothing();
    }
}
