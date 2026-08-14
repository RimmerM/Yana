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

/*
 * The two operations on a phi's *alternatives*, which are the ones that cannot be edits.
 *
 * A phi's alternatives are allocated with it - the values and their source blocks sit past the
 * instruction in one block of memory - so there is no room to add one and no way to take one out. A
 * pass that changes which edges reach a block therefore has to build a replacement, and both passes
 * that do (lower_thread.cpp and lower_merge.cpp) were separately about to.
 *
 * They are here for the reason the three above are: an alternative is an operand, so every one that
 * moves has to move on both sides of the use list, and a rebuild that forgets the old phi's readers
 * leaves an IR that prints correctly and walks wrongly.
 */

// An unattached phi with room for a stated number of alternatives, filled in and added to a block by
// the caller - adding it is what registers its reads, so the whole group has to be settled first.
inline LowerInstPhi* makePhi(Region<LowerRegion>& arena, LowerType type, U32 alternatives) {
    auto storage = arena.alloc(
        sizeof(LowerInstPhi) +
        sizeof(LowerPtr<LowerValue>) * alternatives +
        sizeof(LowerPtr<LowerBlock>) * alternatives);

    auto phi = new (storage) LowerInstPhi(StringId(), type);
    phi->usedCount = alternatives;
    return phi;
}

/*
 * One phi rebuilt without the alternatives whose edges have gone, in place of the one it replaces.
 *
 * The replacement takes the old one's result name, its source location and its readers, so nothing
 * outside the block can tell that the instruction is a different one. `removed` is any container of
 * `LowerBlock*` - a phi naming a departed block on two edges loses both, which is what makes this a
 * filter rather than a subtraction of one entry.
 */
template<class Blocks>
inline void narrowPhi(LowerBase base, Region<LowerRegion>& arena, LowerBlock* block,
                      LowerInstPhi* phi, const Blocks& removed)
{
    auto used = phi->used();
    auto sources = phi->sources();

    SmallArray<LowerPtr<LowerValue>, 8> keptValues;
    SmallArray<LowerPtr<LowerBlock>, 8> keptSources;

    for(Size i = 0; i < used.length; i++) {
        auto gone = false;
        for(auto departed: removed) {
            if(base[sources[i]] == departed) { gone = true; break; }
        }

        if(gone) continue;

        keptValues.push(used.ptr[i]);
        keptSources.push(sources[i]);
    }

    auto replacement = makePhi(arena, phi->result.type, U32(keptValues.size()));
    replacement->source = phi->source;
    replacement->result.name = phi->result.name;

    for(Size i = 0; i < keptValues.size(); i++) {
        replacement->used().ptr[i] = keptValues[i];
        replacement->sources()[i] = keptSources[i];
    }

    detach(base, (LowerInst*)phi);
    block->addInst(base, replacement);
    replaceUses(base, arena, ((LowerInstSingle*)phi)->created().ptr - base,
                &replacement->result - base);

    for(Size i = 0; i < block->phis.size(); i++) {
        if(base[block->phis.get(base, i)] != phi) continue;

        block->phis.remove(base, i);
        break;
    }
}

// Every phi of a block rebuilt without the departed edges - the shape both callers want, and the one
// that has to copy the list first: the rebuild replaces entries in the very list it walks.
template<class Blocks>
inline void narrowBlockPhis(LowerBase base, Region<LowerRegion>& arena, LowerBlock* block,
                            const Blocks& removed)
{
    SmallArray<LowerInstPhi*, 8> phis;
    for(auto phiPtr: block->phis.contents(base)) phis.push(base[phiPtr]);

    for(auto phi: phis) narrowPhi(base, arena, block, phi, removed);
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
