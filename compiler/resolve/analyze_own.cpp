#include "analyze_pass.h"

/*
 * Ownership state - the forward companion.
 *
 * Uninitialized and Moved both mean "owns nothing" and are kept apart only so that a use of one
 * reads as a different mistake from a use of the other. Maybe is the join of anything with
 * anything else, and is the state a drop flag exists to resolve at run time.
 */

static OwnState joinState(OwnState a, OwnState b) {
    if(a == b) return a;

    // Two ways of owning nothing join to the one that produces the better diagnostic.
    auto emptyA = a == OwnState::Uninitialized || a == OwnState::Moved;
    auto emptyB = b == OwnState::Uninitialized || b == OwnState::Moved;
    if(emptyA && emptyB) return OwnState::Moved;

    return OwnState::Maybe;
}

// Everything one instruction does to the ownership state, in place.
static void transferState(Analysis& analysis, Size index, Array<OwnState>& states) {
    auto& effects = analysis.effects[index];

    // Moves before inits: `x = consume(x)` moves out of the slot and then fills it again, and the
    // state that survives is the second of the two.
    for(auto moved: effects.moves) states[moved] = OwnState::Moved;

    auto& instruction = *analysis.local[analysis.order[index]];

    /*
     * Fresh storage owns nothing, and owns nothing *again* on the next pass through a loop.
     *
     * An allocation defines the slot without initializing it, so the two lists split as they do -
     * but the state left behind by whatever the slot held before still has to go, or a slot
     * allocated inside a loop body meets its own previous iteration's Moved on the back edge and
     * joins to Maybe. That reads as "may have been moved out of" at the next use, which is a
     * diagnostic about storage that did not exist yet. `let x = e` inside a `while` is the
     * everyday case, and a closure built per iteration - a lambda, or the continuation of a lens
     * call in a loop body - is the one that has no name to point at.
     */
    if(instruction.kind == Value::Alloc) {
        for(auto def: effects.defs) states[def] = OwnState::Uninitialized;
    }

    for(auto init: effects.inits) states[init] = OwnState::Owned;

    /*
     * A drop ends the life of what it names, and *what it names* is the whole of the rule.
     *
     * Only an unprojected drop empties the slot. A drop of `x.f` is one member's teardown, and this
     * lattice is per local - so marking the root would say the whole of `x` owns nothing, which is
     * both wrong and unrepresentable here: the honest state after a partial teardown is one this
     * pass does not have, and the conservative reading of a state it cannot represent is to leave
     * the slot as it was. The whole-slot drop that follows is what actually empties it.
     *
     * It was unrestricted for as long as no projected drop was ever rooted in a local. Derived
     * teardown glue is what made one: its members used to hang off a `%T` parameter, where
     * `rootLocal` answers nothing, and they now hang off the `->` parameter's own slot - so
     * `drop$Pair` dropped `value.left`, marked all of `value` moved, and reported `value.right` as
     * a use after a move. The other projected drop is the one a write owes for what it replaces
     * (makeOverwriteDrop), which survived only because the write immediately after it put the slot
     * back to Owned.
     */
    if(instruction.kind == Value::Drop) {
        auto& place = ((InstDrop&)instruction).place;
        auto root = rootLocal(analysis, place);
        if(root != maxLimit<U32> && place.projections.isEmpty()) states[root] = OwnState::Moved;
    }
}

void computeOwnership(Analysis& analysis) {
    auto count = analysis.localCount;
    auto blocks = analysis.blockCount();

    // Both rows and the walk's own carrier are the program's storage rather than this function's
    // - see AnalysisScratch. The walk assigns whole rows around, and each of those used to be an
    // allocation per block popped off the worklist.
    auto& entry = analysis.scratch.blockEntry;
    auto& reached = analysis.scratch.work;

    entry.reset(blocks, count, OwnState::Uninitialized);
    reached.reset(blocks);

    // The row the walk carries between blocks, emptied per block popped rather than copied out of
    // `entry` - see AnalysisScratch.
    auto& states = analysis.scratch.walkState;

    // A parameter's slot arrives already holding the caller's value. It is not owned here - see
    // TrackedLocal::owned - but it is initialized, and saying so is what keeps a read of a
    // parameter from reading as a use of storage something moved out of.
    for(Size l = 0; l < count; l++) {
        auto slot = analysis.function.localAt(analysis.local, U32(l));
        if(slot.value && analysis.local[slot.value]->kind == Value::Arg) entry[0][l] = OwnState::Owned;
    }

    reached.set(0, true);
    SmallArray<Size, 32> worklist;
    worklist.push(0);

    while(worklist.size()) {
        auto index = worklist.pop().unwrap();
        auto block = analysis.blockAt(index);
        auto range = analysis.blockRanges[index];

        states.clear();
        for(auto state: entry[index]) states.push(state);

        for(Size i = range.first; i < range.end; i++) {
            analysis.stateBefore.copyInto(i, states);
            transferState(analysis, i, states);
        }

        for(auto successor: block->successors()) {
            if(!successor) continue;

            auto successorIndex = analysis.local[successor]->index;
            auto updated = false;

            // An unreached successor takes this state outright. Joining into it instead would meet
            // every owned local with the all-Uninitialized bottom and turn the lot into Maybe,
            // which is the classic way to get a dataflow analysis that answers "it depends" to
            // every question.
            if(!reached[successorIndex]) {
                entry.copyInto(successorIndex, states);
                reached.set(successorIndex, true);
                updated = true;
            } else {
                for(Size l = 0; l < count; l++) {
                    auto joined = joinState(entry[successorIndex][l], states[l]);
                    if(joined == entry[successorIndex][l]) continue;

                    entry[successorIndex][l] = joined;
                    updated = true;
                }
            }

            if(updated) worklist.push(successorIndex);
        }
    }
}
