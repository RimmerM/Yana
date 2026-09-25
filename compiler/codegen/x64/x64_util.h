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
//
// A vector is its own width and is asked separately, the slot classes above 64 bits being what the
// scalar answer has no room for. Answering 8 for one was not a refusal that had been reasoned about
// - it made every question about a vector load compare 8 against 16 and decline, which read as "a
// vector load is never foldable" and was really "this function does not know about vectors".
inline U32 accessWidthOf(LowerType type) {
    if(isVectorLike(type)) return type.byteWidth();
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

/*
 * The constant an immediate field carries for operand `v` of `user` - J157.
 *
 * A 32-bit operation sign-extends its immediate to 32 bits rather than to 64, so an `I32` constant
 * with its top bit set is an ordinary small immediate there: `0xfffffffe` is `and r32, -2`, one byte.
 * Read as the 64-bit number `immValue` answers it fits no immediate field at all, and such a constant
 * was materialized into a register at every use - `mov $0xffffffff, %r11d ; cmp %r11d, %eax`. So the
 * legality checks, the selector and legalization all ask here, and agree.
 *
 * Only for the kinds whose operation is exactly as wide as the constant. A cast materializes its
 * source at another width, an outgoing argument is stored as eight bytes, and a store or an update
 * narrower or wider than four bytes is not reading an `I32` at its own width.
 */
inline U64 embeddedImmValue(LowerBase base, LowerInst* user, LowerValue* v) {
    auto raw = immValue(v);
    if(v->type != LowerType::Int32 || (raw >> 31) == 0) return raw;

    auto signExtended = U64(I64(I32(U32(raw))));

    switch(user->kind) {
        case LowerInst::Add:
        case LowerInst::Sub:
        case LowerInst::And:
        case LowerInst::Or:
        case LowerInst::Xor:
        case LowerInst::Cmp:
            return signExtended;

        case LowerInst::Store: {
            auto store = (LowerInstStore*)user;
            return base[store->value] == v && store->getWidth() == 4 ? signExtended : raw;
        }

        default:
            return raw;
    }
}
