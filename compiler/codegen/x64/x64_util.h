#pragma once

#include "../../lower/lower_inst.h"
#include "gen.h"

/*
 * Everything about the register file - banks, classes, physical registers, locations, register sets,
 * the frame pointer and the scratch reserve - is in target.h, which gen.h includes. What is left
 * here is the encoding-level and IR-level predicates the backend shares.
 */

inline bool isImm(LowerValue* v) {
    return v->inst()->kind == LowerInst::Imm && (v->flags & LowerValue::Implicit);
}

inline bool isMem(LowerValue* v) {
    return v->inst()->kind == LowerInst::X86Address;
}

inline bool isReg(LowerValue* v) {
    return !isImm(v) && !isMem(v);
}

// True for values that need no location at all: embedded immediates, comparisons folded into flags,
// an elided direct callee, and the result of an argument store (which stands in for the argument in
// the call's operand list and is never read).
inline bool isImplicit(LowerValue* v) {
    return v->flags & LowerValue::Implicit;
}

inline bool needsRex(U8 reg) {
    return reg & 8;
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
