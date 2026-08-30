#include "analyze_internal.h"

namespace mir {

bool isTracked(DeclBase base, DeclPtr<Type> type) {
    auto ownership = ownershipOf(base, type);
    return ownership.needsTeardown() || !ownership.trivialCopy;
}

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

                /*
                 * The subject of a teardown is already being destroyed - see `Function::teardown`.
                 *
                 * It is the one owned local in the language that this frame does not owe a teardown
                 * for, because this frame *is* the teardown: a path for it would place a call to the
                 * function the body is in, on every path through it.
                 */
                auto local = body.local(base, LocalId(place->payload));
                if(function.teardown && local->parameter == 0) break;

                exact[i] = create(*place, MovePathId());
                cover[i] = exact[i];
                break;
            }

            case Place::Field: {
                auto parent = base[place->parent];
                auto within = exact[parent->id.index];
                cover[i] = cover[parent->id.index];

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
    blockCount = body.blocks.size();

    out.flagged.reserve(program.mir, pathCount);
    for(U32 i = 0; i < pathCount; i++) out.flagged.push(program.mir, 0);
}

} // namespace mir
