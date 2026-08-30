#include "analyze_internal.h"
#include "print.h"

namespace mir {

/*
 * Loan checking - Analysis-IR.md §6.4, and the half of `doc/spec/ownership.md` that a `let` needs.
 *
 * Three walks over one structure, and the structure is the *origin environment*: which loans each
 * value may be rooted in (§5.11). Everything else here reads it -
 *
 *  - the **extent** of a loan is the live range of the values rooted in it, so it is the same table
 *    read backwards rather than a second notion of "how long a borrow lasts";
 *  - the **overlap** check compares a new loan or a write against the loans live at that point,
 *    resolving each through the environment so that a reborrow is not a conflict with what it
 *    reborrows;
 *  - **signature conformance** asks where each loan in a returned value's set is rooted, which is a
 *    property of the loan's place and of nothing else.
 *
 * That they are one structure is the point. The current compiler answers these with a provenance
 * relation, a liveness relation and five bespoke retention checks that each read a different flag,
 * and the way that fails is that two of them disagree about what a value refers to.
 */

bool placesOverlap(MirBase base, Body& body, PlaceId a, PlaceId b) {
    if(a == b) return true;

    // The two chains, root first. Bodies project a handful of levels, so the arrays never grow.
    SmallArray<Place*, 8> left, right;
    for(auto p = body.place(base, a); p; p = p->parent ? base[p->parent] : nullptr) left.push(p);
    for(auto p = body.place(base, b); p; p = p->parent ? base[p->parent] : nullptr) right.push(p);

    auto step = [&](SmallArray<Place*, 8>& chain, Size from) {
        return chain[chain.size() - 1 - from];
    };

    auto rootA = step(left, 0);
    auto rootB = step(right, 0);

    /*
     * Two different roots are disjoint - but only because a root is storage this body *names*.
     *
     * A local and a global are different objects and a local and another local are too. A raw
     * address is not: it may point anywhere, so it is the conservative answer, and so is a place
     * reached through a loan value whose own storage this walk cannot see.
     */
    if(rootA->kind == Place::Address || rootB->kind == Place::Address ||
       rootA->kind == Place::Loan || rootB->kind == Place::Loan) {
        return true;
    }

    if(rootA->id != rootB->id) return false;

    auto depth = min(left.size(), right.size());
    for(Size i = 1; i < depth; i++) {
        auto x = step(left, i);
        auto y = step(right, i);

        // Two fields of one record are two objects; an index or a dereference may be the same one,
        // and §5.2 does not claim to prove otherwise.
        if(x->kind != Place::Field || y->kind != Place::Field) return true;
        if(x->payload != y->payload) return false;
    }

    // One is a prefix of the other: reaching the record reaches the field.
    return true;
}

Place* Analysis::rootOf(PlaceId place) {
    auto p = body.place(base, place);
    while(p->parent) p = base[p->parent];
    return p;
}

/*
 * The loans the storage a place is reached through is holding.
 *
 * `carriesOrigins` and not "is this a loan", because a record with an external field owns itself and
 * still refers to somebody else's object - `data Reader {held: 'Guard, at: I32}` is this frame's
 * value and `r.held` is not. Both are the same question to everything that reads this: what has to
 * stay alive, what a reborrow may not conflict with, and what a returned value would be taking with
 * it.
 */
U64* Analysis::originsOfOperand(NodeId at, Operand operand) {
    if(!loanCount) return nullptr;
    if(operand.source() == SourceKind::Result) return originsOfValue(operand.nodeId());
    if(operand.source() == SourceKind::Place) return originsOfPlace(at, operand.placeId());
    return nullptr;
}

U64* Analysis::originsOfPlace(NodeId at, PlaceId place) {
    if(!loanCount) return nullptr;

    auto root = rootOf(place);
    if(root->kind != Place::Local) return nullptr;

    // The state on entry to `at`, and not a whole-body union: which storage a local refers to is a
    // property of the point it is read at - see `nodeLocals`.
    return originsOfLocal(localsAtNode(at), LocalId(root->payload));
}

/*
 * The forward transfer for storage - what one node does to the local origin state.
 *
 * Two shapes and the difference between them is the whole rule.
 *
 * A write into storage this frame **owns** replaces what is there: the old value is gone, and with
 * it every origin it had. That is the strong update, and without it a local reassigned from a second
 * lender goes on carrying the first one's loan for the rest of the body.
 *
 * A write **through a loan** - a `let &d = c` binding assigned to - does not change what `d` refers
 * to; it changes what is at `c`. There is nowhere here to record that, so the written value's
 * origins are unioned into the binding, which over-approximates in the direction that is safe: the
 * value that comes back out of `d` is credited with them.
 *
 * A partial write - `r.at = 3` - is neither, and updates nothing. What it can reach was already in
 * the local's set, and the schema slot it would refine is the axis this analysis still collapses.
 */
Place* Analysis::replacedLocal(Node& node) {
    if(node.kind != Node::Write) return nullptr;

    auto& write = (NodeWrite&)node;
    if(write.destination.source() != SourceKind::Place) return nullptr;

    auto place = body.place(base, write.destination.placeId());
    if(place->parent || place->kind != Place::Local) return nullptr;
    if(!place->occurrence.contract.isOwned()) return nullptr;

    return place;
}

void Analysis::stepOriginsForward(U64* locals, Node& node) {
    if(!changesLocalOrigins(node)) return;

    auto& write = (NodeWrite&)node;
    auto place = body.place(base, write.destination.placeId());
    auto into = originsOfLocal(locals, LocalId(place->payload));

    // Read at this node, so it is the state *before* the write - `nodeLocals` and `locals` are
    // different buffers, so a local assigned from itself copies a row onto a row rather than into
    // the one it is reading.
    auto from = originsOfOperand(node.id, write.value);

    if(replacedLocal(node)) {
        if(from) BitMatrix::copy(into, from, loanWords);
        else BitMatrix::zero(into, loanWords);
        return;
    }

    if(from) BitMatrix::unionInto(into, from, loanWords);
}

/*
 * Whether one node moves the local origin state at all - `stepOriginsForward`'s condition, asked
 * without the state in hand.
 *
 * Structural on purpose: it is what `nodeRow` divides the body into runs by, and the runs are built
 * once before the fixpoint climbs. A condition that depended on the state would give runs that move
 * as the analysis settles, and a table indexed by them would be indexed by a moving target.
 */
bool Analysis::changesLocalOrigins(Node& node) {
    if(node.kind != Node::Write) return false;

    auto& write = (NodeWrite&)node;
    if(write.destination.source() != SourceKind::Place) return false;

    auto place = body.place(base, write.destination.placeId());
    if(place->parent || place->kind != Place::Local) return false;

    return originLocal[place->payload] != kNoOriginLocal;
}

/*
 * Which row of `nodeLocals` each node reads - the run compression, built once.
 *
 * A block starts a run and every node that changes the state ends one, so a body of five hundred
 * nodes and four writes to origin-carrying locals has a table of rows counted in blocks rather than
 * in nodes. A block with no nodes claims no row, and neither does the tail of a block whose last
 * node is a write - a row is allocated where a node is about to read it and nowhere else.
 */
void Analysis::buildLocalRows() {
    for(U32 i = 0; i < body.nodes.size(); i++) nodeRow.push(kNoRow);

    for(U32 index = 0; index < blockCount; index++) {
        auto block = this->block(index);
        auto row = kNoRow;

        for(auto nodePointer: block->nodes.contents(base)) {
            auto& node = *base[nodePointer];

            if(row == kNoRow) row = localRowCount++;
            nodeRow[node.id.index] = row;

            if(changesLocalOrigins(node)) row = kNoRow;
        }
    }
}

/*
 * What every value is rooted in - §5.11's `OriginEnvironment`, as a forward fixpoint.
 *
 * A fixpoint rather than one pass because a value's origin set is the union over every path that
 * reaches it and a loop may write storage after it has already been read. The lattice is a finite
 * set of loans, the transfer is monotone - a strong update replaces one set with another that
 * itself only grows - and every table only ever gains bits, so it settles.
 *
 * Five rules, and each is one sentence:
 *
 *  - a borrowing read is rooted in the loan it took, and in whatever the storage it read was rooted
 *    in *at that point* - which is what makes a reborrow carry its lender along;
 *  - a call whose result is a published loan is rooted in whatever filled that slot, which is
 *    §2.2's "at a call, a slot is bound to a set of concrete origins";
 *  - a block parameter is rooted in whatever every incoming edge supplied;
 *  - storage holds what was last written into it - `stepOriginsForward`;
 *  - everything else produces an owner, which is rooted in nothing.
 *
 * The state it leaves behind is `nodeLocals`, one row per node: the three walks that follow read
 * storage's origins at the point they are asking about, and two of them run backwards and could not
 * recompute a forward state if they wanted to.
 */
void Analysis::computeOrigins() {
    loanCount = body.loans.size();
    loanWords = U32((loanCount + 63) / 64);

    originLocal.clear();
    nodeRow.clear();
    originLocalCount = 0;
    localRowCount = 0;
    if(!loanCount) return;

    for(U32 i = 0; i < body.locals.size(); i++) {
        auto local = body.local(base, LocalId(i));
        originLocal.push(carriesOrigins(decl, local->occurrence) ? originLocalCount++
                                                                 : kNoOriginLocal);
    }

    buildLocalRows();

    valueOrigins.reset(body.nodes.size(), loanCount);
    nodeLocals.resetWords(localRowCount, localWidth());
    localIn.resetWords(blockCount, localWidth());
    localOut.resetWords(blockCount, localWidth());

    auto width = localWidth();

    // The running state, refilled from the block's predecessors at the top of each block.
    SmallArray<U64, 16> state;
    for(Size i = 0; i < width; i++) state.push(0);

    sweepForward([&](U32 index) {
        auto block = this->block(index);
        auto count = block->nodes.size();

        /*
         * The block's entry state: the union over its predecessors' exits.
         *
         * The entry block's is empty, and that is a statement rather than an initialization - a
         * parameter's storage belongs to the caller and is rooted in no loan of *this* body, which
         * is exactly what `checkReturnConformance` reads the parameter's own contract for.
         */
        BitMatrix::zero(state.pointer(), width);

        for(auto predecessorPointer: block->incoming.contents(base)) {
            auto from = localsAt(localOut, base[predecessorPointer]->index.index);
            BitMatrix::unionInto(state.pointer(), from, width);
        }

        BitMatrix::unionInto(localsAt(localIn, index), state.pointer(), width);

        auto lastRow = kNoRow;

        for(U32 position = 0; position < count; position++) {
            auto& node = nodeAt(index, position);

            /*
             * Published before the node's own rules run, because they ask `originsOfPlace` for this
             * very row: the operands of a node read the state on entry to it. Once per *run* rather
             * than once per node - every node in a run reads one row, and unioning the same state
             * into it again at each of them would be the same answer arrived at repeatedly.
             */
            auto row = nodeRow[node.id.index];
            if(row != lastRow) {
                BitMatrix::unionInto(nodeLocals.row(row), state.pointer(), width);
                lastRow = row;
            }

            switch(node.kind) {
                case Node::Read: {
                    auto& read = (NodeRead&)node;
                    auto into = originsOfValue(node.id);

                    if(read.loan.valid()) BitMatrix::set(into, read.loan.index);

                    /*
                     * What the storage was rooted in, where the value that comes out of it can
                     * still refer to that storage.
                     *
                     * A borrow inherits its lender's origins, which is what makes a reborrow carry
                     * the outer loan along. A *copy* or a *move* of a record with an external field
                     * inherits them too, and that is the half a rule about borrowing alone would
                     * miss: what travels is the record, and the reference inside it goes on
                     * referring to the same object.
                     *
                     * `carriesOrigins` on the **result** is what separates those from the case that
                     * inherits nothing: an `I32` copied out of a borrowed record is a freshly rooted
                     * value, and a read that passed the lender's origins on to it would make
                     * `fn depth(r: a'Reader) -> I32 = r.at` a function returning a reference to its
                     * argument.
                     */
                    if(carriesOrigins(decl, asResult(node).occurrence)) {
                        if(auto from = originsOfPlace(node.id, read.place.placeId())) {
                            BitMatrix::unionInto(into, from, loanWords);
                        }
                    }

                    break;
                }

                // A record is rooted in whatever its components were - §5.11's "a constructor
                // maps component slots into result slots", with one set rather than one per slot.
                case Node::Aggregate: {
                    auto& built = (NodeAggregate&)node;
                    auto into = originsOfValue(node.id);
                    auto components = built.components.contents(base);

                    for(U32 i = 0; i < components.size(); i++) {
                        if(auto from = originsOfOperand(node.id, *components.pointerAt(i))) {
                            BitMatrix::unionInto(into, from, loanWords);
                        }
                    }

                    break;
                }

                case Node::Call: {
                    auto& call = (NodeCall&)node;
                    auto& callee = *decl[call.callee];
                    if(callee.result.contract.origin() != Origin::Slot) break;

                    /*
                     * The slot the result names, bound to what filled it.
                     *
                     * Every argument whose parameter publishes the same slot contributes, and the
                     * result may derive from any of them - §2.2's `choose`, whose result binds
                     * `source` to the union and keeps both alive. Deliberately conservative, and
                     * deliberately not a claim that the members are one object.
                     */
                    auto slot = callee.result.contract.slot();
                    auto into = originsOfValue(node.id);
                    auto arguments = call.arguments.contents(base);

                    for(U32 i = 0; i < arguments.size(); i++) {
                        auto& parameter = *decl[callee.args.get(decl, i)];
                        auto contract = parameter.occurrence.contract;
                        if(contract.origin() != Origin::Slot || contract.slot() != slot) continue;

                        if(auto from = originsOfOperand(node.id, *arguments.pointerAt(i))) {
                            BitMatrix::unionInto(into, from, loanWords);
                        }
                    }

                    break;
                }

                default:
                    break;
            }

            // Storage last, so that a write's own value operand was read under the state the node
            // started in - `stepOriginsForward` is the only thing that changes it.
            stepOriginsForward(state.pointer(), node);
        }

        auto moved = BitMatrix::unionInto(localsAt(localOut, index), state.pointer(), width);

        // The edges, which is where a block parameter's set comes from: the operand a predecessor
        // supplied is what the parameter holds on that path, so the parameter is rooted in the union
        // over the predecessors.
        if(count) {
            auto last = base[block->nodes.get(base, count - 1)];
            forEachEdge(*last, [&](Edge& edge) {
                if(!edge.target) return;

                auto target = base[edge.target];
                auto supplied = edge.arguments.contents(base);
                auto changed = false;

                for(U32 i = 0; i < supplied.size() && i < target->arguments.size(); i++) {
                    auto parameter = base[target->arguments.get(base, i)];
                    if(auto from = originsOfOperand(last->id, *supplied.pointerAt(i))) {
                        changed |= BitMatrix::unionInto(originsOfValue(parameter->id), from,
                                                        loanWords);
                    }
                }

                // The target reads its own parameters, so a parameter that gained an origin is a
                // reason to visit the target again even where nothing about this block's storage
                // moved.
                if(changed) wake(target->index.index);
            });
        }

        if(moved) wakeSuccessors(index);
    });
}

/*
 * How long each loan lasts - the backward half, and the same table read the other way.
 *
 * A loan is live wherever a value rooted in it still has a use ahead of it, and it does not exist
 * before the read that took it. That is the whole transfer: kill at the creation, generate at every
 * use of a value the environment says is rooted in it. Nothing here knows what a borrow is *for*,
 * which is the point - a loan handed to a call, stored in a binding, carried around a loop and
 * returned all reach this as the same statement about uses.
 */
void Analysis::computeLoanExtents() {
    if(!loanCount) return;

    loanIn.reset(blockCount, loanCount);
    loanOut.reset(blockCount, loanCount);

    SmallArray<U64, 4> live;
    for(U32 w = 0; w < loanWords; w++) live.push(0);

    sweepBackward([&](U32 index) {
        auto block = this->block(index);
        BitMatrix::zero(live.pointer(), loanWords);

        auto count = block->nodes.size();
        if(count) {
            auto last = base[block->nodes.get(base, count - 1)];
            forEachEdge(*last, [&](Edge& edge) {
                if(!edge.target) return;

                auto target = loansAt(loanIn, base[edge.target]->index.index);
                BitMatrix::unionInto(live.pointer(), target, loanWords);
            });
        }

        BitMatrix::replace(loansAt(loanOut, index), live.pointer(), loanWords);

        for(U32 n = count; n > 0; n--) stepLoansBackward(live.pointer(), nodeAt(index, n - 1));

        if(BitMatrix::replace(loansAt(loanIn, index), live.pointer(), loanWords)) {
            wakePredecessors(index);
        }
    });
}

void Analysis::stepLoansBackward(U64* live, Node& node) {
    // The loan this node took does not exist before it. Killed first, so that a read whose own
    // result is its only use leaves nothing live above it.
    if(node.kind == Node::Read) {
        auto& read = (NodeRead&)node;
        if(read.loan.valid()) BitMatrix::clear(live, read.loan.index);
    }

    /*
     * Every operand, values and places alike - see `originsOfOperand`. A loan bound to a name is
     * still in use where the *name* is read, and a rule that looked only at values ended its extent
     * at the binding that stored it.
     *
     * The one operand that is not a use is the destination of a write that **replaces** what is in
     * this frame's own storage: what was there is discarded unread, so the loans it carried end
     * above the write rather than at it. That is the same statement `stepOriginsForward` makes going
     * the other way - one node, one decision about whether the old value survives it - and it is
     * what makes the flow-sensitive state worth having: without it, refilling a reader kept the
     * displaced lender borrowed for exactly one more node, which is where the next write to it is.
     *
     * A *partial* write and a write through a loan are both uses, and the same condition says so:
     * `r.at = 3` reaches the value through `r`, and `d = box(2)` on a `&` binding reaches `b`
     * through `d`. Neither discards what the local refers to.
     */
    auto replaced = replacedLocal(node);

    forEachOperand(base, node, [&](Operand& operand, OperandRole role) {
        if(role == OperandRole::WriteTarget && replaced) return;

        auto origins = originsOfOperand(node.id, operand);
        if(!origins) return;

        BitMatrix::unionInto(live, origins, loanWords);
    });
}

namespace {

/*
 * What one access is asking to do to storage - the input to the conflict rules.
 *
 * Three, and the third is not a mistake: reading a `TrivialCopy` value out through a live exclusive
 * loan is legal, because "producing a copy is a different operation from handing out another
 * borrow, and does not require suspending exclusivity". So a copy is an access this walk sees and
 * deliberately does nothing about, and saying so by name is what stops it from being added later
 * by somebody who noticed the gap.
 */
enum class AccessKind: U8 {
    TakeLoan,
    Write,
    Copy,
};

} // namespace

// The overlap and conformance checks - §6.4, over settled extents.
bool Analysis::checkLoans() {
    if(!loanCount) return errors == 0;

    /*
     * Which loans each loan's place can reach, through the origin environment - the expansion this
     * walk used to rebuild from scratch at every comparison.
     *
     * A loan of `d` where `let d = c` reaches `c`, and a check that compared the two places alone
     * would let `c` be written while `d` is live. That expansion is a transitive closure over the
     * loans, it is the same closure for every comparison made at one node, and rebuilding it per
     * comparison is what made this walk cubic in the loan count: the old shape asked `underlying`
     * twice per live loan per access, and each of those scanned every loan at every step.
     *
     * Per *node* and not per body, because `originsOfPlace` is a question about a program point -
     * which is the whole reason `nodeLocals` is flow-sensitive.
     */
    BitMatrix closure;
    closure.reset(loanCount, loanCount);

    SmallArray<U64, 4> scratch, reachedSet, overlapping;
    for(U32 w = 0; w < loanWords; w++) { scratch.push(0); reachedSet.push(0); overlapping.push(0); }

    auto closureAt = ~U32(0);

    auto buildClosure = [&](NodeId at) {
        if(closureAt == at.index) return;
        closureAt = at.index;

        for(U32 l = 0; l < loanCount; l++) {
            auto into = closure.row(l);
            auto from = originsOfPlace(at, body.loan(base, LoanId(l))->place);

            if(from) BitMatrix::copy(into, from, loanWords);
            else BitMatrix::zero(into, loanWords);
        }

        /*
         * One round per level of reborrowing, and a body has one or two.
         *
         * The snapshot is what makes the walk over a row safe while the row is being unioned into:
         * a loan added this round is picked up by the next one, which is the ordinary way a closure
         * is taken and the reason the loop tests for a change rather than counting rounds.
         */
        for(auto changed = true; changed; ) {
            changed = false;

            for(U32 l = 0; l < loanCount; l++) {
                auto into = closure.row(l);
                BitMatrix::copy(scratch.pointer(), into, loanWords);

                BitMatrix::forEach(scratch.pointer(), loanWords, [&](Size m) {
                    changed |= BitMatrix::unionInto(into, closure.row(m), loanWords);
                });
            }
        }
    };

    /*
     * Every place one access may actually reach - the place itself, and the place of every loan the
     * storage behind it is rooted in, transitively.
     *
     * The projection is dropped rather than rebased, which is conservative and is the safe
     * direction: `d.left` where `let d = c` is reported as reaching all of `c` rather than `c.left`,
     * so two disjoint fields reached through one binding conflict when they need not. Rebasing needs
     * the interned place `c.left`, and the builder that interns places is gone by the time this runs
     * - §5.2 is explicit that the alias rule is conservative, and this is one of the places it is.
     */
    auto reachFrom = [&](NodeId at, PlaceId place, SmallArray<PlaceId, 8>& into) {
        into.clear();
        into.push(place);

        BitMatrix::zero(reachedSet.pointer(), loanWords);

        if(auto from = originsOfPlace(at, place)) {
            BitMatrix::copy(reachedSet.pointer(), from, loanWords);
            BitMatrix::forEach(from, loanWords, [&](Size m) {
                BitMatrix::unionInto(reachedSet.pointer(), closure.row(m), loanWords);
            });
        }

        BitMatrix::forEach(reachedSet.pointer(), loanWords, [&](Size m) {
            into.push(body.loan(base, LoanId(m))->place);
        });
    };

    SmallArray<PlaceId, 8> reached;

    /*
     * One access, against every loan that is still live after it.
     *
     * A loan that dies at this node does not extend past it, so `live` is the set *after* the node -
     * which is also what makes a loan created here not conflict with itself. `skip` is what the
     * access reborrows: writing through a `&` binding is what the binding is for, and a reborrow of
     * a loan is not a conflict with the loan it reborrows (§2.5).
     *
     * The overlap is decided in two halves, and that is what keeps the comparison out of the loop.
     * `overlapping` is "whose own place does this access touch", asked once of every loan against
     * the places the access reaches; and a loan conflicts when it is in that set or when anything it
     * reaches is - which is a word-at-a-time test against the closure rather than a second walk over
     * two place lists.
     */
    auto check = [&](Node& node, PlaceId place, AccessKind kind, const U64* live, const U64* skip) {
        if(kind == AccessKind::Copy) return;
        if(!BitMatrix::any(live, loanWords)) return;

        buildClosure(node.id);
        reachFrom(node.id, place, reached);

        BitMatrix::zero(overlapping.pointer(), loanWords);

        for(U32 l = 0; l < loanCount; l++) {
            auto other = body.loan(base, LoanId(l))->place;

            for(auto x: reached) {
                if(!placesOverlap(base, body, x, other)) continue;

                BitMatrix::set(overlapping.pointer(), l);
                break;
            }
        }

        for(U32 l = 0; l < loanCount; l++) {
            if(!BitMatrix::test(live, l)) continue;
            if(skip && BitMatrix::test(skip, l)) continue;

            auto& other = *body.loan(base, LoanId(l));

            // Any number of shared loans may coexist. Everything else - a unique loan meeting any
            // overlapping loan, and a write or a move meeting either - conflicts.
            if(kind == AccessKind::TakeLoan) {
                auto taking = ((NodeRead&)node).loan;
                if(taking.index == l) continue;

                auto capability = body.loan(base, taking)->capability;
                if(capability == Access::Shared && other.capability == Access::Shared) continue;
            }

            if(!BitMatrix::test(overlapping.pointer(), l) &&
               !BitMatrix::intersects(overlapping.pointer(), closure.row(l), loanWords)) {
                continue;
            }

            auto here = placeText(context, decl, base, body, *body.place(base, place));
            auto there = placeText(context, decl, base, body, *body.place(base, other.place));
            auto exclusive = other.capability == Access::Unique;

            if(kind == AccessKind::Write) {
                report(node.source,
                       exclusive
                           ? "%@ cannot be written here, because %@ is borrowed exclusively and that borrow is still in use"_v
                           : "%@ cannot be written here, because %@ is borrowed and that borrow is still in use"_v,
                       here, there);
            } else {
                report(node.source,
                       exclusive
                           ? "%@ cannot be borrowed here, because %@ is already borrowed exclusively and that borrow is still in use"_v
                           : "%@ cannot be borrowed exclusively here, because %@ is already borrowed and that borrow is still in use"_v,
                       here, there);
            }

            return;
        }
    };

    /*
     * One forward walk with the live-loan set at every position, which is recomputed from the
     * block's live-out by stepping the same backward transfer the fixpoint used. That is the §8.6
     * trade `walkBlock` makes for the same reason: a table of blocks times positions times loans
     * would be the largest thing this analysis allocates, for an answer each position uses once.
     */
    BitMatrix after;
    SmallArray<U64, 4> live;
    for(U32 w = 0; w < loanWords; w++) live.push(0);

    for(U32 index = 0; index < blockCount; index++) {
        auto block = this->block(index);
        auto count = block->nodes.size();
        if(!count) continue;

        after.reset(count, loanCount);
        BitMatrix::copy(live.pointer(), loansAt(loanOut, index), loanWords);

        for(U32 n = count; n > 0; n--) {
            auto position = n - 1;
            BitMatrix::copy(after.row(position), live.pointer(), loanWords);
            stepLoansBackward(live.pointer(), nodeAt(index, position));
        }

        for(U32 position = 0; position < count; position++) {
            auto& node = nodeAt(index, position);
            auto at = after.row(position);

            switch(node.kind) {
                case Node::Read: {
                    auto& read = (NodeRead&)node;
                    auto place = read.place.placeId();

                    switch(read.place.mode()) {
                        case UseMode::BorrowShared:
                        case UseMode::BorrowUnique:
                            check(node, place, AccessKind::TakeLoan, at, originsOfValue(node.id));
                            break;

                        // A move out of storage is a write to it: what was there is gone, and a loan
                        // of it would be left pointing at a hole.
                        case UseMode::Take:
                            check(node, place, AccessKind::Write, at,
                                  originsOfPlace(node.id, place));
                            break;

                        case UseMode::CopyTrivial:
                        case UseMode::Inspect:
                        case UseMode::Forward:
                        case UseMode::Count:
                            check(node, place, AccessKind::Copy, at, nullptr);
                            break;
                    }

                    break;
                }

                case Node::Write: {
                    auto& write = (NodeWrite&)node;
                    if(write.destination.source() != SourceKind::Place) break;

                    // An `Init` establishes a slot rather than writing through one, so there is
                    // nothing in it for a loan to be pointing at.
                    if(write.mode == Destination::Init) break;

                    auto place = write.destination.placeId();
                    check(node, place, AccessKind::Write, at, originsOfPlace(node.id, place));
                    break;
                }

                case Node::Return:
                    checkReturnConformance(node);
                    break;

                default:
                    break;
            }
        }
    }

    return errors == 0;
}

/*
 * The origin environment, into the region - the words as they are, not a byte per loan.
 *
 * This is the largest table the analysis publishes, and the one thing that keeps it from being
 * scratch is that the conversion reads it: `attachLoan` asks what each borrowed value is rooted in,
 * because the origin environment is the only honest answer to that and the OSSA loan it mints is
 * built from it. The dump reads it too, which is what a golden showing loans and not what a value
 * is rooted in could not distinguish.
 */
void Analysis::publishOrigins() {
    out.loanCount = loanCount;
    out.loanWords = loanWords;
    if(!loanCount) return;

    auto nodes = body.nodes.size();
    out.origins.reserve(program.mir, nodes * loanWords);

    for(U32 i = 0; i < nodes; i++) {
        auto row = valueOrigins.row(i);
        for(U32 w = 0; w < loanWords; w++) out.origins.push(program.mir, row[w]);
    }
}

/*
 * Signature conformance - §6.4's "return and destination slots must conform to declared loan
 * groups", and Analysis-Borrows-Revised.md §2.2's rule that a signature states the relationship on
 * both ends.
 *
 * The declaration half is already checked: `resolveOccurrence` refuses a result that names an origin
 * no parameter published. What is left is the body half - that every loan the returned value is
 * actually rooted in is one the declared slot promised - and it is a question about each loan's
 * *place*, because where a loan is rooted is where its storage lives.
 *
 * Three answers and each is a different mistake. A global outlives the program, so it satisfies any
 * slot. A parameter in the declared slot is what the signature promised. Anything else - this
 * frame's own storage, or a parameter in a different slot, or one whose loan the signature did not
 * publish at all - is a reference the caller cannot be told the extent of.
 */
void Analysis::checkReturnConformance(Node& node) {
    auto& written = (NodeReturn&)node;
    if(!written.hasValue) return;

    auto declared = function.result.contract;
    if(written.value.source() != SourceKind::Result) return;

    auto origins = originsOfValue(written.value.nodeId());

    /*
     * The first loan that fails, and then nothing more from this `return`.
     *
     * A value's origin set holds every loan it may be rooted in, and one mistake usually puts
     * several in it: `return r.held` on a reader this frame built is the loan of the guard *and* the
     * loan of the field that holds it, which are one error about one statement. Reporting each would
     * be the pass counting its own intermediate values at the author.
     */
    for(U32 l = 0; l < loanCount; l++) {
        if(!BitMatrix::test(origins, l)) continue;

        auto& loan = *body.loan(base, LoanId(l));
        auto root = rootOf(loan.place);

        // A global outlives the program, so a reference into one satisfies any slot and needs no
        // relationship published for it.
        if(root->kind == Place::Global) continue;

        auto named = placeText(context, decl, base, body, *body.place(base, loan.place));

        /*
         * An **owned** result that nonetheless refers to something - a record with an external
         * field, handed back.
         *
         * The signature has nowhere to say whose storage it refers to: the relationship is a loan
         * argument on the result's own type, `-> Reader(a')`, which the parser has a kind for and
         * `resolveType` does not read yet. Refused by name, because the alternative is a value the
         * caller receives with a dangling field and no diagnostic anywhere.
         */
        if(declared.origin() != Origin::Slot) {
            report(node.source,
                   "this result refers to %@, and its type has no origin to say so - not implemented"_v,
                   named);
            return;
        }

        if(root->kind != Place::Local) {
            report(node.source, "%@ is not storage this function can publish the extent of"_v, named);
            return;
        }

        auto local = body.local(base, LocalId(root->payload));
        if(!local->isParameter()) {
            report(node.source,
                   "%@ is this function's own storage, so a borrow of it cannot outlive the call"_v,
                   named);
            return;
        }

        auto granted = decl[function.args.get(decl, local->parameter)]->occurrence.contract;
        if(granted.origin() == Origin::Slot && granted.slot() == declared.slot()) continue;

        if(granted.origin() != Origin::Slot) {
            report(node.source,
                   "%@ is borrowed for the length of this call, so a borrow of it cannot be returned - publish an origin on it with `'`"_v,
                   named);
            return;
        }

        report(node.source, "%@ is in a different origin from the one the result names"_v, named);
        return;
    }
}

} // namespace mir
