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
        for(auto def: effects.defs) live[def] = 0;
        for(auto use: effects.uses) live[use] = 1;
        for(auto overwritten: effects.overwrites) live[overwritten] = 1;
    }
}

void computeLiveness(Analysis& analysis) {
    auto count = analysis.localCount;
    auto blocks = analysis.blockCount();

    for(Size i = 0; i < blocks; i++) {
        analysis.liveIn.push(emptySet(count));
        analysis.liveOut.push(emptySet(count));
    }

    auto changed = true;
    while(changed) {
        changed = false;

        for(Size i = blocks; i > 0; i--) {
            auto index = i - 1;
            auto block = analysis.blockAt(index);
            auto live = emptySet(count);

            for(auto successor: block->outgoing) {
                if(!successor) continue;

                auto& successorIn = analysis.liveIn[analysis.local[successor]->index];
                for(Size l = 0; l < count; l++) live[l] |= successorIn[l];
            }

            for(Size l = 0; l < count; l++) {
                if(live[l] != analysis.liveOut[index][l]) changed = true;
            }

            analysis.liveOut[index] = live;

            auto range = analysis.blockRanges[index];
            applyBackward(analysis, range.first, range.end, live);

            for(Size l = 0; l < count; l++) {
                if(live[l] != analysis.liveIn[index][l]) changed = true;
            }

            analysis.liveIn[index] = live;
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

    for(Size l = 0; l < count; l++) {
        Array<U8> occupied;
        for(Size i = 0; i < analysis.instructionCount; i++) occupied.push(0);

        for(Size b = 0; b < analysis.blockCount(); b++) {
            auto range = analysis.blockRanges[b];
            if(range.end == range.first) continue;

            // Replay the backward walk to recover liveness at each point inside the block, which
            // the fixpoint only kept at the two ends.
            Array<U8> before;
            auto live = analysis.liveOut[b];

            for(Size i = range.end; i > range.first; i--) {
                auto& effects = analysis.effects[i - 1];
                for(auto def: effects.defs) live[def] = 0;
                for(auto use: effects.uses) live[use] = 1;
                for(auto overwritten: effects.overwrites) live[overwritten] = 1;
                before.push(live[l]);
            }

            for(Size i = range.first; i < range.end; i++) {
                auto liveBefore = before[range.end - 1 - i];
                auto defines = false;
                for(auto def: analysis.effects[i].defs) defines = defines || def == l;
                for(auto init: analysis.effects[i].inits) defines = defines || init == l;

                occupied[i] = liveBefore || defines;
            }
        }

        result.rangeOffsets.push(U32(result.ranges.size()));
        auto emitted = 0u;
        auto open = maxLimit<U32>;

        for(Size i = 0; i <= analysis.instructionCount; i++) {
            auto live = i < analysis.instructionCount && occupied[i];

            if(live && open == maxLimit<U32>) {
                open = U32(i);
            } else if(!live && open != maxLimit<U32>) {
                result.ranges.push(LiveRange { open, U32(i) });
                open = maxLimit<U32>;
                emitted++;
            }
        }

        result.rangeCounts.push(emitted);
    }
}
