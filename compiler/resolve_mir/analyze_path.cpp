#include "analyze_internal.h"

namespace mir {

PathState meetState(PathState a, PathState b) {
    if(a == b) return a;
    if(a == PathState::Maybe || b == PathState::Maybe) return PathState::Maybe;
    if(a == PathState::Owned || b == PathState::Owned) return PathState::Maybe;

    // `Uninitialized` meeting `Moved`: nothing is here either way, and the diagnostic a later use
    // gets should be the one that names an action the author took.
    return PathState::Moved;
}

/*
 * The move-path tree, built from the places the body actually interned.
 *
 * Lazily in exactly the sense §5.10 means: a path exists because some place in the body names it,
 * not because the type has a field. A record read only as a whole interns one place and gets one
 * path; a record one of whose fields is moved out interns that field's place and gets two.
 *
 * The walk relies on the interning order - a `Field` place is created after the place it projects,
 * because `Builder::fieldPlace` takes the parent record's place - so one forward pass sees every
 * parent before its children and needs no second round.
 */
void Analysis::buildPaths() {
    auto places = body.places.size();

    exact.clear();
    cover.clear();
    exact.reserve(places);
    cover.reserve(places);

    for(U32 i = 0; i < places; i++) {
        exact.push(MovePathId());
        cover.push(MovePathId());
    }

    auto create = [&](Place& place, MovePathId parent) {
        auto record = new (program.mir) MovePath();
        record->id = MovePathId(out.paths.push(program.mir, record - base));
        record->parent = parent;
        record->place = place.id;
        record->type = place.occurrence.type;

        if(parent.valid()) path(parent).children.push(program.mir, record->id);
        return record->id;
    };

    for(U32 i = 0; i < places; i++) {
        auto place = body.place(base, PlaceId(i));

        switch(place->kind) {
            case Place::Local: {
                /*
                 * A local root, where the function owns what is in it.
                 *
                 * A borrowed parameter is storage somebody else owns, so there is nothing here to
                 * initialize, move or release - what the loan checker has to say about it is
                 * milestone 5's, and giving it a move path now would make this pass place a drop for
                 * the caller's value.
                 */
                if(!place->occurrence.contract.isOwned()) break;
                if(!isTracked(decl, place->occurrence.type)) break;

                exact[i] = create(*place, MovePathId());
                cover[i] = exact[i];

                /*
                 * The subject of a teardown is already being destroyed - see `Function::teardown`
                 * and `Analysis::teardownSubject`.
                 *
                 * It is followed like any other owned local and released like none of them: this
                 * frame *is* the teardown, so a drop placed for it would call the function the body
                 * is in on every path through it. `placeDrops` reads the field and emits nothing in
                 * the subtree; what the following buys is the diagnostic for a linear member the
                 * body forgot, and a partial move out of the subject that this used to refuse.
                 */
                auto local = body.local(base, LocalId(place->payload));
                if(function.teardown && local->parameter == 0) teardownSubject = exact[i];

                /*
                 * Storage a flattened lens brought with it - `Local::region`, and `keepFrameLocals`.
                 *
                 * The region is matched exactly rather than walked upwards, because that is what
                 * distinguishes the two kinds of local inside a lens call: the callee's own are made
                 * while its `Frame` region is open, and a continuation's are made inside the
                 * `Handover` nested in it and belong to the caller like any other.
                 */
                auto region = body.region(base, local->region);
                if(region && region->kind == Continuation::Frame) {
                    frameLocals.push(FrameLocal { local->region, exact[i] });
                }

                break;
            }

            /*
             * A property step is tracked exactly as a field is - see `Place::Property`.
             *
             * Which field it turns out to be differs per instantiation, and *that it is one* does
             * not: the requirement names one component of its subject, two requirements name two,
             * and the partial-ownership argument below applies to both without a word changing. The
             * conversion is where the index arrives, and a path built here is what makes the read
             * of one an ordinary read of a projection.
             */
            case Place::Property:
            case Place::Field: {
                auto parent = base[place->parent];
                auto within = exact[parent->id.index];
                cover[i] = cover[parent->id.index];

                // An external field holds a loan rather than an object, so there is nothing here to
                // initialize, move or release - and a path for one would place a teardown for the
                // lender's value at the end of this frame.
                if(!place->occurrence.contract.isOwned()) break;

                /*
                 * A field of a tracked record, whatever the field's own type.
                 *
                 * `isTracked` is deliberately *not* asked here, and the reason is partial ownership:
                 * once `p.left` has been moved out, `p.x` is still there and still readable, and the
                 * only way to say so is for `p.x` to have a state of its own. A scheme that tracked
                 * only the fields that can be moved has to answer that question from the parent's
                 * state, which by then says the parent as a whole is gone.
                 *
                 * Such a path costs one row and never produces a drop - `placeDrops` emits one only
                 * for a type with a teardown - so what it buys is exactness in the diagnostics for
                 * nothing in the output.
                 */
                if(!within.valid()) break;

                exact[i] = create(*place, within);
                cover[i] = exact[i];
                break;
            }

            case Place::Discriminant:
            case Place::Index:
            case Place::Deref:
                /*
                 * Collapsed to the container - §5.2's conservative alias rule, read from the
                 * initialization side. Two indices into one container may be the same element, so
                 * tracking them separately would let a move out of `a[i]` and a use of `a[j]` agree
                 * that neither touched the other.
                 */
                cover[i] = cover[base[place->parent]->id.index];
                break;

            case Place::Global:
            case Place::Loan:
            case Place::Address:
                // Not this function's to release. A global outlives every body (`doc/spec/
                // ownership.md`: nothing runs for one when the program ends), and a loan and a raw
                // address name storage whose owner is elsewhere.
                break;

            case Place::KindCount:
                break;
        }
    }

    pathCount = out.paths.size();
    pathWords = U32((pathCount + 63) / 64);
    blockCount = body.blocks.size();

    computeOrder();

    out.flagged.reserve(program.mir, pathCount);
    for(U32 i = 0; i < pathCount; i++) out.flagged.push(program.mir, 0);
}

/*
 * The blocks in reverse postorder - the order every fixpoint here is driven in.
 *
 * Postorder by an explicit stack rather than by recursion, for the reason the SSA verifier's own
 * dominator walk gives: nothing bounds a body's CFG depth, so a recursive walk is a stack overflow
 * on a program rather than on a compiler bug.
 *
 * Unreachable blocks are appended in index order rather than dropped. `checkLoans` and `walkBlock`
 * walk every block the body has, so a block left out of the order would be walked against table
 * rows nothing ever wrote; visiting it with the entry state it has - which is empty, since it has no
 * reached predecessor - is the same answer the round-based sweep gave it.
 */
void Analysis::computeOrder() {
    order.clear();
    if(!blockCount) return;

    SmallArray<U32, 64> stack, next, postorder;
    IndexSet seen, ordered;

    seen.reset(blockCount);
    ordered.reset(blockCount);
    for(U32 i = 0; i < blockCount; i++) next.push(0);

    auto entry = base[body.entry]->index.index;
    stack.push(entry);
    seen.set(entry, true);

    while(stack.size()) {
        auto index = stack[stack.size() - 1];

        SmallArray<U32, 4> successors;
        forEachSuccessor(index, [&](U32 target) { successors.push(target); });

        if(next[index] < successors.size()) {
            auto successor = successors[next[index]++];
            if(seen.add(successor)) stack.push(successor);
            continue;
        }

        postorder.push(index);
        stack.pop();
    }

    for(Size i = postorder.size(); i > 0; i--) {
        ordered.set(postorder[i - 1], true);
        order.push(postorder[i - 1]);
    }

    for(U32 i = 0; i < blockCount; i++) {
        if(!ordered.get(i)) order.push(i);
    }
}

// The blocks one block's terminator can reach - what a forward fixpoint wakes when a block's exit
// state moves, and what the postorder walk above descends into.
void Analysis::wakeSuccessors(U32 index) {
    forEachSuccessor(index, [&](U32 target) { wake(target); });
}

void Analysis::wakePredecessors(U32 index) {
    for(auto predecessorPointer: this->block(index)->incoming.contents(base)) {
        wake(base[predecessorPointer]->index.index);
    }
}

} // namespace mir
