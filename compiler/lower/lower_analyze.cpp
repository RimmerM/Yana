#include "lower_inst.h"

/*
 * Implementation of 'A Simple, Fast Dominance Algorithm' by Keith D. Cooper, Timothy J. Harvey, and Ken Kennedy.
 */

static void traversePostorder(LowerBase base, LowerBlock* b, BlockList& target) {
    b->marker = 1;

    for(auto o: b->outgoing) {
        if(o && !base[o]->marker) traversePostorder(base, base[o], target);
    }

    b->postIndex = BlockIndex(target.size());
    target.push(b->index);
}

BlockList LowerFunction::buildPostorder(LowerBase base) {
    auto blockList = blocks.contents(base);
    assertTrue(blockList.size() < kNullBlock);

    // Reset the block ordering information.
    for(Size i = 0; i < blockList.size(); i++) {
        auto b = base[blockList[i]];
        b->index = i;
        b->marker = 0;
        b->postIndex = kNullBlock;
    }

    // Traverse to build the list.
    BlockList postorder(blockList.size());
    traversePostorder(*arena, base[blocks.get(base, 0)], postorder);

    return ::move(postorder);
}

static BlockIndex intersect(const BlockList& dominators, BlockIndex finger1, BlockIndex finger2) {
    while(finger1 != finger2) {
        while(finger1 < finger2) {
            finger1 = dominators[finger1];
        }

        while(finger2 < finger1) {
            finger2 = dominators[finger2];
        }
    }

    return finger1;
}

DominatorTree LowerFunction::buildDominatorTree(LowerBase base) {
    if(blocks.isEmpty()) return {};

    auto postorder = buildPostorder(base);

    BlockList dominators(blocks.size());
    for(Size i = 0; i < blocks.size(); i++) dominators.push(-1);

    auto startNode = BlockIndex(postorder.size() - 1);
    dominators[startNode] = startNode;

    auto changed = true;

    while(changed) {
        changed = false;

        for(Int i = startNode - 1; i >= 0; i--) {
            auto block = base[blocks.get(base, postorder[i])];
            assertTrue(block->incoming.size() > 0);

            auto idiom = base[block->incoming.get(base, 0)]->postIndex;
            assertTrue(idiom >= 0);

            for(Size j = 1; j < block->incoming.size(); j++) {
                auto p = base[block->incoming.get(base, j)]->postIndex;
                assertTrue(p >= 0);

                if(dominators[p] >= 0) idiom = intersect(dominators, p, idiom);
            }

            if(dominators[i] != idiom) {
                dominators[i] = idiom;
                changed = true;
            }
        }
    }

    return DominatorTree {
        .postorder = ::move(postorder),
        .tree = ::move(dominators),
        .startIndex = startNode,
    };
}

// Assigns a dense LiveId to every value the instruction creates. Unlike an escape-analysis-filtered
// numbering, this covers block-local values too: the LiveId doubles as the index into
// Liveness::ranges, and the register allocator needs a range for every value it has to place, not
// only for the ones that cross a block boundary. Block-local values simply never appear in any
// block's live-in/live-out set, so the extra ids cost bits in those sets and nothing else.
static void addInst(Liveness& live, LowerInst* inst) {
    if(inst->createdCount == 0) return;

    assertTrue(live.valueMap.size() + inst->createdCount < kNullLive);
    inst->liveId = live.valueMap.size();

    for(auto& v: inst->created()) {
        live.valueMap.push(&v);
    }
}

/*
 * Implementation of 'Computing Liveness Sets for SSA-Form Programs'
 * by Florian Brandner, Benoit Boissinot, Alain Darte, Benoît Dupont de Dinechin, Fabrice Rastello.
 */

template<bool isSmall>
static void processUpwards(LowerBase base, Liveness& live, LiveSet& blockSet, LowerBlock* b, LowerValue* v) {
    // If `v` is defined in a non-phi node in the block, we don't have to propagate upwards.
    // Phi nodes are always live-in, since they represent an incoming value from a predecessor block.
    auto inst = v->inst();
    auto valueBlock = base[inst->block];
    auto phi = isPhi(inst);

    if(valueBlock == b && !phi) return;

    auto liveId = v->liveId();
    assertTrue(liveId != kNullLive);

    // Mark the value as live-in. If it was already marked, we are done.
    if(blockSet.liveIn.get<isSmall>(liveId)) return;
    blockSet.liveIn.set<isSmall>(liveId);

    // Do not propagate local phi definitions upward.
    // Values created by a local phi are live-in in the block; values used by an external phi are live-out.
    if(valueBlock == b && phi) return;

    for(auto offset: b->incoming.contents(base)) {
        auto p = base[offset];
        auto l = live.getBlock(p);

        l->liveOut.set<isSmall>(liveId);
        processUpwards<isSmall>(base, live, *l, p, v);
    }
}

template<bool isSmall>
static void processInst(LowerBase base, Liveness& live, LiveSet& blockSet, LowerBlock* block, LowerInst* inst) {
    for(auto offset: inst->used()) {
        auto u = base[offset];
        if(u->flags & LowerValue::Implicit) continue;

        processUpwards<isSmall>(base, live, blockSet, block, u);
    }
}

template<bool isSmall, class Container>
static void processBlocks(LowerBase base, Liveness& live, Container blockList) {
    // Process each instruction that can potentially consume values,
    // and forward liveness information along any usage paths.
    // Do not process phi nodes here - they don't consume values by themselves.
    // The result of the phi will be marked as live if it is used anywhere,
    // while the values used by any phi nodes are processed separately.
    for(auto blockOffset: blockList) {
        auto b = base[blockOffset];
        auto set = live.getBlock(b);

        // Mark uses by phis in outgoing blocks.
        for(auto s: b->outgoing) {
            if(!s) continue;

            for(auto offset: base[s]->phis.contents(base)) {
                auto phi = base[offset];
                auto values = phi->used();
                auto blocks = phi->sources();

                for(Size i = 0; i < values.length; i++) {
                    if(base[blocks.ptr[i]] == b) {
                        auto v = base[values.ptr[i]];
                        if(v->flags & LowerValue::Implicit) continue;

                        auto liveId = v->liveId();
                        assertTrue(liveId != kNullLive);

                        set->liveOut.template set<isSmall>(liveId);
                        processUpwards<isSmall>(base, live, *set, b, v);
                    }
                }
            }
        }

        // Mark uses by instructions in the block.
        for(auto i: b->instructions.contents(base)) {
            processInst<isSmall>(base, live, *set, b, base[i]);
        }

        processInst<isSmall>(base, live, *set, b, base[b->terminator]);
    }
}

/*
 * Live ranges.
 *
 * The liveness sets above answer "is this value live at this block boundary". The register
 * allocator needs the stronger "over which stretch of the program does this value need a register",
 * so that it can hand a value one register for its whole lifetime instead of tracking remaining
 * use counts as it walks. That stretch is derived here, once, from the sets plus a single linear
 * numbering of the instructions.
 *
 * The numbering follows LowerFunction::blocks in order, so any consumer that walks the blocks in
 * the same order sees exactly these indices without having to store them per instruction. Callers
 * that want tight ranges should put the block list in reverse postorder first (see orderBlocks in
 * codegen/x64/transform.cpp); the ranges stay correct in any order, just wider.
 */

static void extendRange(Liveness& live, LowerValue* v, U32 index) {
    if(v->flags & LowerValue::Implicit) return;

    auto id = v->liveId();
    if(id == kNullLive) return;

    live.ranges[id].extend(index);
}

static void buildRanges(LowerBase base, LowerFunction& fun, Liveness& live) {
    for(Size i = 0; i < live.valueMap.size(); i++) live.ranges.push(LiveRange {});

    auto blockList = fun.blocks.contents(base);

    // Arguments occupy their incoming registers from the moment the function is entered, before
    // any instruction has run, so their ranges have to start at the very first index rather than
    // at whatever block first reports them live-in.
    for(auto a: fun.args.contents(base)) {
        extendRange(live, &base[a]->result, 0);
    }

    U32 index = 0;

    for(auto offset: blockList) {
        auto b = base[offset];
        auto set = live.getBlock(b);
        set->firstIndex = index;

        // A phi is conceptually defined at the top of its block, before the first instruction.
        for(auto p: b->phis.contents(base)) {
            extendRange(live, &base[p]->result, index);
        }

        for(auto i: b->instructions.contents(base)) {
            auto inst = base[i];

            for(auto u: inst->used()) extendRange(live, base[u], index);
            for(auto& v: inst->created()) extendRange(live, &v, index);

            index++;
        }

        for(auto u: base[b->terminator]->used()) extendRange(live, base[u], index);

        set->lastIndex = index;
        index++;
    }

    live.instCount = index;

    // A value that is live at a block boundary has to hold its register across that boundary, even
    // where the block itself neither defines nor uses it.
    for(auto offset: blockList) {
        auto set = live.getBlock(base[offset]);

        set->liveIn.iterate(set->valueCount, [&](LiveId id) {
            live.ranges[id].extend(set->firstIndex);
        });

        set->liveOut.iterate(set->valueCount, [&](LiveId id) {
            live.ranges[id].extend(set->lastIndex);
        });
    }

    // A phi's register is written by a move at the end of *every* predecessor, including back-edge
    // predecessors that are numbered long after the phi's own block. Its range has to reach those
    // move sites, or something else could be handed its register in between and be overwritten.
    for(auto offset: blockList) {
        auto b = base[offset];

        for(auto p: b->phis.contents(base)) {
            auto& result = base[p]->result;
            if(result.flags & LowerValue::Implicit) continue;

            auto id = result.liveId();
            if(id == kNullLive) continue;

            for(auto in: b->incoming.contents(base)) {
                live.ranges[id].extend(live.getBlock(base[in])->lastIndex);
            }
        }
    }
}

Ptr<Liveness> LowerFunction::buildLiveness(LowerBase base) {
    Ptr<Liveness> live(new Liveness { arena });

    // Number every value in the function. Phis come first within a block, matching the order
    // buildRanges walks them in.
    auto blockList = blocks.contents(base);

    for(auto a: args.contents(base)) {
        addInst(*live, base[a]);
    }

    for(auto offset: blockList) {
        auto b = base[offset];

        // No need to process the terminator, as it never produces any value.
        for(auto i: b->phis.contents(base)) {
            addInst(*live, base[i]);
        }

        for(auto i: b->instructions.contents(base)) {
            addInst(*live, base[i]);
        }
    }

    // Now we know the maximum number of values that could be live between blocks,
    // so we can allocate a static bitset for each block.
    auto setSize = live->valueMap.size();
    live->allocateBlocks(arena, blockList.size(), setSize);

    // Choose between two implementations based on the number of live values.
    // If the number is small, we can avoid allocating separate sets.
    if(EmbedSet::isSmall(setSize)) {
        processBlocks<true>(base, *live, blockList);
    } else {
        processBlocks<false>(base, *live, blockList);
    }

    buildRanges(base, *this, *live);

    return ::move(live);
}
