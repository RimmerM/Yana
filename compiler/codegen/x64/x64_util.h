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

// True for values that need no location at all: embedded immediates, comparisons folded into flags,
// an elided direct callee, and the result of an argument store (which stands in for the argument in
// the call's operand list and is never read).
inline bool isImplicit(LowerValue* v) {
    return v->flags & LowerValue::Implicit;
}

inline bool needsRex(U8 reg) {
    return reg & 8;
}

// The bytes a value of this type occupies, which is the access a load performs when it extends
// nothing. Every scalar the lowering produces is four bytes or eight, which is exactly the
// distinction a slot class already makes. Shared because two passes ask the same question of the
// same load: whether it may be folded into its reader, and whether it may be a recipe.
inline U32 accessWidthOf(LowerType type) {
    return stackSlotClassFor(type) == StackSlotClass::Slot32 ? 4 : 8;
}

inline bool is64Bit(LowerType type) {
    return type == LowerType::Int64 || type == LowerType::Float64 || type == LowerType::Pointer;
}

// The constant an Imm carries. `v` points at a LowerImm's *embedded* `result` field rather than at
// the start of the enclosing LowerImm object, so `v->inst()` (not a raw `(LowerImm*)v` cast) is
// what recovers the real LowerImm* - it undoes the `result` field's offset via LowerValue::inset.
//
// Whether a given form can carry it is fitsImmediate in machine.h, which is the one statement of
// immediate legality.
inline U64 immValue(LowerValue* v) {
    assertTrue(v->inst()->kind == LowerInst::Imm);
    return ((LowerImm*)v->inst())->i;
}
