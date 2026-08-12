#include "lower_inst.h"
#include "lower_fold.h"

/*
 * Editing what is already built.
 *
 * Three operations, and they are here rather than beside the pass that first needed them because a
 * use list is two-way: the validator checks that an operand names a value *and* that the value names
 * its user, so a rewrite that updates one direction and not the other is as broken as one that
 * updates neither. Anything that moves an operand goes through these.
 */

// Removing one reference from a value's use list. One rather than all: an instruction that uses the
// same value twice appears twice, and the list has to keep saying so.
inline void dropUse(LowerBase base, LowerPtr<LowerValue> value, LowerPtr<LowerInst> user) {
    auto& uses = base[value]->uses;
    for(Size i = 0; i < uses.size(); i++) {
        if(uses.get(base, i) == user) {
            uses.remove(base, i);
            return;
        }
    }
}

// Taking an instruction out of circulation: it stops counting as a user of everything it read. The
// instruction itself is then dropped from its block by whoever is rebuilding the list.
inline void detach(LowerBase base, LowerInst* inst) {
    auto used = inst->used();
    for(Size i = 0; i < used.length; i++) dropUse(base, used.ptr[i], inst - base);
}

// Pointing one operand of an instruction at a different value, both directions at once. The operand
// is named by reference because it is a field of the instruction - `binary->lhs` and the rest - so
// that a caller rewriting one of two identical operands moves exactly the one it meant.
inline void setOperand(LowerBase base, Region<LowerRegion>& arena, LowerInst* inst,
                       LowerPtr<LowerValue>& operand, LowerValue* to) {
    auto target = to - base;
    if(operand == target) return;

    dropUse(base, operand, inst - base);
    operand = target;
    base[target]->uses.push(arena, inst - base);
}

// Pointing every reader of one value at another.
inline void replaceUses(LowerBase base, Region<LowerRegion>& arena, LowerPtr<LowerValue> from,
                        LowerPtr<LowerValue> to) {
    // Null would mean replacing a value with nothing where something reads it. Asserted rather than
    // tolerated because the failure is otherwise a use of whatever sits at offset zero of the arena.
    assertTrue(to != nullptr);
    if(from == to) return;

    auto& uses = base[from]->uses;
    while(uses.size()) {
        auto userPtr = uses.get(base, uses.size() - 1);
        uses.remove(base, uses.size() - 1);

        // Every matching operand at once, and one use entry per visit: an instruction reading the
        // value twice is in the list twice, so the counts stay equal either way round.
        auto operands = base[userPtr]->used();
        for(Size i = 0; i < operands.length; i++) {
            if(operands.ptr[i] == from) operands.ptr[i] = to;
        }

        base[to]->uses.push(arena, userPtr);
    }
}

inline LowerInst* nop(LowerBase base, LowerModule& module, LowerBlock& block) {
    return block.addInst(base, new (module.arena) LowerInst(LowerInst::Nop));
}

// The three builders that can produce a value already known ask lower_fold.h first. It is asked here
// rather than at the sites that build bit arithmetic because every producer of lower IR comes through
// these, and because an operand is always built before its consumer - so a chain of operations over
// literals collapses from the bottom up without anything having to walk it afterwards.
template<LowerInst::Kind kind>
inline LowerInst* unary(LowerBase base, LowerModule& module, LowerBlock& block, LowerValue* arg, LowerType type, StringId name) {
    static_assert(kind >= LowerInst::FirstUnary && kind <= LowerInst::LastUnary);

    if(auto folded = foldUnaryArith(base, module, block, kind, arg, type, name)) return folded;
    return block.addInst(base, new (module.arena) LowerInstUnary(kind, name, type, arg - base));
}

template<bool signedSource, bool signedResult>
inline LowerInst* cast(LowerBase base, LowerModule& module, LowerBlock& block, LowerValue* arg, LowerType type, StringId name) {
    if(auto folded = foldCast(base, module, block, arg, type, signedSource, name)) return folded;
    return block.addInst(base, new (module.arena) LowerInstCast(name, type, arg - base, signedSource, signedResult));
}

template<LowerInst::Kind kind>
inline LowerInst* binary(LowerBase base, LowerModule& module, LowerBlock& block, LowerValue* lhs, LowerValue* rhs, LowerType type, StringId name) {
    static_assert(kind >= LowerInst::FirstBinary && kind <= LowerInst::LastBinary);

    if(auto folded = foldBinary(base, module, block, kind, lhs, rhs, type, name)) return folded;
    return block.addInst(base, new (module.arena) LowerInstBinary(name, type, lhs - base, rhs - base, kind));
}

// `type` is the *result*, which is a Bool for two scalars and a mask of their shape for two vectors -
// see LowerInstCmp, where the parameter is argued. It defaults to the scalar answer rather than being
// computed from the operands, because a comparison whose operands are vectors is the only caller that
// has anything else to say and it is the caller that knows the mask.
inline LowerInst* cmp(LowerBase base, LowerModule& module, LowerBlock& block, LowerValue* lhs, LowerValue* rhs, LowerCmp c, StringId name, LowerType type = LowerType::Int32) {
    return block.addInst(base, new (module.arena) LowerInstCmp(name, lhs - base, rhs - base, c, type));
}

inline LowerInst* load(LowerBase base, LowerModule& module, LowerBlock& block, LowerValue* from, U32 width, bool signExtend, LowerType type, StringId name) {
    return block.addInst(base, new (module.arena) LowerInstLoad(from - base, name, type, width, signExtend));
}

template<class Prepare>
inline LowerInst* call(LowerBase base, LowerModule& module, LowerBlock& block, Size createdCount, Size usedCount, LowerCallType callType, Prepare&& prepare) {
    auto embeddedSize = sizeof(LowerValue) * createdCount + sizeof(LowerPtr<LowerValue>) * usedCount;
    auto inst = (LowerInstCall*)module.arena.alloc(sizeof(LowerInstCall) + embeddedSize);
    new (inst) LowerInstCall(createdCount, usedCount, callType);

    prepare(inst);
    return block.addInst(base, inst);
}

inline LowerInst* je(LowerBase base, LowerModule& module, LowerBlock& block, LowerValue* cond, LowerBlock* then, LowerBlock* otherwise) {
    return block.addInst(base, new (module.arena) LowerInstJe(cond - base, then - base, otherwise - base));
}

inline LowerInst* jmp(LowerBase base, LowerModule& module, LowerBlock& block, LowerBlock* to) {
    return block.addInst(base, new (module.arena) LowerInstJmp(to - base));
}

template<class Prepare>
inline LowerInst* ret(LowerBase base, LowerModule& module, LowerBlock& block, Size createdCount, Prepare&& prepare) {
    auto inst = (LowerInstRet*)module.arena.alloc(sizeof(LowerInstRet) + sizeof(LowerPtr<LowerValue>) * createdCount);
    new (inst) LowerInstRet;

    prepare(inst);
    return block.addInst(base, new (module.arena) LowerInstRet());
}
