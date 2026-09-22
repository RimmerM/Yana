#include "opt_internal.h"

namespace jsir {

void Optimizer::buildUses() {
    auto count = body.values.size();

    useStart.clear();
    useStart.reserve(count + 1);
    for(U32 i = 0; i <= count; i++) useStart.push(0);

    eachInst([&](JsInst& inst) {
        eachUse(inst, [&](JsValue* value, JsUseRole) { useStart[value->id.index + 1]++; });
    });

    for(U32 i = 0; i < count; i++) useStart[i + 1] += useStart[i];

    useList.clear();
    useList.reserve(useStart[count]);
    for(U32 i = 0; i < useStart[count]; i++) useList.push(Use {});

    auto& fill = s.fill;
    fill.clear();
    fill.reserve(count);
    for(U32 i = 0; i < count; i++) fill.push(useStart[i]);

    eachInst([&](JsInst& inst) {
        eachUse(inst, [&](JsValue* value, JsUseRole role) {
            useList[fill[value->id.index]++] = Use { &inst, role };
        });
    });

    liveUses.clear();
    liveUses.reserve(count);
    for(U32 i = 0; i < count; i++) liveUses.push(useStart[i + 1] - useStart[i]);
}

/*
 * The use counts one instruction stops making - `JsBuilder::countUses` run backwards, which is
 * what makes the number `codegen/js/opt.cpp` reads for its one-use inlining still true.
 *
 * A place's owner goes with the **last** instruction naming that place rather than with each of
 * them; see `placeRefs`.
 */
void Optimizer::release(JsInst& inst) {
    eachUse(inst, [&](JsValue* used, JsUseRole) {
        if(used->id.index < liveUses.size() && liveUses[used->id.index]) {
            liveUses[used->id.index]--;
        }
    });

    forEachUse(base, inst, [&](JsirPtr<JsValue> used) {
        if(used && base[used]->uses) base[used]->uses--;
    });

    auto placeId = JsPlaceId();
    if(inst.kind == JsInst::Load) placeId = ((JsInstLoad&)inst).place;
    else if(inst.kind == JsInst::Store) placeId = ((JsInstStore&)inst).place;
    else return;

    if(!placeId.valid() || placeId.index >= placeRefs.size()) return;
    if(!placeRefs[placeId.index] || --placeRefs[placeId.index]) return;

    auto& place = builder.place(placeId);
    if(place.owner && base[place.owner]->uses) base[place.owner]->uses--;
    if(place.index && base[place.index]->uses) base[place.index]->uses--;
    if(place.bitScale && base[place.bitScale]->uses) base[place.bitScale]->uses--;
}

void Optimizer::countPlaceRefs() {
    placeRefs.clear();
    placeRefs.reserve(body.places.size());
    for(U32 i = 0; i < body.places.size(); i++) placeRefs.push(0);

    eachInst([&](JsInst& inst) {
        auto placeId = JsPlaceId();
        if(inst.kind == JsInst::Load) placeId = ((JsInstLoad&)inst).place;
        else if(inst.kind == JsInst::Store) placeId = ((JsInstStore&)inst).place;
        else return;

        if(placeId.valid() && placeId.index < placeRefs.size()) placeRefs[placeId.index]++;
    });
}

Buffer<Use> Optimizer::usesOf(JsValue* value) {
    auto first = useStart[value->id.index];
    return Buffer<Use> { useList.pointer() + first, useStart[value->id.index + 1] - first };
}

bool Optimizer::computeDominance() {
    if(!flow) flow = &jsCfgOf(target, body);
    return flow->reducible;
}

// The lowest block dominating both - the dominator tree's own meet, as the two chains walked
// towards each other by reverse postorder. `JsCfg::rpo` is a total order, so this terminates.
U32 Optimizer::commonDominator(U32 a, U32 b) {
    while(a != b) {
        while(flow->rpo[a] > flow->rpo[b]) a = flow->idom[a];
        while(flow->rpo[b] > flow->rpo[a]) b = flow->idom[b];
    }

    return a;
}

void Optimizer::applyRewrites() {
    /*
     * Where each instruction sits in its own block, in one pass over the body.
     *
     * A scan of the block per rewrite is what this was, and the two numbers multiply: a body
     * the elision fires on a hundred times has a hundred scans of the block each rewrite is in.
     * The index is rebuilt here rather than maintained, because the only thing that moves an
     * instruction is this loop and it replaces in place.
     */
    auto& instAt = s.instAt;
    instAt.clear();
    instAt.reserve(U32(body.insts.size()));
    for(U32 i = 0; i < body.insts.size(); i++) instAt.push(maxLimit<U32>);

    for(U32 b = 0; b < body.blocks.size(); b++) {
        auto block = body.block(base, JsBlockId(b));
        for(Size i = 0; i < block->instructionList.size(); i++) {
            instAt[base[block->instructionList.get(base, i)]->id.index] = U32(i);
        }
    }

    for(auto& rewrite: pending) {
        auto& inst = *rewrite.inst;
        auto block = base[inst.block];

        auto at = Size(inst.id.index < instAt.size() ? instAt[inst.id.index] : maxLimit<U32>);
        if(at >= block->instructionList.size()) continue;
        if(base[block->instructionList.get(base, at)] != &inst) continue;

        release(inst);

        auto replacement = new (target.arena) JsInstAlias(inst.source, rewrite.from - base);
        replacement->id = inst.id;
        replacement->block = inst.block;
        replacement->result = inst.result;
        rewrite.from->uses++;
        if(rewrite.from->id.index < liveUses.size()) liveUses[rewrite.from->id.index]++;

        if(inst.result) base[inst.result]->definer = (JsInst*)replacement - base;

        block->instructionList.set(base, at, (JsInst*)replacement - base);
    }

    pending.clear();
}

// Where each live instruction sits in its own block - `s.instAt`, which `applyRewrites` also
// fills and clears. Rebuilt per pass rather than maintained, for that function's reason.
void Optimizer::indexPositions() {
    auto& instAt = s.instAt;
    instAt.clear();
    instAt.reserve(U32(body.insts.size()));
    for(U32 i = 0; i < body.insts.size(); i++) instAt.push(maxLimit<U32>);

    for(U32 b = 0; b < body.blocks.size(); b++) {
        auto block = body.block(base, JsBlockId(b));
        if(block->deleted) continue;

        for(Size i = 0; i < block->instructionList.size(); i++) {
            instAt[base[block->instructionList.get(base, i)]->id.index] = U32(i);
        }
    }
}

U32 Optimizer::positionOf(JsInst& inst) {
    return inst.id.index < s.instAt.size() ? s.instAt[inst.id.index] : maxLimit<U32>;
}

bool Optimizer::strictlyDominates(U32 above, U32 below) {
    for(U32 depth = 0; below != kNoBlock && depth < body.blocks.size(); depth++) {
        below = flow->idom[below];
        if(below == above) return true;
    }

    return false;
}

/*
 * Whether `inst` has run, and cannot run again, by the time `clone` does.
 *
 * Earlier in the clone's own block, or in a block that strictly dominates it. The second half is
 * only a statement about *order* because the caller asks this of a clone in no loop - one in a
 * loop asks `runsBeforeInIteration` instead: a
 * block the clone's block could reach again after running would put both in one cycle, which in
 * a reducible graph is a natural loop the clone's block would be a member of.
 */
bool Optimizer::runsBefore(JsInst& inst, JsInst& clone) {
    auto at = base[inst.block]->index.index;
    auto cloneAt = base[clone.block]->index.index;

    if(at == cloneAt) return positionOf(inst) < positionOf(clone);
    return strictlyDominates(at, cloneAt);
}

/*
 * Whether `inst` has run by the time `clone` does **in the same iteration** of every loop the
 * clone is in - J86, the loop half of `runsBefore`.
 *
 * Earlier in the clone's own block, or in a block that strictly dominates it and is in exactly
 * the loops the clone's block is in. The second half is an order for the reason `runsBefore`'s
 * is, one loop level down: a path from that block to the clone that went round a back edge
 * would pass the innermost header, which dominates the block, so it would pass the block again.
 * What it says nothing about is the *next* iteration, which is `refilledFirst`'s question.
 */
bool Optimizer::runsBeforeInIteration(JsInst& inst, JsInst& clone) {
    auto at = base[inst.block]->index.index;
    auto cloneAt = base[clone.block]->index.index;

    if(at == cloneAt) return positionOf(inst) < positionOf(clone);
    return flow->loopMask[at] == flow->loopMask[cloneAt] && strictlyDominates(at, cloneAt);
}

/*
 * Every load and store of each variable slot, in `eachInst`'s order - block order, and program
 * order within one - as a CSR index into `s.touches` by `s.touchStart`, built once per pass so
 * that asking about one slot costs that slot's touches rather than a walk of the body. A body the
 * conversion wrote a thousand clones into is exactly the one a walk per clone was quadratic in.
 *
 * One index for every pass that asks, over `slotOfPlace`, so a bit range over a packed record
 * local is a touch of that local everywhere. There were three - a `Var`-only one for the move
 * passes, one per slot for the dead-store walk, and a body walk per clone in `updateAsLiteral` -
 * and the first disagreed with the other two about the bit range.
 */
void Optimizer::indexTouches() {
    auto count = U32(body.variables.size());
    auto& start = s.touchStart;
    auto& touches = s.touches;
    start.clear();
    for(U32 i = 0; i <= count; i++) start.push(0);

    eachInst([&](JsInst& inst) {
        bool whole = false;
        auto slot = slotOfPlace(placeOfAccess(inst), whole);
        if(slot < count) start[slot + 1]++;
    });

    for(U32 i = 0; i < count; i++) start[i + 1] += start[i];

    auto& fill = s.fill;
    fill.clear();
    for(U32 i = 0; i < count; i++) fill.push(start[i]);

    touches.clear();
    for(U32 i = 0; i < start[count]; i++) touches.push(nullptr);

    eachInst([&](JsInst& inst) {
        bool whole = false;
        auto slot = slotOfPlace(placeOfAccess(inst), whole);
        if(slot < count) touches[fill[slot]++] = &inst;
    });
}

void Optimizer::insertAt(JsBlock* block, Size at, JsInst* inst) {
    auto& list = block->instructionList;
    auto count = list.size();
    if(at >= count) {
        list.push(target.arena, inst - base);
        return;
    }

    list.push(target.arena, list.get(base, count - 1));
    for(Size i = count - 1; i > at; i--) list.set(base, i, list.get(base, i - 1));
    list.set(base, at, inst - base);
}

/*
 * Where `inst` sits in `block` now, or the block's size where it is not there - by a scan, for
 * the passes that ask while they edit the list and so cannot read `indexPositions`' table. From
 * the end, because the one asked most is a terminator.
 */
Size Optimizer::indexIn(JsBlock* block, JsInst* inst) {
    auto& list = block->instructionList;
    for(Size i = list.size(); i-- > 0;) {
        if(base[list.get(base, i)] == inst) return i;
    }
    return list.size();
}

// `inst` takes `old`'s id, position and result, and `old` is released.
void Optimizer::replaceInst(JsInst* old, JsInst* inst) {
    auto block = base[old->block];
    auto at = indexIn(block, old);

    release(*old);
    inst->id = old->id;
    inst->block = old->block;
    if(old->result && jsInstResult(inst->kind) != kNoResult) {
        inst->result = old->result;
        base[old->result]->definer = inst - base;
    }

    body.insts.set(base, old->id.index, inst - base);
    block->instructionList.set(base, at, inst - base);
}

void Optimizer::removeInst(JsInst* inst) {
    auto block = base[inst->block];
    auto at = indexIn(block, inst);
    if(at == block->instructionList.size()) return;

    release(*inst);
    block->instructionList.remove(base, at);
}

/*
 * The variable slot a place reads or writes, and whether it is the whole of it.
 *
 * A `BitRange` over a `Var` word is a field of a packed record local: a read of it reads the
 * word and a write of it is a read-modify-write, so it is never a whole store and it is always a
 * read. Every other place kind is no variable at all.
 */
U32 Optimizer::slotOfPlace(JsPlaceId id, bool& whole) {
    whole = false;
    if(!id.valid()) return kNoVariable;

    auto& place = builder.place(id);
    if(place.kind == JsPlaceKind::Var) {
        whole = true;
        return place.slot;
    }

    if(place.kind == JsPlaceKind::BitRange && place.word.valid() &&
       builder.place(place.word).kind == JsPlaceKind::Var) {
        return builder.place(place.word).slot;
    }

    return kNoVariable;
}

// The place a load or a store names, and no place for any other instruction.
JsPlaceId Optimizer::placeOfAccess(JsInst& inst) {
    if(inst.kind == JsInst::Load) return ((JsInstLoad&)inst).place;
    if(inst.kind == JsInst::Store) return ((JsInstStore&)inst).place;
    return JsPlaceId();
}

// Whether `id` is the whole of variable `slot` - a load of its value, or a store replacing it.
bool Optimizer::isWholeVariable(JsPlaceId id, U32 slot) {
    bool whole = false;
    return slotOfPlace(id, whole) == slot && whole;
}

} // namespace jsir
