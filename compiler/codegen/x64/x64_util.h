#pragma once

#include "../../lower/lower_inst.h"

enum class IntRegister: U8 {
    rax = 0, rcx, rdx, rbx, rsp, rbp, rsi, rdi,
    r8, r9, r10, r11, r12, r13, r14, r15,
};

enum class XmmRegister: U8 {
    xmm0 = 0x10, xmm1, xmm2, xmm3, xmm4, xmm5, xmm6, xmm7,
    xmm8, xmm9, xmm10, xmm11, xmm12, xmm13, xmm14, xmm15,
};

inline bool isImm(LowerValue* v) {
    return v->inst()->kind == LowerInst::Imm && (v->flags & LowerValue::Implicit);
}

inline bool isMem(LowerValue* v) {
    return v->inst()->kind == LowerInst::X86Address;
}

inline bool isReg(LowerValue* v) {
    return !isImm(v) && !isMem(v);
}

inline bool needsRex(U8 reg) {
    return reg & 8;
}

inline bool needsRex(const LowerValue* v) {
    return needsRex(v->reg);
}

inline bool needsRex(const LowerInstBinary* i) {
    return needsRex(i->lhs) || needsRex(i->rhs);
}

inline bool is64Bit(LowerType type) {
    return type == LowerType::Int64 || type == LowerType::Float64 || type == LowerType::Pointer;
}

inline Maybe<U8> encodeImm8(LowerValue* v) {
    assertTrue(v->inst()->kind == LowerInst::Imm);

    auto imm = ((LowerImm*)v)->i;
    if((imm & 0xffffffffffffff80) == 0xffffffffffffff80 || (imm & 0x7f) == imm) {
        return Just(U8(imm));
    } else {
        return Nothing();
    }
}

inline Maybe<U32> encodeImm32(LowerValue* v) {
    assertTrue(v->inst()->kind == LowerInst::Imm);

    auto imm = ((LowerImm*)v)->i;
    if((imm & 0xffffffff80000000) == 0xffffffff80000000 || (imm & 0x7fffffff) == imm) {
        return Just(U32(imm));
    } else {
        return Nothing();
    }
}
