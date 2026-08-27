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

/*
 * Whether a local names storage this frame may empty and fill again.
 *
 * `&` and nothing else. A plain borrowed parameter is shared - any number of readers may be looking
 * at the same storage at once, and one of them emptying it is visible to the rest however quickly it
 * writes back. Exclusivity is precisely the capability that makes the window unobservable, which is
 * what the whole rule rests on, and it is the same test internBorrowSlot applies to a borrow value's
 * `mut` bit one level further out.
 *
 * A closure's environment slot is not owned either and is not a `&` binding, so it falls out here
 * without a case of its own.
 */
static bool exclusiveBinding(Analysis& analysis, U32 local) {
    if(local >= analysis.function.localCount()) return false;

    auto slot = analysis.function.localAt(analysis.local, local);
    return slot.borrowed && slot.convention == ast::BindType::Ref;
}

BorrowedPlace borrowedPlaceOf(Analysis& analysis, const Place& place) {
    BorrowedPlace result;

    /*
     * A raw pointer root is deliberately absent. `let ->x = *p` is Native taking ownership of memory
     * it holds the only address of, which is the one thing that module exists to be able to do, so a
     * pointer place is not borrowed storage in this sense at all.
     *
     * A global is borrowed - nothing may take its contents - but never emptiable: its storage
     * outlives every frame there is, so there is no release point at which a write back could be
     * required, and checkMoves gives it its own message.
     */
    auto borrowedRoot = place.root == PlaceRoot::Borrow;
    auto local = maxLimit<U32>;

    if(!borrowedRoot) {
        if(place.root != PlaceRoot::Local) return result;

        local = rootLocal(analysis, place);
        if(local == maxLimit<U32> || analysis.tracked[local].owned) return result;
    }

    result.borrowed = true;

    /*
     * A value with no teardown is *read* out of borrowed storage rather than taken out of it.
     *
     * Nothing is invalidated by the read: there is no teardown to run twice, and the bytes the
     * source holds are as good after it as before. So the obligation this rule exists to impose -
     * put something back before the owner sees it again - has nothing to protect, and there is no
     * state to keep either.
     *
     * It is not a corner. `String` on the JavaScript target is a host primitive, so the identical
     * `concat` that reaches the lattice natively needs no lattice at all there - and without this
     * the interning scan, which only ever makes a slot for droppable storage, would decline and the
     * move would be refused on a target where it is the safest thing in the file.
     *
     * **TrivialSink as well as no teardown**, because it is the second clause above that an
     * authored `Sink` denies: writing one *is* the statement that the bytes are not the whole story
     * (Core.yana says so where the class is declared). A type whose sink fixes up an interior
     * reference leaves stale bytes behind when it is relocated out of a container that still counts
     * the element, and nothing about the absence of a teardown makes that safe. Nothing in this tree
     * has an authored Sink, so this costs no fixture and closes the hole before there is one.
     */
    auto held = ownershipIn(analysis.module, functionGen(analysis.global, analysis.function),
                            placeType(analysis.module, analysis.function, place));

    if(!held.needsTeardown() && held.trivialSink) {
        result.emptiable = true;
        return result;
    }

    if(borrowedRoot) {
        result.slot = borrowSlotOf(analysis, place);
        result.emptiable = result.slot != maxLimit<U32>;
        return result;
    }

    // The state row is the *slot's*, so a projected place cannot use it: the lattice is per local
    // and says nothing about which member of one is still there.
    auto projections = place.projections;
    if(projections.isNotEmpty() || !exclusiveBinding(analysis, local)) return result;

    result.emptiable = true;
    result.local = local;
    return result;
}

OwnState borrowedStateAt(Analysis& analysis, Size index, const BorrowedPlace& place) {
    if(place.slot != maxLimit<U32>) return analysis.borrowStateBefore[index][place.slot];
    if(place.local != maxLimit<U32>) return analysis.stateBefore[index][place.local];

    // Not emptiable, so nothing ever took anything out of it.
    return OwnState::Owned;
}

/*
 * The place a memory-typed argument names, in the two shapes one arrives in.
 *
 * A concrete call writes the hand-over out - `%v = move %target` is what `sinkValue` puts in front
 * of a `->` argument - and a deferred class dispatch does not: `%v = load %target : a` is the whole
 * of it there, because `emitGenericDispatch` deliberately applies no conventions, the callee not
 * being a function that call site reaches. Both name the same storage, and answering it once here is
 * what keeps the two passes that ask from being written twice and getting one of them wrong.
 *
 * Null for everything else, which includes every argument that is a computed value: those name no
 * storage, so there is nothing for anyone to be asking about.
 */
const Place* argumentPlace(Analysis& analysis, ModulePtr<Value> arg) {
    if(!arg) return nullptr;

    auto& value = *analysis.local[arg];

    if(value.kind == Value::Move) return &((InstMove&)value).place;
    if(value.kind == Value::LoadPlace) return &((InstLoadPlace&)value).place;

    return nullptr;
}

/*
 * Whether the callee declared position `index` as `->`, over the three call forms.
 *
 * One reading for all of them, and each is the same declaration seen from a different distance: a
 * direct call has the function, an erased dispatch has the class signature - a signature is where a
 * `->` is declared, and a class method's conventions are the class's, so every instance takes the
 * argument the same way - and a dynamic call has the function *type*, which interns each argument's
 * convention precisely so that a caller holding nothing else can read it.
 *
 * **Unknown is not a sink.** A callee this cannot see through, a position past the last declared
 * parameter, a `calldyn` with no function type: none of those said the value was handed over, and
 * both callers use this to stop doing something rather than to start, so the default that errs safe
 * is the one that keeps the ordinary rule in force. That is the opposite of `assumedRetained`'s, and
 * deliberately - it answers "might this be kept", which has to guess the other way.
 */
bool declaredSink(Analysis& analysis, Inst& instruction, U16 index) {
    auto sunk = [&](ModulePtr<Function> callee) {
        if(!callee) return false;

        auto& declared = analysis.local[callee]->args;
        if(index >= declared.size()) return false;

        return analysis.local[declared.get(analysis.local, index)]->convention == ast::BindType::Sink;
    };

    switch(instruction.kind) {
        case Value::Call:
            return sunk(((InstCall&)instruction).callee);

        case Value::GenCall:
            return sunk(((InstGenCall&)instruction).callee);

        case Value::CallDyn: {
            auto signature = ((InstCallDyn&)instruction).signature;
            if(!signature || analysis.global[signature]->kind != Type::Fun) return false;

            auto& declared = ((FunType*)analysis.global[signature])->args;
            if(index >= declared.size()) return false;

            return declared.get(analysis.global, index).convention == ast::BindType::Sink;
        }

        default:
            return false;
    }
}

// The arguments of whichever of the three call forms this is, or nothing for anything else. The
// three lists are separate fields of three structs and there is no base holding them, which is the
// only reason this exists.
ModuleList<ModulePtr<Value>, false>* callArguments(Inst& instruction) {
    switch(instruction.kind) {
        case Value::Call: return &((InstCall&)instruction).args;
        case Value::GenCall: return &((InstGenCall&)instruction).args;
        case Value::CallDyn: return &((InstCallDyn&)instruction).args;
        default: return nullptr;
    }
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
     * Only a hand-over creates a slot - see eachEmptiedPlace for the two shapes one has. A write
     * through a borrow is what *fills* one, and a body full of those with none among them has
     * nothing to prove - which is every function in the library that assigns through a `&`
     * parameter, and the reason this pass costs them one scan and no rows at all.
     *
     * Droppable only. Storage nobody has to release cannot be double-freed by a hole, so a move out
     * of it and no write back is a read, and reads of borrowed storage are what borrowing is for.
     */
    for(Size i = 0; i < analysis.instructionCount; i++) {
        eachEmptiedPlace(analysis, *analysis.local[analysis.order[i]], [&](const Place& place) {
            if(!needsTeardown(analysis.module, placeType(analysis.module, analysis.function, place))) {
                return;
            }

            internBorrowSlot(analysis, place, true);
        });
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
            eachEmptiedPlace(analysis, instruction, [&](const Place& place) {
                auto slot = borrowSlotOf(analysis, place);
                if(slot != maxLimit<U32>) states[slot] = OwnState::Moved;
            });

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
