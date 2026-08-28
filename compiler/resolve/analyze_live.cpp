#include "analyze_pass.h"

/*
 * Liveness - the ordinary backward fixpoint.
 *
 * A local is live at a point when some path from there reaches a use of it before the next write
 * of the whole slot. Blocks are walked in reverse per round, which settles in one round for
 * straight-line code and in as many rounds as the loop nest is deep for anything else.
 */

void applyBackward(Analysis& analysis, Size first, Size end, LocalSet& live) {
    for(Size i = end; i > first; i--) {
        auto& effects = analysis.effects[i - 1];

        // Defs before uses: an instruction that both writes and reads a slot leaves it live above.
        // An overwrite is both at once - it ends the old value's range and is the point that range
        // has to reach - which is why it is applied on the reading side.
        for(auto def: effects.defs) live.set(def, false);
        for(auto use: effects.uses) live.set(use, true);
        for(auto overwritten: effects.overwrites) live.set(overwritten, true);
        for(auto joined: effects.joins) live.set(joined, true);
    }
}

void computeLiveness(Analysis& analysis) {
    auto count = analysis.localCount;
    auto blocks = analysis.blockCount();

    analysis.liveIn.reset(blocks, count);
    analysis.liveOut.reset(blocks, count);

    // The set one block's walk carries, cleared per block rather than built per block: this is the
    // innermost thing the fixpoint does, and it used to be an allocation each time round.
    auto& live = analysis.scratch.work;

    auto changed = true;
    while(changed) {
        changed = false;

        for(Size i = blocks; i > 0; i--) {
            auto index = i - 1;
            auto block = analysis.blockAt(index);
            live.reset(count);

            for(auto successor: block->successors()) {
                if(!successor) continue;
                live.unionWith(analysis.liveIn[analysis.local[successor]->index]);
            }

            if(!live.equals(analysis.liveOut[index])) {
                analysis.liveOut[index].copyFrom(live);
                changed = true;
            }

            auto range = analysis.blockRanges[index];
            applyBackward(analysis, range.first, range.end, live);

            if(!live.equals(analysis.liveIn[index])) {
                analysis.liveIn[index].copyFrom(live);
                changed = true;
            }
        }
    }
}

/*
 * Live ranges, for the printed form.
 *
 * A local is occupied at a point when it holds something reachable there: either the backward
 * liveness says a use is still ahead, or this is the instruction that gave it a value. Coalescing
 * the runs of occupied indices is what turns a per-point answer into the ranges-with-holes shape
 * the header describes.
 */
void buildRanges(Analysis& analysis, OwnershipResult& result) {
    auto count = analysis.localCount;

    // One offset per local plus a terminator, so that a local's ranges are the stretch between its
    // own offset and the next - see OwnershipResult.
    result.rangeStart.clear();
    result.ranges.clear();

    // Both sets are over instruction indices rather than over locals, and both are cleared per
    // local rather than built per local - the loop below is per local per block, and each of these
    // used to be an allocation inside it.
    auto& occupied = analysis.scratch.occupied;
    auto& before = analysis.scratch.positions;
    auto& live = analysis.scratch.work;

    for(Size l = 0; l < count; l++) {
        occupied.reset(analysis.instructionCount);

        for(Size b = 0; b < analysis.blockCount(); b++) {
            auto range = analysis.blockRanges[b];
            if(range.end == range.first) continue;

            // Replay the backward walk to recover liveness at each point inside the block, which
            // the fixpoint only kept at the two ends.
            before.reset(range.end - range.first);
            live.copyFrom(analysis.liveOut[b]);

            for(Size i = range.end; i > range.first; i--) {
                auto& effects = analysis.effects[i - 1];
                for(auto def: effects.defs) live.set(def, false);
                for(auto use: effects.uses) live.set(use, true);
                for(auto overwritten: effects.overwrites) live.set(overwritten, true);
                for(auto joined: effects.joins) live.set(joined, true);
                before.set(range.end - i, live[l]);
            }

            for(Size i = range.first; i < range.end; i++) {
                auto liveBefore = before[range.end - 1 - i];
                auto defines = false;
                for(auto def: analysis.effects[i].defs) defines = defines || def == l;
                for(auto init: analysis.effects[i].inits) defines = defines || init == l;

                occupied.set(i, liveBefore || defines);
            }
        }

        result.rangeStart.push(U32(result.ranges.size()));
        auto open = maxLimit<U32>;

        for(Size i = 0; i <= analysis.instructionCount; i++) {
            auto live = i < analysis.instructionCount && occupied[i];

            if(live && open == maxLimit<U32>) {
                open = U32(i);
            } else if(!live && open != maxLimit<U32>) {
                result.ranges.push(LiveRange { open, U32(i) });
                open = maxLimit<U32>;
            }
        }
    }

    // The terminator, which is what makes the last local's stretch readable like every other's.
    result.rangeStart.push(U32(result.ranges.size()));
}
