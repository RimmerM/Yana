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
 * Live intervals.
 *
 * The liveness sets above answer "is this value live at this block boundary". The register allocator
 * needs the stronger "over which stretches of the program does this value need a location", so that
 * two values can share one register exactly when they are never live at the same time. Those
 * stretches are derived here, once, from the sets plus a single linear numbering of the
 * instructions.
 *
 * The numbering follows LowerFunction::blocks in order, so any consumer that walks the blocks the
 * same way sees exactly these indices without having to store them per instruction.
 *
 * Ranges are built one block at a time and are therefore exact, holes and all: a value contributes a
 * range to a block only where it is genuinely live in it. Block order affects how *compact* the
 * result is - a loop whose blocks are contiguous yields one range where a scattered one yields
 * several - but never whether it is correct.
 */

// One block's worth of one value's liveness, before the ranges are packed per value.
struct RangeEntry {
    LiveId id;
    Range range;
};

static constexpr U32 kNotSeen = maxLimit<U32>;

static void addRange(Array<RangeEntry>& entries, LowerValue* v, U32 from, U32 to) {
    if(v->flags & LowerValue::Implicit) return;

    auto id = v->liveId();
    if(id == kNullLive) return;

    entries.push(RangeEntry { id, Range { from, to } });
}

// Packs the per-block entries into one arena array with a span per value, sorted and with ranges
// that meet merged into one.
//
// A counting sort by value id: the total is known up front, so every value gets a contiguous run
// and nothing is copied twice. Within a value the entries are almost sorted already - blocks are
// numbered in order - so the per-value pass is an insertion sort, which is the fastest thing there
// is for the one or two ranges a typical value ends up with.
static void packRanges(Liveness& live, Array<RangeEntry>& entries, Size valueCount) {
    Array<U32> start;
    Array<U32> cursor;

    for(Size i = 0; i < valueCount; i++) { start.push(0); cursor.push(0); }
    for(auto& e: entries) start[e.id]++;

    U32 total = 0;
    for(Size i = 0; i < valueCount; i++) {
        auto count = start[i];
        start[i] = total;
        cursor[i] = total;
        total += count;
    }

    Array<Range> sorted;
    for(U32 i = 0; i < total; i++) sorted.push(Range {});
    for(auto& e: entries) sorted[cursor[e.id]++] = e.range;

    for(Size id = 0; id < valueCount; id++) {
        auto begin = start[id];
        auto count = cursor[id] - begin;

        for(U32 i = 1; i < count; i++) {
            auto range = sorted[begin + i];
            auto j = i;

            while(j > 0 && sorted[begin + j - 1].from > range.from) {
                sorted[begin + j] = sorted[begin + j - 1];
                j--;
            }

            sorted[begin + j] = range;
        }

        RangeSpan span { U32(live.rangeStore.size()), 0 };

        for(U32 i = 0; i < count; i++) {
            auto range = sorted[begin + i];

            // Touching counts as overlapping: [0, 4) and [4, 9) describe one unbroken stretch, and
            // leaving them apart would let something else be given the location at index 4.
            if(span.count > 0) {
                auto& last = live.rangeStore[live.rangeStore.size() - 1];

                if(range.from <= last.to) {
                    if(range.to > last.to) last.to = range.to;
                    continue;
                }
            }

            live.rangeStore.push(range);
            span.count++;
        }

        live.spans.push(span);
    }
}

static void buildRanges(LowerBase base, LowerFunction& fun, Liveness& live) {
    auto blockList = fun.blocks.contents(base);
    auto valueCount = live.valueMap.size();

    // The linear numbering comes first: every range below is stated in it. A block owns the indices
    // of its instructions followed by one for its terminator.
    U32 index = 0;

    for(auto offset: blockList) {
        auto set = live.getBlock(base[offset]);

        set->firstIndex = index;
        index += U32(base[offset]->instructions.size());
        set->lastIndex = index;
        index++;
    }

    live.instCount = index;

    Array<RangeEntry> entries;

    // Where the block being walked defines a value and where it last reads it. Allocated once for
    // the function and reset per block through `touched`, so the cost follows the number of values
    // a block actually mentions rather than the number the function has.
    Array<U32> definedAt;
    Array<U32> lastUse;
    Array<LiveId> touched;

    for(Size i = 0; i < valueCount; i++) { definedAt.push(kNotSeen); lastUse.push(kNotSeen); }

    for(auto offset: blockList) {
        auto b = base[offset];
        auto set = live.getBlock(b);

        for(auto id: touched) { definedAt[id] = kNotSeen; lastUse[id] = kNotSeen; }
        touched.clear();

        auto note = [&](LowerValue* v, U32 at, bool definition) {
            if(v->flags & LowerValue::Implicit) return;

            auto id = v->liveId();
            if(id == kNullLive) return;

            if(lastUse[id] == kNotSeen && definedAt[id] == kNotSeen) touched.push(id);
            if(definition && definedAt[id] == kNotSeen) definedAt[id] = at;

            lastUse[id] = at;
        };

        auto at = set->firstIndex;

        // A phi is conceptually defined at the top of its block, before the first instruction. Its
        // operands belong to the edges rather than to this block, and are accounted for below.
        for(auto p: b->phis.contents(base)) note(&base[p]->result, at, true);

        for(auto i: b->instructions.contents(base)) {
            auto inst = base[i];

            for(auto u: inst->used()) note(base[u], at, false);
            for(auto& v: inst->created()) note(&v, at, true);

            at++;
        }

        for(auto u: base[b->terminator]->used()) note(base[u], at, false);

        // Where a value that leaves this block live stops needing its location.
        //
        // Being live-out is not by itself enough to carry it into the next block: a value that is
        // live-out only because a phi in a successor takes it from this edge is read by the copy at
        // this terminator and is finished there. Ending it at the terminator's `after` point - which
        // is exactly where that phi's own range begins - is what lets the two share a register and
        // makes the copy vanish. A value genuinely live in a successor runs one point further, to
        // where the next block in the numbering begins, so the two ranges join into one.
        auto liveOutEnd = [&](LiveId id) {
            for(auto s: b->outgoing) {
                if(!s) continue;

                auto succ = live.getBlock(base[s]);
                if(succ->liveIn.get(succ->valueCount, id)) return afterInst(set->lastIndex) + 1;
            }

            return afterInst(set->lastIndex);
        };

        // One range per value this block mentions: from where it becomes live here - its entry if
        // it arrives live, its definition otherwise - to where it stops being needed.
        for(auto id: touched) {
            auto liveIn = set->liveIn.get(set->valueCount, id);
            assertTrue(liveIn || definedAt[id] != kNotSeen); // a use with no definition reaching it

            auto from = liveIn ? beforeInst(set->firstIndex) : afterInst(definedAt[id]);
            auto to = set->liveOut.get(set->valueCount, id) ? liveOutEnd(id) : afterInst(lastUse[id]);

            entries.push(RangeEntry { id, Range { from, to } });
        }

        // A value that passes through the block without being mentioned in it still occupies its
        // location for the whole of it.
        set->liveIn.iterate(set->valueCount, [&](LiveId id) {
            if(lastUse[id] != kNotSeen || definedAt[id] != kNotSeen) return;
            if(!set->liveOut.get(set->valueCount, id)) return;

            entries.push(RangeEntry { id, Range { beforeInst(set->firstIndex), liveOutEnd(id) } });
        });
    }

    // Arguments occupy their incoming registers from the moment the function is entered, before any
    // instruction has run. They are defined outside every block, so no block reports them live-in
    // and the walk above never gives them a range in the entry block they flow out of.
    {
        auto entry = live.getBlock(base[fun.blocks.get(base, 0)]);

        for(auto a: fun.args.contents(base)) {
            auto& result = base[a]->result;
            if(result.flags & LowerValue::Implicit) continue;

            auto id = result.liveId();
            if(id == kNullLive) continue;

            auto to = entry->liveOut.get(entry->valueCount, id)
                ? afterInst(entry->lastIndex) + 1
                : beforeInst(0) + 1;

            entries.push(RangeEntry { id, Range { beforeInst(0), to } });
        }
    }

    // A phi's location is written by a move at the end of *every* predecessor, including back-edge
    // predecessors numbered long after the phi's own block, so it has to be live at those move
    // sites or something else could be handed its register in between.
    //
    // One point per edge, not a stretch reaching back to the phi. That is the whole difference holes
    // make here: the phi is live at the loop header and again at the latch, and dead in between, so
    // the value computed for the next iteration is free to use its register.
    //
    // It starts at the terminator's `after` point because the copy feeding it reads the incoming
    // value at `before` - the two ends of a copy, not two values needing the register at once.
    for(auto offset: blockList) {
        auto b = base[offset];

        for(auto p: b->phis.contents(base)) {
            for(auto in: b->incoming.contents(base)) {
                auto last = live.getBlock(base[in])->lastIndex;
                addRange(entries, &base[p]->result, afterInst(last), afterInst(last) + 1);
            }
        }
    }

    packRanges(live, entries, valueCount);
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
