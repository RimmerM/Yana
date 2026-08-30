#include "analyze_internal.h"
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
void Analysis::markUse(U8* live, MovePathId id, bool exactly) {
    for(auto up = id; up.valid(); up = path(up).parent) live[up.index] = 1;
    if(!exactly) return;

    // Downwards from the exact path: a whole read of a record is a read of every member.
    SmallArray<MovePathId, 16> pending;
    pending.push(id);

    while(pending.size()) {
        auto current = pending.pop().unwrap();
        live[current.index] = 1;

        for(auto child: path(current).children.contents(base)) pending.push(child);
    }
}

void Analysis::markKill(U8* live, MovePathId id) {
    SmallArray<MovePathId, 16> pending;
    pending.push(id);

    while(pending.size()) {
        auto current = pending.pop().unwrap();
        live[current.index] = 0;

        for(auto child: path(current).children.contents(base)) pending.push(child);
    }
}

/*
 * One node, backwards: kill what it defines, then mark what it reads.
 *
 * That order and not the other. A write whose value came out of the place it writes - `x = f(x)`
 * for a type that owns something - reads the old contents and then replaces them, so the read has to
 * survive the kill; doing the kill second would make the source of a self-assignment dead before it
 * was read.
 */
void Analysis::applyBackward(U8* live, Node& node) {
    forEachOperand(base, node, [&](Operand& operand, OperandRole role) {
        if(role != OperandRole::WriteTarget) return;
        if(operand.source() != SourceKind::Place) return;

        auto id = exact[operand.placeId().index];
        if(id.valid()) markKill(live, id);
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
     * A use of the result is therefore a use of the place, walked one step. **One step, and this is
     * the interim form**: §6.4's loan checking gives a borrow an `origin set` and an extent, and a
     * borrow stored in a value that is itself passed on is what that machinery is for. Until then
     * the rule covers what this stage can build - a borrow read out of a place and used directly -
     * and a borrow that escaped into a record would be outside it, which is why milestone 5 replaces
     * this rather than extending it.
     */
    forEachOperand(base, node, [&](Operand& operand, OperandRole) {
        if(operand.source() != SourceKind::Result) return;

        auto producer = body.node(base, operand.nodeId());
        if(producer->kind != Node::Read) return;

        auto& read = (NodeRead&)*producer;
        switch(read.place.mode()) {
            case UseMode::BorrowShared:
            case UseMode::BorrowUnique:
            case UseMode::Inspect:
                break;

            // A copy and a move both produce a value that is not rooted in the place any more: the
            // first duplicated it and the second took it, and neither depends on what is left there.
            case UseMode::CopyTrivial:
            case UseMode::Take:
            case UseMode::Forward:
            case UseMode::Count:
                return;
        }

        useOfPlace(live, read.place.placeId());
    });
}

// One place, used. The exact path where the place is tracked itself and the nearest tracked
// container where it is not - see `Analysis::exact` and `Analysis::cover` for why those differ.
void Analysis::useOfPlace(U8* live, PlaceId place) {
    auto index = place.index;
    auto exactly = exact[index].valid();
    auto id = exactly ? exact[index] : cover[index];
    if(id.valid()) markUse(live, id, exactly);
}

void Analysis::computeLiveness() {
    auto entries = blockCount * pathCount;

    liveIn.clear();
    liveOut.clear();
    liveIn.reserve(entries);
    liveOut.reserve(entries);

    for(U32 i = 0; i < entries; i++) {
        liveIn.push(0);
        liveOut.push(0);
    }

    if(!pathCount) return;

    /*
     * Reverse block order until nothing moves.
     *
     * A worklist would visit fewer blocks; a reverse sweep visits a loop's body before its header on
     * the first round, which is where the saving would have come from, and a body of tens of blocks
     * settles in two or three rounds either way. When a body appears where it does not, the
     * measurement belongs beside this comment.
     */
    for(auto changed = true; changed; ) {
        changed = false;

        for(U32 i = blockCount; i > 0; i--) {
            auto index = i - 1;
            auto block = this->block(index);

            SmallArray<U8, 64> live;
            for(U32 p = 0; p < pathCount; p++) live.push(0);

            // Out is the union of the successors' ins. The edge operands themselves are operands of
            // the terminator, so they are counted by the walk below rather than here.
            auto count = block->nodes.size();
            if(count) {
                auto last = base[block->nodes.get(base, count - 1)];
                forEachEdge(*last, [&](Edge& edge) {
                    if(!edge.target) return;

                    auto target = liveAt(liveIn, base[edge.target]->index.index);
                    for(U32 p = 0; p < pathCount; p++) live[p] |= target[p];
                });
            }

            auto out = liveAt(liveOut, index);
            for(U32 p = 0; p < pathCount; p++) {
                if(out[p] == live[p]) continue;
                out[p] = live[p];
                changed = true;
            }

            for(U32 n = count; n > 0; n--) applyBackward(live.pointer(), nodeAt(index, n - 1));

            auto in = liveAt(liveIn, index);
            for(U32 p = 0; p < pathCount; p++) {
                if(in[p] == live[p]) continue;
                in[p] = live[p];
                changed = true;
            }
        }
    }
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

    for(U32 index = 0; index < blockCount; index++) {
        auto block = this->block(index);
        auto count = block->nodes.size();

        SmallArray<U8, 64> live;
        auto out = liveAt(liveOut, index);
        for(U32 p = 0; p < pathCount; p++) live.push(out[p]);

        for(U32 n = count; n > 0; n--) {
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

                if(live[id.index]) {
                    report(node.source,
                           "%@ is used after this, and %@ cannot be copied - write the last use as the one that consumes it"_v,
                           pathName(id),
                           typeText(context, decl, place->occurrence.type));
                    return;
                }

                operand = operand.withMode(UseMode::Take);
            });

            applyBackward(live.pointer(), node);
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

} // namespace mir
