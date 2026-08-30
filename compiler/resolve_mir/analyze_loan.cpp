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
U8* Analysis::originsOfOperand(Operand operand) {
    if(!loanCount) return nullptr;
    if(operand.source() == SourceKind::Result) return originsOfValue(operand.nodeId());
    if(operand.source() == SourceKind::Place) return originsOfPlace(operand.placeId());
    return nullptr;
}

U8* Analysis::originsOfPlace(PlaceId place) {
    if(!loanCount) return nullptr;

    auto root = rootOf(place);
    if(root->kind != Place::Local) return nullptr;

    auto local = body.local(base, LocalId(root->payload));
    if(!carriesOrigins(decl, local->occurrence)) return nullptr;

    return originsOfLocal(local->index);
}

namespace {

// `into |= from`, answering whether anything changed - the one operation the two fixpoints below
// are made of.
bool addOrigins(U8* into, const U8* from, U32 count) {
    auto changed = false;
    for(U32 i = 0; i < count; i++) {
        if(!from[i] || into[i]) continue;
        into[i] = 1;
        changed = true;
    }

    return changed;
}

} // namespace

/*
 * What every value is rooted in - §5.11's `OriginEnvironment`, as a forward fixpoint.
 *
 * A fixpoint rather than one pass because a local's origin set is the union over every write into
 * it and a loop may write one after it has already been read. The lattice is a finite set of loans
 * and every step only adds, so it settles.
 *
 * Five rules, and each is one sentence:
 *
 *  - a borrowing read is rooted in the loan it took, and in whatever the storage it read was rooted
 *    in - which is what makes a reborrow carry its lender along;
 *  - a call whose result is a published loan is rooted in whatever filled that slot, which is
 *    §2.2's "at a call, a slot is bound to a set of concrete origins";
 *  - a block parameter is rooted in whatever every incoming edge supplied;
 *  - storage that holds a loan is rooted in whatever was written into it;
 *  - everything else produces an owner, which is rooted in nothing.
 */
void Analysis::computeOrigins() {
    loanCount = body.loans.size();

    valueOrigins.clear();
    localOrigins.clear();
    if(!loanCount) return;

    for(U32 i = 0; i < body.nodes.size() * loanCount; i++) valueOrigins.push(0);
    for(U32 i = 0; i < body.locals.size() * loanCount; i++) localOrigins.push(0);

    for(auto changed = true; changed; ) {
        changed = false;

        for(U32 index = 0; index < blockCount; index++) {
            auto block = this->block(index);
            auto count = block->nodes.size();

            for(U32 position = 0; position < count; position++) {
                auto& node = nodeAt(index, position);

                switch(node.kind) {
                    case Node::Read: {
                        auto& read = (NodeRead&)node;
                        auto into = originsOfValue(node.id);

                        if(read.loan.valid() && !into[read.loan.index]) {
                            into[read.loan.index] = 1;
                            changed = true;
                        }

                        /*
                         * What the storage was rooted in, where the value that comes out of it can
                         * still refer to that storage.
                         *
                         * A borrow inherits its lender's origins, which is what makes a reborrow
                         * carry the outer loan along. A *copy* or a *move* of a record with an
                         * external field inherits them too, and that is the half a rule about
                         * borrowing alone would miss: what travels is the record, and the reference
                         * inside it goes on referring to the same object.
                         *
                         * `carriesOrigins` on the **result** is what separates those from the case
                         * that inherits nothing: an `I32` copied out of a borrowed record is a
                         * freshly rooted value, and a read that passed the lender's origins on to it
                         * would make `fn depth(r: a'Reader) -> I32 = r.at` a function returning a
                         * reference to its argument.
                         */
                        if(carriesOrigins(decl, asResult(node).occurrence)) {
                            if(auto from = originsOfPlace(read.place.placeId())) {
                                changed |= addOrigins(into, from, loanCount);
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
                            if(auto from = originsOfOperand(*components.pointerAt(i))) {
                                changed |= addOrigins(into, from, loanCount);
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
                         * Every argument whose parameter publishes the same slot contributes, and
                         * the result may derive from any of them - §2.2's `choose`, whose result
                         * binds `source` to the union and keeps both alive. Deliberately
                         * conservative, and deliberately not a claim that the members are one
                         * object.
                         */
                        auto slot = callee.result.contract.slot();
                        auto into = originsOfValue(node.id);
                        auto arguments = call.arguments.contents(base);

                        for(U32 i = 0; i < arguments.size(); i++) {
                            auto& parameter = *decl[callee.args.get(decl, i)];
                            auto contract = parameter.occurrence.contract;
                            if(contract.origin() != Origin::Slot || contract.slot() != slot) continue;

                            if(auto from = originsOfOperand(*arguments.pointerAt(i))) {
                                changed |= addOrigins(into, from, loanCount);
                            }
                        }

                        break;
                    }

                    case Node::Write: {
                        auto& write = (NodeWrite&)node;
                        if(write.destination.source() != SourceKind::Place) break;

                        auto place = body.place(base, write.destination.placeId());
                        if(place->parent || place->kind != Place::Local) break;
                        if(!carriesOrigins(decl, place->occurrence)) break;

                        auto local = body.local(base, LocalId(place->payload));
                        if(auto from = originsOfOperand(write.value)) {
                            changed |= addOrigins(originsOfLocal(local->index), from, loanCount);
                        }

                        break;
                    }

                    default:
                        break;
                }
            }

            // The edges, which is where a block parameter's set comes from: the operand a
            // predecessor supplied is what the parameter holds on that path, so the parameter is
            // rooted in the union over the predecessors.
            if(!count) continue;

            auto last = base[block->nodes.get(base, count - 1)];
            forEachEdge(*last, [&](Edge& edge) {
                if(!edge.target) return;

                auto target = base[edge.target];
                auto supplied = edge.arguments.contents(base);

                for(U32 i = 0; i < supplied.size() && i < target->arguments.size(); i++) {
                    auto parameter = base[target->arguments.get(base, i)];
                    if(auto from = originsOfOperand(*supplied.pointerAt(i))) {
                        changed |= addOrigins(originsOfValue(parameter->id), from, loanCount);
                    }
                }
            });
        }
    }
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
    loanIn.clear();
    loanOut.clear();
    if(!loanCount) return;

    for(U32 i = 0; i < blockCount * loanCount; i++) { loanIn.push(0); loanOut.push(0); }

    for(auto changed = true; changed; ) {
        changed = false;

        for(U32 i = blockCount; i > 0; i--) {
            auto index = i - 1;
            auto block = this->block(index);

            SmallArray<U8, 64> live;
            for(U32 l = 0; l < loanCount; l++) live.push(0);

            auto count = block->nodes.size();
            if(count) {
                auto last = base[block->nodes.get(base, count - 1)];
                forEachEdge(*last, [&](Edge& edge) {
                    if(!edge.target) return;

                    auto target = loansAt(loanIn, base[edge.target]->index.index);
                    for(U32 l = 0; l < loanCount; l++) live[l] |= target[l];
                });
            }

            auto out = loansAt(loanOut, index);
            for(U32 l = 0; l < loanCount; l++) {
                if(out[l] == live[l]) continue;
                out[l] = live[l];
                changed = true;
            }

            for(U32 n = count; n > 0; n--) stepLoansBackward(live.pointer(), nodeAt(index, n - 1));

            auto in = loansAt(loanIn, index);
            for(U32 l = 0; l < loanCount; l++) {
                if(in[l] == live[l]) continue;
                in[l] = live[l];
                changed = true;
            }
        }
    }
}

void Analysis::stepLoansBackward(U8* live, Node& node) {
    // The loan this node took does not exist before it. Killed first, so that a read whose own
    // result is its only use leaves nothing live above it.
    if(node.kind == Node::Read) {
        auto& read = (NodeRead&)node;
        if(read.loan.valid()) live[read.loan.index] = 0;
    }

    // Every operand, values and places alike - see `originsOfOperand`. A loan bound to a name is
    // still in use where the *name* is read, and a rule that looked only at values ended its extent
    // at the binding that stored it.
    forEachOperand(base, node, [&](Operand& operand, OperandRole) {
        auto origins = originsOfOperand(operand);
        if(!origins) return;

        for(U32 l = 0; l < loanCount; l++) live[l] |= origins[l];
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

/*
 * The overlap and conformance checks - §6.4, over settled extents.
 *
 * One forward walk with the live-loan set at every position, which is recomputed from the block's
 * live-out by stepping the same backward transfer the fixpoint used. That is the §8.6 trade
 * `walkBlock` makes for the same reason: a table of blocks times positions times loans would be the
 * largest thing this analysis allocates, for an answer each position uses once.
 */
bool Analysis::checkLoans() {
    if(!loanCount) return errors == 0;

    // Every place a loan may actually reach, expanded through the storage it was taken out of. A
    // loan of `d` where `let d = c` reaches `c`, and a check that compared the two places alone
    // would let `c` be written while `d` is live.
    SmallArray<U8, 64> seen;
    for(U32 l = 0; l < loanCount; l++) seen.push(0);

    auto underlying = [&](PlaceId place, SmallArray<PlaceId, 8>& into) {
        for(U32 l = 0; l < loanCount; l++) seen[l] = 0;

        SmallArray<PlaceId, 8> pending;
        pending.push(place);

        while(pending.size()) {
            auto current = pending.pop().unwrap();
            into.push(current);

            auto origins = originsOfPlace(current);
            if(!origins) continue;

            /*
             * The projection is dropped rather than rebased, which is conservative and is the safe
             * direction: `d.left` where `let d = c` is reported as reaching all of `c` rather than
             * `c.left`, so two disjoint fields reached through one binding conflict when they need
             * not. Rebasing needs the interned place `c.left`, and the builder that interns places
             * is gone by the time this runs - §5.2 is explicit that the alias rule is conservative,
             * and this is one of the places it is.
             */
            for(U32 l = 0; l < loanCount; l++) {
                if(!origins[l] || seen[l]) continue;
                seen[l] = 1;
                pending.push(body.loan(base, LoanId(l))->place);
            }
        }
    };

    auto mayReach = [&](PlaceId a, PlaceId b) {
        SmallArray<PlaceId, 8> left, right;
        underlying(a, left);
        underlying(b, right);

        for(auto x: left) {
            for(auto y: right) {
                if(placesOverlap(base, body, x, y)) return true;
            }
        }

        return false;
    };

    /*
     * One access, against every loan that is still live after it.
     *
     * A loan that dies at this node does not extend past it, so `live` is the set *after* the node -
     * which is also what makes a loan created here not conflict with itself. `skip` is what the
     * access reborrows: writing through a `&` binding is what the binding is for, and a reborrow of
     * a loan is not a conflict with the loan it reborrows (§2.5).
     */
    auto check = [&](Node& node, PlaceId place, AccessKind kind, U8* live, const U8* skip) {
        if(kind == AccessKind::Copy) return;

        for(U32 l = 0; l < loanCount; l++) {
            if(!live[l] || (skip && skip[l])) continue;

            auto& other = *body.loan(base, LoanId(l));

            // Any number of shared loans may coexist. Everything else - a unique loan meeting any
            // overlapping loan, and a write or a move meeting either - conflicts.
            if(kind == AccessKind::TakeLoan) {
                auto taking = ((NodeRead&)node).loan;
                if(taking.index == l) continue;

                auto capability = body.loan(base, taking)->capability;
                if(capability == Access::Shared && other.capability == Access::Shared) continue;
            }

            if(!mayReach(place, other.place)) continue;

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

    for(U32 index = 0; index < blockCount; index++) {
        auto block = this->block(index);
        auto count = block->nodes.size();

        SmallArray<U8, 64> after;
        for(U32 i = 0; i < count * loanCount; i++) after.push(0);

        {
            SmallArray<U8, 64> live;
            auto out = loansAt(loanOut, index);
            for(U32 l = 0; l < loanCount; l++) live.push(out[l]);

            for(U32 n = count; n > 0; n--) {
                auto position = n - 1;
                for(U32 l = 0; l < loanCount; l++) after[position * loanCount + l] = live[l];
                stepLoansBackward(live.pointer(), nodeAt(index, position));
            }
        }

        for(U32 position = 0; position < count; position++) {
            auto& node = nodeAt(index, position);
            auto live = after.pointer() + position * loanCount;

            switch(node.kind) {
                case Node::Read: {
                    auto& read = (NodeRead&)node;
                    auto place = read.place.placeId();

                    switch(read.place.mode()) {
                        case UseMode::BorrowShared:
                        case UseMode::BorrowUnique:
                            check(node, place, AccessKind::TakeLoan, live,
                                  originsOfValue(node.id));
                            break;

                        // A move out of storage is a write to it: what was there is gone, and a loan
                        // of it would be left pointing at a hole.
                        case UseMode::Take:
                            check(node, place, AccessKind::Write, live, originsOfPlace(place));
                            break;

                        case UseMode::CopyTrivial:
                        case UseMode::Inspect:
                        case UseMode::Forward:
                        case UseMode::Count:
                            check(node, place, AccessKind::Copy, live, nullptr);
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
                    check(node, place, AccessKind::Write, live, originsOfPlace(place));
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

void Analysis::publishOrigins() {
    out.loanCount = loanCount;
    if(!loanCount) return;

    auto entries = body.nodes.size() * loanCount;
    out.origins.reserve(program.mir, entries);
    for(U32 i = 0; i < entries; i++) out.origins.push(program.mir, valueOrigins[i]);
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
        if(!origins[l]) continue;

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
