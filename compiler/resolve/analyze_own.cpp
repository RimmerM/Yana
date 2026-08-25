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

/*
 * Borrowed storage, as a lattice of its own.
 *
 * The forward walk below is the one above with a different index. What is not the same is where the
 * rows come from: a local has a slot because the frame has one, and a borrow root has one only if
 * this pass can prove that two mentions of it are the same storage. Interning is that proof, and
 * everything it declines to intern stays refused exactly as it was.
 */

// The steps a path may take and still name storage inside the root it started in. The same line
// placeOverwriteDrops draws, and for the same reason: an Index or a Deref leaves the root behind,
// so what is on the other side is not something this frame's slot speaks for.
static bool pathStep(const Projection& projection, U32& step) {
    if(projection.kind == ProjectionKind::Field || projection.kind == ProjectionKind::Downcast) {
        step = projection.index;
        return true;
    }

    return false;
}

/*
 * The slot a borrow-rooted place names, interning one if this is the first mention of it.
 *
 * Four things are declined, and each of them is a case where a later mention could be different
 * storage from this one:
 *
 *  - a projected place, because a move out of part of a value is refused anyway (`wholeMove`);
 *  - a shared borrow, because it is not exclusive, so something else may be looking at the storage
 *    while this frame has emptied it - `&` is the capability that makes the window unobservable;
 *  - a borrow that is anything but a load of an access path: a call result, a phi, a select. Two
 *    of those in one body may or may not be the same storage and nothing here can say which;
 *  - a path stepping through an Index or a Deref, per pathStep.
 *
 * `find` is a linear scan. The array it scans is empty for every function that never moves out of a
 * borrow and one entry long for very nearly all of the rest - a body with two of these in it is a
 * fold over two accumulators - so an index on it would be a hash table to hold one element.
 */
static U32 internBorrowSlot(Analysis& analysis, const Place& place, bool create) {
    if(place.root != PlaceRoot::Borrow) return maxLimit<U32>;

    auto projections = place.projections;
    if(projections.isNotEmpty()) return maxLimit<U32>;

    auto borrow = place.pointer;
    if(!borrow) return maxLimit<U32>;

    auto borrowType = analysis.local[borrow]->type;
    if(!borrowType || !isBorrow(analysis.global, borrowType)) return maxLimit<U32>;

    auto& reference = *(BorrowType*)analysis.global[borrowType];
    if(!reference.mut || !reference.to) return maxLimit<U32>;

    auto& source = *analysis.local[borrow];
    if(source.kind != Value::LoadPlace) return maxLimit<U32>;

    /*
     * The path's own root, which is a local for a `&` binding this frame has a slot for and a
     * pointer for a captured one - see BorrowSlot::viaPointer. A global root is left out: nothing
     * can move out of one in the first place, so a slot for it would never be emptied.
     */
    auto& path = ((InstLoadPlace&)source).place;
    U32 root = maxLimit<U32>;
    auto viaPointer = path.root == PlaceRoot::Pointer;

    if(viaPointer) {
        if(!path.pointer) return maxLimit<U32>;
        root = analysis.local[path.pointer]->id;
    } else {
        root = rootLocal(analysis, path);
    }

    if(root == maxLimit<U32>) return maxLimit<U32>;

    // Built before the scan because the comparison is against it, and discarded by rewinding the
    // shared array when the slot turns out to already exist.
    auto& steps = analysis.scratch.borrowPath;
    auto start = U32(steps.size());

    for(auto projection: path.projections.contents(analysis.local)) {
        U32 step = 0;
        if(!pathStep(projection, step)) { steps.resize(start); return maxLimit<U32>; }
        steps.push(step);
    }

    auto count = U32(steps.size()) - start;

    for(Size i = 0; i < analysis.borrowSlots.size(); i++) {
        auto& candidate = analysis.borrowSlots[i];
        if(candidate.root != root || candidate.viaPointer != viaPointer) continue;
        if(candidate.pathCount != count) continue;

        auto same = true;
        for(U32 s = 0; s < count && same; s++) {
            same = steps[candidate.pathStart + s] == steps[start + s];
        }

        if(!same) continue;

        steps.resize(start);
        return U32(i);
    }

    if(!create) { steps.resize(start); return maxLimit<U32>; }

    analysis.borrowSlots.push(BorrowSlot {
        .root = root,
        .pathStart = start,
        .pathCount = count,
        .viaPointer = viaPointer,
        .type = reference.to,
    });

    return U32(analysis.borrowSlots.size() - 1);
}

U32 borrowSlotOf(Analysis& analysis, const Place& place) {
    return internBorrowSlot(analysis, place, false);
}

// The write that fills borrowed storage again. An Init or a whole-slot Assign through the same
// borrow, which is what `acc = acc ++ part` lowers its second half to.
static U32 filledSlot(Analysis& analysis, Inst& instruction) {
    if(instruction.kind != Value::Assign && instruction.kind != Value::Init) return maxLimit<U32>;
    return borrowSlotOf(analysis, ((InstInit&)instruction).place);
}

void computeBorrowOwnership(Analysis& analysis) {
    // Emptied by the constructor, with the rest - see Analysis::Analysis. What is reset here is only
    // the table, because a body with no slots must leave no rows behind for the next one either.
    auto& slots = analysis.borrowSlots;
    analysis.borrowStateBefore.reset(0);

    /*
     * The interning scan, which is also the test for whether there is anything to do.
     *
     * Only a move creates a slot. A write through a borrow is what *fills* one, and a body full of
     * those with no move among them has nothing to prove - which is every function in the library
     * that assigns through a `&` parameter, and the reason this pass costs them one scan and no
     * rows at all.
     *
     * Droppable only. Storage nobody has to release cannot be double-freed by a hole, so a move out
     * of it and no write back is a read, and reads of borrowed storage are what borrowing is for.
     */
    for(Size i = 0; i < analysis.instructionCount; i++) {
        auto& instruction = *analysis.local[analysis.order[i]];
        if(instruction.kind != Value::Move) continue;

        auto& moved = (InstMove&)instruction;
        if(!needsTeardown(analysis.module, placeType(analysis.module, analysis.function, moved.place))) {
            continue;
        }

        internBorrowSlot(analysis, moved.place, true);
    }

    if(slots.isEmpty()) return;

    auto count = slots.size();
    auto blocks = analysis.blockCount();

    auto& entry = analysis.scratch.borrowEntry;
    auto& states = analysis.scratch.borrowWalk;
    auto& reached = analysis.scratch.work;

    analysis.borrowStateBefore.reset(analysis.instructionCount, count, OwnState::Owned);
    entry.reset(blocks, count, OwnState::Owned);
    reached.reset(blocks);

    /*
     * Borrowed storage arrives holding something, on every path, always.
     *
     * That is the difference from the lattice above, whose bottom is Uninitialized because a local's
     * storage genuinely holds nothing until something puts a value in it. A borrow is a reference to
     * a value that already exists - there is no program point at which one refers to a hole, because
     * the frame that lent it could not have lent it otherwise. So the walk starts at Owned and the
     * only thing that can empty a slot is this body's own move.
     */
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
            analysis.borrowStateBefore.copyInto(i, states);

            auto& instruction = *analysis.local[analysis.order[i]];

            // Moves before fills, on the same reading as the lattice above: `acc = acc ++ part`
            // empties the slot and then puts a value back, and the second of the two is what the
            // instruction after it sees.
            if(instruction.kind == Value::Move) {
                auto slot = borrowSlotOf(analysis, ((InstMove&)instruction).place);
                if(slot != maxLimit<U32>) states[slot] = OwnState::Moved;
            }

            auto filled = filledSlot(analysis, instruction);
            if(filled != maxLimit<U32>) states[filled] = OwnState::Owned;
        }

        for(auto successor: block->successors()) {
            if(!successor) continue;

            auto successorIndex = analysis.local[successor]->index;
            auto updated = false;

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
