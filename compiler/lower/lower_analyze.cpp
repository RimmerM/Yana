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

static bool canContribute(LowerBase base, LowerInst* inst) {
    if(isPhi(inst)) return true;
    auto b = inst->block;

    for(auto v: inst->created()) {
        if(v.flags & LowerValue::Implicit) continue;

        for(auto offset: v.uses.contents(base)) {
            auto use = base[offset];

            // If any use of the value is in a different block or a phi in the same block,
            // it can escape the current block.
            if(use->block != b || isPhi(use)) return true;
        }
    }

    return false;
}

static void addInst(Liveness& live, LowerInst* inst) {
    assertTrue(live.valueMap.size() < kNullLive);
    inst->liveId = live.valueMap.size();

    for(auto& v: inst->created()) {
        live.valueMap.push(&v);
    }
}

static void checkInst(LowerBase base, Liveness& live, LowerInst* inst) {
    if(canContribute(base, inst)) {
        addInst(live, inst);
    } else {
        assertTrue(inst->liveId == kNullLive);
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

Ptr<Liveness> LowerFunction::buildLiveness(LowerBase base) {
    Ptr<Liveness> live(new Liveness { arena });

    // Generate the list of values that contribute to liveness.
    // Values only contribute if they are used outside (or in a phi in) the block they are created in.
    auto blockList = blocks.contents(base);

    for(auto a: args.contents(base)) {
        checkInst(base, *live, base[a]);
    }

    for(auto offset: blockList) {
        auto b = base[offset];

        // Phis always potentially contribute to inter-block liveness.
        // No need to process the terminator, as it never produces any value.
        for(auto i: b->phis.contents(base)) {
            addInst(*live, base[i]);
        }

        for(auto i: b->instructions.contents(base)) {
            checkInst(base, *live, base[i]);
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

    return ::move(live);
}
