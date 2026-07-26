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

/*
 * Loops.
 *
 * A back edge is one to a block the depth-first walk is still inside, and the loop it closes is the
 * natural loop of that edge: the header, plus everything the latch is reachable from without passing
 * back through the header. Two back edges to one header describe one loop with two latches rather
 * than two loops, so they are grouped by header and their bodies unioned - a block in such a loop is
 * one loop deep, not two, and runs once per iteration like every other block in it.
 *
 * The result is the nesting rather than a count: which loop is innermost for each block, and which
 * loop each loop sits in. That is what lets an edge be classified as leaving a loop, which is the
 * only static estimate of a branch's probability worth making (see edgeWeightsOf).
 */

struct BackEdge {
    LowerBlock* latch;
    LowerBlock* header;
};

static constexpr U32 kOnStack = 1;
static constexpr U32 kFinished = 2;

// An edge to a block still on the walk's own stack is one that closes a cycle, which is the whole of
// what makes it a back edge. Blocks the entry cannot reach are never visited and belong to no loop.
static void findBackEdges(LowerBase base, LowerBlock* b, Array<BackEdge>& out) {
    b->marker = kOnStack;

    for(auto s: b->outgoing) {
        if(!s) continue;
        auto succ = base[s];

        if(succ->marker == kOnStack) out.push(BackEdge { b, succ });
        else if(succ->marker != kFinished) findBackEdges(base, succ, out);
    }

    b->marker = kFinished;
}

// One loop while its body is being collected, before the loops are ordered and the nesting read off.
struct LoopBody {
    BlockIndex header = kNullBlock;
    Array<bool> members;
    Size size = 0;
};

// Adds one back edge's natural loop to `loop`: everything the latch is reachable from backwards,
// stopping at the header, which is marked before the walk starts precisely so that it bounds it -
// a predecessor reached through the header is outside the loop.
static void addLatch(LowerBase base, LoopBody& loop, LowerBlock* latch) {
    Array<LowerBlock*> pending;

    auto visit = [&](LowerBlock* b) {
        if(loop.members[b->index]) return;

        loop.members[b->index] = true;
        loop.size++;
        pending.push(b);
    };

    visit(latch);

    // Walked as a queue rather than popped, so that `pending` doubles as the visited list.
    for(Size i = 0; i < pending.size(); i++) {
        for(auto p: pending[i]->incoming.contents(base)) visit(base[p]);
    }
}

bool LoopInfo::contains(BlockIndex loop, BlockIndex block) const {
    auto h = header[block];

    // Bounded rather than run to the end of the chain: `parent` points strictly outward for the
    // properly nested loops a reducible CFG produces, and an irreducible one is the single shape
    // that could close the chain into a cycle. A bound costs nothing and beats proving it cannot.
    for(Size step = 0; h != kNullBlock && step <= header.size(); step++) {
        if(h == loop) return true;
        h = parent[h];
    }

    return false;
}

LoopInfo LowerFunction::buildLoops(LowerBase base) {
    auto blockList = blocks.contents(base);

    LoopInfo info;
    for(Size i = 0; i < blockList.size(); i++) {
        auto b = base[blockList[i]];
        b->index = BlockIndex(i);
        b->marker = 0;

        info.header.push(kNullBlock);
        info.parent.push(kNullBlock);
        info.depth.push(0);
    }

    Array<BackEdge> backEdges;
    findBackEdges(base, base[blocks.get(base, 0)], backEdges);

    // One loop per header, its body the union of the natural loops of every back edge to it.
    Array<LoopBody> loops;

    for(auto& edge: backEdges) {
        Size found = loops.size();
        for(Size i = 0; i < loops.size(); i++) {
            if(loops[i].header == edge.header->index) { found = i; break; }
        }

        if(found == loops.size()) {
            LoopBody loop;
            loop.header = edge.header->index;
            for(Size i = 0; i < blockList.size(); i++) loop.members.push(false);

            loop.members[loop.header] = true;
            loop.size = 1;
            loops.push(::move(loop));
        }

        addLatch(base, loops[found], edge.latch);
    }

    // Smallest first, so that the first loop to claim a block is the innermost one containing it and
    // the first loop to contain a header is that loop's parent. Natural loops are nested or
    // disjoint, so smaller means inner wherever two of them share a block at all.
    Array<Size> order;
    for(Size i = 0; i < loops.size(); i++) order.push(i);

    for(Size i = 1; i < order.size(); i++) {
        auto v = order[i];
        auto j = i;

        while(j > 0 && loops[order[j - 1]].size > loops[v].size) {
            order[j] = order[j - 1];
            j--;
        }

        order[j] = v;
    }

    // Every header heads its own loop, whatever else contains it. That identity is what the chain
    // walks terminate on, so it is established before anything else can claim the block.
    for(auto& loop: loops) info.header[loop.header] = loop.header;

    for(auto o: order) {
        auto& loop = loops[o];

        for(Size b = 0; b < blockList.size(); b++) {
            if(!loop.members[b]) continue;

            if(info.header[b] == kNullBlock) info.header[b] = loop.header;

            // The innermost loop containing a header, other than the one it heads, is its parent.
            if(BlockIndex(b) != loop.header && info.isHeader(BlockIndex(b)) && info.parent[b] == kNullBlock) {
                info.parent[b] = loop.header;
            }
        }
    }

    for(Size b = 0; b < blockList.size(); b++) {
        U16 d = 0;
        auto h = info.header[b];

        for(Size step = 0; h != kNullBlock && step <= loops.size(); step++) {
            d++;
            h = info.parent[h];
        }

        info.depth[b] = d;
        base[blockList[b]]->loopDepth = d;
    }

    return info;
}

/*
 * Edge likelihood.
 *
 * What the IR says wins, because it is the only thing that can know: a branch that tests a value the
 * program computed looks exactly like one that tests whether an allocation failed, and only whoever
 * wrote it can tell the two apart. Where nothing is stated, exactly one thing about a branch is
 * derivable from the CFG - that a loop runs more than once, so the edge leaving it is taken once
 * where the edge staying in it is taken every iteration but the last.
 *
 * Nothing else is guessed. An ordinary data-dependent branch is left at even odds rather than being
 * declared exceptional on the strength of what its arms happen to contain, since the IR has already
 * lost whatever made it exceptional. The other static sources the design names - a call that does not
 * return, an edge into unreachable code - have nothing to read yet: this IR has no way to write
 * either down, and a rule keyed on something unrepresentable would be dead code claiming coverage.
 */

// Weights are relative, so only their ratio matters - but the product below has to stay inside 64
// bits at the frequency ceiling, which is what bounds them.
static U32 clampWeight(U32 weight) {
    if(weight < 1) return 1;
    return weight > kMaxEdgeWeight ? kMaxEdgeWeight : weight;
}

EdgeWeights edgeWeightsOf(LowerBase base, const LoopInfo& loops, LowerBlock* block) {
    // A block with one successor has nothing to weigh: everything reaching it continues there.
    if(!block->outgoing[0] || !block->outgoing[1]) return EdgeWeights {};

    auto term = base[block->terminator];
    assertTrue(term->kind == LowerInst::Je); // the only terminator with two successors

    auto je = (LowerInstJe*)term;

    if(je->hasLikelihood()) {
        auto& left = je->likelihood[0];
        auto& right = je->likelihood[1];

        // The better-informed of the two sources describes the pair: a profile that measured one arm
        // measured the branch, and the sibling's default of one is what it measured it against.
        return EdgeWeights {
            { clampWeight(left.weight), clampWeight(right.weight) },
            left.source > right.source ? left.source : right.source,
        };
    }

    auto loop = loops.header[block->index];

    if(loop != kNullBlock) {
        auto stays0 = loops.contains(loop, base[block->outgoing[0]]->index);
        auto stays1 = loops.contains(loop, base[block->outgoing[1]]->index);

        // Only when exactly one of them leaves. A branch whose arms both stay in the loop says
        // nothing about how long the loop runs, and neither does one whose arms both leave it.
        if(stays0 != stays1) {
            EdgeWeights out;
            out.source = LikelihoodSource::Static;
            out.weight[stays0 ? 0 : 1] = U32(kLoopTripCount - 1);
            out.weight[stays0 ? 1 : 0] = 1;

            return out;
        }
    }

    return EdgeWeights {};
}

/*
 * Block frequency.
 *
 * One walk in reverse postorder, with the back edges left out of it: every other edge into a block
 * comes from a block the walk has already answered, so the sum is complete when it is taken and no
 * fixpoint is needed. What the back edges would have contributed is the loop's own multiplier, which
 * the header is scaled by instead.
 *
 * The two halves are chosen to agree rather than to be independently plausible. A loop whose exit
 * edge has probability 1/T satisfies H = entry + H(1 - 1/T), which is H = entry * T - exactly what
 * scaling the header by T produces. So the exit block comes out at the frequency of the entry rather
 * than at T times it, and a loop that iterates eight times is left eight times as hot as the code
 * around it without the code it leaves to becoming hot as well. Both numbers come from
 * kLoopTripCount, which is why they cannot drift apart.
 *
 * They agree exactly for a loop with one exit. A loop with several - a search with both a "found"
 * and a "ran out" ending - gives each of them 1/T of wherever it sits, so the exits together account
 * for more than one departure per entry, and the loop is really being modelled as running rather
 * fewer than T times. Making that exact means solving for the trip count the exit structure implies
 * rather than assuming one, which is a fixpoint over each loop; nothing that reads the result needs
 * it, because every consumer compares blocks and the ranking is the same either way.
 */

// What one edge carries: the source block's frequency, split between its successors in proportion to
// their weights.
static U64 edgeFrequency(LowerBase base, const LoopInfo& loops, const FunctionFrequencyInfo& freq,
    LowerBlock* from, LowerBlock* to)
{
    auto source = freq.relativeBlockFrequency[from->index];
    if(!from->outgoing[1]) return source;

    auto weights = edgeWeightsOf(base, loops, from);
    auto edge = base[from->outgoing[0]] == to ? 0 : 1;

    return source * weights.weight[edge] / weights.total();
}

FunctionFrequencyInfo LowerFunction::buildFrequencies(LowerBase base) {
    auto loops = buildLoops(base);
    auto postorder = buildPostorder(base);
    auto blockList = blocks.contents(base);

    FunctionFrequencyInfo out;
    for(Size i = 0; i < blockList.size(); i++) out.relativeBlockFrequency.push(0);

    for(Size i = postorder.size(); i > 0; i--) {
        auto index = postorder[i - 1];
        auto b = base[blockList[index]];

        // Execution arrives at the entry block from outside the function; nothing branches to it.
        U64 total = index == 0 ? kEntryFrequency : 0;

        for(auto p: b->incoming.contents(base)) {
            auto pred = base[p];
            if(loops.isBackEdge(pred->index, b->index)) continue;

            total += edgeFrequency(base, loops, out, pred, b);
        }

        if(loops.isHeader(index)) total *= kLoopTripCount;
        if(total > kMaxFrequency) total = kMaxFrequency;

        // A block the walk reached does execute, however rarely. Rounding it to nothing would make
        // it indistinguishable from one nothing reaches at all, which is the one distinction a
        // consumer weighing spill costs must not lose.
        if(total == 0) total = 1;

        out.relativeBlockFrequency[index] = total;
    }

    return out;
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
