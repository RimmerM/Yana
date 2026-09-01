#include "analyze_internal.h"
#include "own.h"
#include "print.h"

namespace mir {

/*
 * Liveness over move paths - §6.2.
 *
 * The ordinary backward fixpoint, with one thing about it that is not ordinary and is the whole
 * reason it is over *paths* rather than over locals: a use and a definition do not travel the same
 * way through the tree.
 *
 * A **use** of `p.left` keeps `p` alive, because releasing `p` would release `p.left`; so a use
 * walks *up*. A use of `p` as a whole also keeps `p.left` alive, because a whole read reads its
 * members; so an exact use also walks *down*. A **definition** of `p.left` does not redefine `p`,
 * so a kill walks down and stops - and a walk that killed upwards is how writing one field of a
 * record comes to look like a redefinition of the record and lets its other fields be released
 * early.
 */
void Analysis::markUse(U64* live, MovePathId id, bool exactly) {
    for(auto up = id; up.valid(); up = path(up).parent) BitMatrix::set(live, up.index);
    if(!exactly) return;

    // Downwards from the exact path: a whole read of a record is a read of every member.
    SmallArray<MovePathId, 16> pending;
    pending.push(id);

    while(pending.size()) {
        auto current = pending.pop().unwrap();
        BitMatrix::set(live, current.index);

        for(auto child: path(current).children.contents(base)) pending.push(child);
    }
}

void Analysis::markKill(U64* live, MovePathId id) {
    SmallArray<MovePathId, 16> pending;
    pending.push(id);

    while(pending.size()) {
        auto current = pending.pop().unwrap();
        BitMatrix::clear(live, current.index);

        for(auto child: path(current).children.contents(base)) pending.push(child);
    }
}

/*
 * One node, backwards: kill what it defines, then mark what it reads - and what it wrote *into*.
 *
 * That order and not the other. A write whose value came out of the place it writes - `x = f(x)`
 * for a type that owns something - reads the old contents and then replaces them, so the read has to
 * survive the kill; doing the kill second would make the source of a self-assignment dead before it
 * was read.
 */
void Analysis::applyBackward(U64* live, OriginCursor& cursor, Node& node) {
    forEachOperand(base, node, [&](Operand& operand, OperandRole role) {
        if(role != OperandRole::WriteTarget) return;
        if(operand.source() != SourceKind::Place) return;

        auto id = exact[operand.placeId().index];
        if(id.valid()) markKill(live, id);
    });

    /*
     * Writing *into* a value is a use of the value.
     *
     * `g.id = 9` defines `g.id` and says nothing about `g` - the kill above is right to walk down
     * and stop - but the storage has to still be there for the field to be written, and that is a
     * use of everything the place is reached through. Without it the container's live range ended
     * at its last *read*, and a partial write below that read got a `drop` placed above it: the
     * write then landed in storage that had been released, the write was normalized to a `reinit`
     * because the state said the value was gone, and the teardown ran a second time at the end of
     * the body. A `Drop` on freed memory and a double release, from three passes each behaving as
     * documented.
     *
     * The ancestors and never the place itself, which is why this is not simply `useOfPlace`: the
     * exact path was just killed and marking it live again would undo the kill. `cover` is already
     * a proper ancestor where the place is not tracked itself, and where it is, the parent is.
     */
    forEachOperand(base, node, [&](Operand& operand, OperandRole role) {
        if(role != OperandRole::WriteTarget) return;
        if(operand.source() != SourceKind::Place) return;

        auto index = operand.placeId().index;
        auto container = exact[index].valid() ? path(exact[index]).parent : cover[index];
        if(container.valid()) markUse(live, container, false);
    });

    forEachOperand(base, node, [&](Operand& operand, OperandRole role) {
        if(role != OperandRole::ReadSource) return;
        if(operand.source() != SourceKind::Place) return;

        useOfPlace(live, operand.placeId());
    });

    /*
     * A borrow keeps the storage it came out of alive for as long as the *value* is used.
     *
     * Without this the extent of a loan is the instruction that took it, which is a use after free
     * with two instructions between: `read borrow.shared (held)` names the place, `call label` names
     * the read's result, and nothing between them names `held` - so the place looks dead at the call
     * and the object is released before the callee reads it. That is not a corner case; it is what
     * every borrowed argument looks like.
     *
     * The rule is the origin environment read directly (§5.11): a use of a value is a use of the
     * storage behind every loan the value may be rooted in *at the positions the use reaches* - so
     * `t.right` keeps the guard its own field named alive and says nothing about `t.left`'s. That covers the borrow read out of a
     * place and used at the next instruction, and equally the one bound to a name, carried around a
     * loop, returned out of a call in a published slot, or reborrowed twice over - all of which are
     * the same statement about the *set* rather than four rules about four shapes. It replaces a
     * one-step walk to the producing read, which was correct for exactly the first of those.
     */
    forEachOperand(base, node, [&](Operand& operand, OperandRole) {
        forEachOperandOrigin(cursor, operand, [&](U32, const U64* origins) {
            BitMatrix::forEach(origins, loanWords, [&](Size l) {
                useOfPlace(live, body.loan(base, LoanId(U32(l)))->place);
            });
        });
    });
}

// One place, used. The exact path where the place is tracked itself and the nearest tracked
// container where it is not - see `Analysis::exact` and `Analysis::cover` for why those differ.
void Analysis::useOfPlace(U64* live, PlaceId place) {
    auto index = place.index;
    auto exactly = exact[index].valid();
    auto id = exactly ? exact[index] : cover[index];
    if(id.valid()) markUse(live, id, exactly);
}

void Analysis::computeLiveness() {
    liveIn.reset(blockCount, pathCount);
    liveOut.reset(blockCount, pathCount);
    if(!pathCount) return;

    SmallArray<U64, 8> live;
    for(U32 w = 0; w < pathWords; w++) live.push(0);

    OriginCursor cursor { *this };

    sweepBackward([&](U32 index) {
        auto block = this->block(index);
        BitMatrix::zero(live.pointer(), pathWords);

        // Out is the union of the successors' ins. The edge operands themselves are operands of the
        // terminator, so they are counted by the walk below rather than here.
        forEachSuccessor(index, [&](U32 target) {
            BitMatrix::unionInto(live.pointer(), liveAt(liveIn, target), pathWords);
        });

        BitMatrix::replace(liveAt(liveOut, index), live.pointer(), pathWords);

        auto count = block->nodes.size();
        cursor.enterAtExit(index);

        for(U32 n = count; n > 0; n--) {
            cursor.undo(n - 1);
            applyBackward(live.pointer(), cursor, nodeAt(index, n - 1));
        }

        if(BitMatrix::replace(liveAt(liveIn, index), live.pointer(), pathWords)) {
            wakePredecessors(index);
        }
    });
}

/*
 * Every `Forward`, answered - §5.4, and the one rewrite a sealed body permits.
 *
 * A bare owned name in a sink position is a move when it is dead afterwards and is nothing else when
 * it is not: the type is one that refuses duplication, so there is no borrow that would satisfy an
 * owning position and no implicit copy to fall back on. Two answers, therefore, and the second is a
 * diagnostic rather than a different mode.
 *
 * The walk is backwards within each block from the block's own live-out, because "afterwards" is
 * exactly what liveness at the *next* position means and recomputing it forwards would need the
 * fixpoint's answer at every point rather than at every block.
 */
bool Analysis::resolveForwards() {
    if(!pathCount) return true;

    OriginCursor cursor { *this };

    for(U32 index = 0; index < blockCount; index++) {
        auto block = this->block(index);
        auto count = block->nodes.size();

        SmallArray<U64, 8> live;
        auto out = liveAt(liveOut, index);
        for(U32 w = 0; w < pathWords; w++) live.push(out[w]);

        cursor.enterAtExit(index);

        for(U32 n = count; n > 0; n--) {
            cursor.undo(n - 1);
            auto& node = nodeAt(index, n - 1);

            // What is live *after* this node is what the walk holds before it steps over it, which
            // is what "dead afterwards" is asking about.
            forEachOperand(base, node, [&](Operand& operand, OperandRole role) {
                if(operand.mode() != UseMode::Forward) return;

                if(role != OperandRole::ReadSource || operand.source() != SourceKind::Place) {
                    report(node.source, "internal: a forward operand does not name a place"_v);
                    return;
                }

                auto place = body.place(base, operand.placeId());
                auto id = exact[operand.placeId().index];

                if(!id.valid()) {
                    /*
                     * Nothing tracks this storage, so there is no move to record and no state to
                     * leave behind. A global is the case that reaches this: it outlives every body,
                     * so moving out of one would leave a hole the language has no way to fill.
                     */
                    report(node.source,
                           "a value of type %@ cannot be moved out of this place - it is not storage this function owns"_v,
                           typeText(context, decl, place->occurrence.type));
                    return;
                }

                if(BitMatrix::test(live.pointer(), id.index)) {
                    report(node.source,
                           "%@ is used after this, and %@ cannot be copied - write the last use as the one that consumes it"_v,
                           pathName(id),
                           typeText(context, decl, place->occurrence.type));
                    return;
                }

                operand = operand.withMode(UseMode::Take);
            });

            applyBackward(live.pointer(), cursor, node);
        }
    }

    return errors == 0;
}

/*
 * The name a diagnostic calls a path by - what the author wrote, projections included.
 *
 * `p.left` rather than `p`, because a partially moved record has a state per field and a message
 * that named only the binding could not tell "the record is gone" from "one of its fields is". That
 * distinction is the whole of partial ownership, so a diagnostic that cannot make it is describing a
 * different language: the first version of this named the root and produced "p is used after this,
 * and Guard cannot be copied" for a `p` that is a `Pair`.
 *
 * `placeText` is the spelling, shared with the dump and with the resolver's own diagnostics - see
 * print.h.
 */
String Analysis::pathName(MovePathId id) {
    return placeText(context, decl, base, body, *body.place(base, path(id).place));
}

bool Analysis::writtenDisposal(MovePathId id) {
    auto place = body.place(base, path(id).place);
    if(place->kind != Place::Local) return false;
    return body.local(base, LocalId(place->payload))->disposal;
}

/*
 * One deferred disposal, recorded once per type.
 *
 * Once per *type* and not per point, because what the instantiation asks is a question about the
 * type: a body that lets three values of one type die is one fact stated three times, and three
 * diagnostics per specialization would be three reports of one mistake. The first point wins, which
 * makes the record and the message that reads it depend on the body's own order rather than on the
 * order the caller happened to specialize in.
 *
 * `implicit` is or-ed rather than overwritten. A body that both writes `->x` for one value and lets
 * another of the same type die implicitly has chosen a point, and the strictest reading of the
 * disposals it holds is what the type has to pass.
 */
void Analysis::deferDisposal(DeclPtr<Type> type, LocationId source, bool implicit) {
    for(Size i = 0; i < function.disposals.size(); i++) {
        auto held = function.disposals.get(decl, U32(i));
        if(held.type != type) continue;

        if(implicit && !held.implicit) {
            held.implicit = true;
            function.disposals.set(decl, U32(i), held);
        }

        return;
    }

    Disposal disposal;
    disposal.type = type;
    disposal.source = source;
    disposal.implicit = implicit;
    function.disposals.push(program.decl, disposal);
}

void Analysis::checkDisposalPermission(LocationId source, MovePathId id) {
    if(disposalPermitted(context, program, path(id).type, function.module)) return;

    report(source,
           "%@ has a `@local` teardown, so it may only be disposed of in the module that declared it"_v,
           typeText(context, decl, path(id).type));
}

} // namespace mir
