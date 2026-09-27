#include "analyze_internal.h"
#include "print.h"
#include "select.h"

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

bool placesOverlap(MirBase base, DeclBase decl, Body& body, PlaceId a, PlaceId b) {
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

        /*
         * Two fields of one record are two objects; an index or a dereference may be the same one,
         * and §5.2 does not claim to prove otherwise.
         *
         * A **property** is a field whose index is not known here, and the disjointness argument
         * survives that unchanged: two requirements name two components, because a requirement is
         * about one declared field and a subject cannot satisfy two of them with one. What it does
         * not survive is comparing a property against a field of the same parent - which cannot
         * arise, since a property's parent is a type variable and a field's is a record.
         */
        if(x->kind != y->kind) return true;
        if(x->kind != Place::Field && x->kind != Place::Property) return true;
        if(x->payload == y->payload) continue;

        /*
         * Two fields of a **sum** are disjoint only inside one constructor.
         *
         * Across two arms they are the same bytes read two ways, so the ordinary "two fields are two
         * objects" argument is exactly what does not hold here - which is the whole difference
         * between a product and a sum, stated where the difference has a consequence. Reaching both
         * needs two matches on one value that cannot both have succeeded, so it is unreachable
         * rather than useful; answering conservatively costs a program nothing and answering
         * structurally would hand out two exclusive borrows of one word.
         */
        auto owner = base[x->parent]->occurrence.type;
        if(x->kind == Place::Field && isSumRecord(decl, owner)) {
            auto& record = *(RecordType*)decl[owner];
            if(record.constructorOf(decl, U16(x->payload)) !=
               record.constructorOf(decl, U16(y->payload))) {
                return true;
            }
        }

        return false;
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
 * A body with no loans has no origin state, and every operation here is a no-op on one.
 *
 * The same condition `forEachOperandOrigin` opens with, and it is not only an optimization:
 * `computeOrigins` returns before it fills anything when there are no loans, so `logStart` is empty
 * and a walk that indexed it would be reading a table nobody wrote. Most bodies borrow nothing.
 */
OriginCursor::OriginCursor(Analysis& analysis): analysis(analysis) {
    for(Size i = 0; i < analysis.localWidth(); i++) buffer.push(0);
}

void OriginCursor::enter(U32 block) {
    if(!analysis.loanCount) return;

    blockIndex = block;
    entry = analysis.logStart[block];

    auto width = analysis.localWidth();
    if(width) BitMatrix::copy(state(), analysis.localsAt(analysis.localIn, block), width);
}

void OriginCursor::enterAtExit(U32 block) {
    if(!analysis.loanCount) return;

    blockIndex = block;
    entry = analysis.logStart[block + 1];

    auto width = analysis.localWidth();
    if(width) BitMatrix::copy(state(), analysis.localsAt(analysis.localOut, block), width);
}

void OriginCursor::undo(U32 position) {
    if(!analysis.loanCount) return;

    auto first = analysis.logStart[blockIndex];

    while(entry > first && analysis.log[entry - 1].position == position) {
        entry--;

        auto& edit = analysis.log[entry];
        auto into = state() + Size(edit.row) * analysis.stateWords;
        BitMatrix::copy(into, analysis.logPrevious.row(entry), analysis.stateWords);
    }
}

void Analysis::projectionOf(PlaceId place, Projection& into) {
    into.fields.clear();
    into.opaque = false;

    // The chain, place first. Bodies project a handful of levels, so the array never grows.
    SmallArray<Place*, 8> chain;
    for(auto p = body.place(base, place); p; p = p->parent ? base[p->parent] : nullptr) chain.push(p);

    // Read root-first, which is the order a schema slot's path is written in. Everything past the
    // first `Index` or `Deref` is past the point a path can be compared with, so the run stops
    // there and says so rather than pretending the steps below it are fields of this value.
    for(Size i = chain.size() - 1; i > 0; i--) {
        auto step = chain[i - 1];

        /*
         * The referent of a **loan field** is no step of the path (J127). A schema writes the field
         * and what is inside the object it names as one path - `.0` is `Just`'s loan of the item and
         * `.0.*` the pointee of the `%a'I32` it lends - and the `Deref` is only the compiler reaching
         * through its own indirection (`print.cpp` prints nothing for it). Counted as a step, it made
         * everything below the field the value's root: `it.next()`'s caller-lent pointer, copied out
         * of the payload, came out rooted in the object's loan beside the payload's own, so keeping
         * it across the next `next()` was refused (J125 §5) and `xs` was not what kept it alive.
         */
        if(step->kind == Place::Deref && !into.opaque && chain[i]->kind == Place::Field &&
           !chain[i]->occurrence.contract.isOwned()) {
            continue;
        }

        /*
         * A **property** stops the run, deliberately: what is being built is a path of field indices
         * to compare against a schema's, and a property's payload is a requirement's position rather
         * than an index. Reading it as one would compare two different numbering schemes and match
         * by coincidence.
         */
        if(step->kind != Place::Field || into.opaque) {
            into.opaque = true;
            continue;
        }

        into.fields.push(U16(step->payload));
    }
}

U32 Analysis::slotAt(DeclPtr<OwnershipSchema> schema, ExternalSlot& slot, U32 from) {
    if(!schema) return 0;

    auto& found = *decl[schema];
    auto want = slot.path.size() - from;

    for(U32 j = 0; j < found.slots.size(); j++) {
        auto& other = *decl[found.slots.get(decl, j)];
        if(other.path.size() != want) continue;

        auto same = true;
        for(Size step = 0; step < want; step++) {
            if(other.path.get(decl, step) == slot.path.get(decl, from + step)) continue;

            same = false;
            break;
        }

        if(same) return 1 + j;
    }

    /*
     * No such position, so the root - which is the conservative answer and not a missing one.
     *
     * Claiming a loan at the root says the value itself may refer to that storage, which keeps the
     * lender alive and makes the borrow conflict; claiming it at a slot nobody reads would let it
     * die. The two directions are not symmetric, so the fallback is the one that over-approximates.
     */
    return 0;
}

void Analysis::selectPositions(const Projection& projection, const Occurrence& local,
                               const Occurrence& at, SmallArray<PositionMap, 8>& into, bool writing) {
    if(!originPositions(decl, local)) return;

    auto schema = shapeOf(decl, local);

    /*
     * The local's own root: reaching anything through the local reaches it through whatever the
     * local itself refers to, and what comes out is a value inside that storage.
     *
     * Except where a **read** lands at or inside one of the local's loan slots: what it names is
     * then the slot's object (below), and the local's storage was only the way to it. `e.key` in
     * `for e in pairs(m)` is a window into `m`'s keys, not into the entry record `pairs` handed
     * over from its own frame, and carrying the record's root there made `b = {b | key: e.key}`
     * look like it kept the callee's frame once that root had a call bound (J129). A write keeps
     * the root, and so does every place no slot contains.
     */
    auto inSlot = false;
    if(schema && !writing) {
        auto& slots = *decl[schema];
        for(U32 i = 0; i < slots.slots.size() && !inSlot; i++) {
            auto& slot = *decl[slots.slots.get(decl, i)];
            auto depth = U32(slot.path.size());
            if(depth > projection.fields.size()) continue;

            inSlot = true;
            for(U32 step = 0; step < depth; step++) {
                if(slot.path.get(decl, step) != projection.fields[step]) inSlot = false;
            }
        }
    }

    if(!projection.copiedReferent && !inSlot) into.push(PositionMap { 0, 0 });

    /*
     * The element position of a container of loan-carrying values - `hidesLoans`. The whole local is
     * the one place it corresponds to a position of the value; anything under it - an element, a
     * field of the run - is somewhere inside the elements, so every position of that value is the
     * element position's, in both directions. That over-approximates a `Run`'s scalar fields, which
     * carry nothing to over-approximate.
     */
    if(auto element = elementPosition(decl, local)) {
        if(!projection.fields.size() && !projection.opaque) {
            into.push(PositionMap { U16(element), U16(elementPosition(decl, at)) });
        } else {
            auto count = originPositions(decl, at);
            for(U32 p = 0; p < count; p++) into.push(PositionMap { U16(element), U16(p) });
        }
    }

    if(!schema) return;

    auto& slots = *decl[schema];
    auto target = shapeOf(decl, at);

    // Whether the schema has a slot one pointee step below `slot` - an explicit `%a'T` inside the
    // field - and the value's own pointee position, for the copied-referent case below.
    auto pointeeSlotBelow = [&](OwnershipSchema& schema, ExternalSlot& slot) {
        auto depth = slot.path.size();
        for(U32 j = 0; j < schema.slots.size(); j++) {
            auto& other = *decl[schema.slots.get(decl, j)];
            if(other.path.size() != depth + 1) continue;
            if(!PathStep::isPointee(other.path.get(decl, depth))) continue;

            auto same = true;
            for(Size step = 0; step < depth; step++) {
                if(other.path.get(decl, step) != slot.path.get(decl, step)) same = false;
            }
            if(same) return true;
        }
        return false;
    };

    auto pointeePosition = [&](const Occurrence& value) -> U32 {
        auto shape = shapeOf(decl, value);
        if(!shape) return 0;

        auto& found = *decl[shape];
        for(U32 j = 0; j < found.slots.size(); j++) {
            auto& other = *decl[found.slots.get(decl, j)];
            if(other.path.size() == 1 && PathStep::isPointee(other.path.get(decl, 0))) return 1 + j;
        }
        return 0;
    };

    for(U32 i = 0; i < slots.slots.size(); i++) {
        auto& slot = *decl[slots.slots.get(decl, i)];
        auto depth = U32(slot.path.size());
        auto walked = U32(projection.fields.size());

        auto common = min(depth, walked);
        auto diverged = false;

        for(U32 step = 0; step < common; step++) {
            if(slot.path.get(decl, step) == projection.fields[step]) continue;

            diverged = true;
            break;
        }

        // Two different fields of one record are two positions, and this is the line that says so:
        // `t.right` reaches neither `t.left` nor anything under it.
        if(diverged) continue;

        /*
         * The place is at or inside this slot, so what it names is somebody else's object and the
         * value read out of it is rooted where the slot was pointing - at its own root.
         *
         * A **write** goes the other way, and there the root alone lost what the value refers to
         * (J128): `o.held.ref = values(owned)` through a `&k'Inner` field stored a `%kept'I32`
         * whose loan is at its pointee position, not its root, so `o`'s `held` row gained nothing
         * and `drop (owned)` landed before the read back through `o.held.ref` (7 for 47 under
         * `-no-ir-opt`). Everything the written value refers to is now somewhere inside the object
         * the slot names, which the slot is the one position for - every position of the value
         * lands on it, as the element position's do above. A read keeps the root alone: widening
         * it would put the slot's loans on interior positions of every value read through one.
         */
        if(depth <= walked) {
            if(writing) {
                auto count = originPositions(decl, at);
                for(U32 p = 0; p < count; p++) into.push(PositionMap { U16(1 + i), U16(p) });
            } else if(!projection.copiedReferent) {
                into.push(PositionMap { U16(1 + i), 0 });
            } else if(depth == walked && !pointeeSlotBelow(slots, slot)) {
                /*
                 * A pointer copied out of a loan field whose pointee has **no slot of its own** -
                 * `data RP {r: '%Int}`, where `'%Int` is `'%'Int` (J164): the pointee is in the
                 * field's own origin, so the copy's pointee position is rooted where the field's
                 * loan is. `copiedReferent` takes the root off because the copy does not refer to
                 * the object; what it does refer to was, until this, nowhere - so `let q = r.r`
                 * let the array `q` points into be released before `*q` read it (J162 §5).
                 * Where the schema has a pointee slot below the field (`%a'I32` in `Just`'s
                 * payload, J125) the branch below maps it and this one stays out of the way.
                 */
                auto pointee = pointeePosition(at);
                if(pointee) into.push(PositionMap { U16(1 + i), U16(pointee) });
            }
            continue;
        }

        // The slot is inside the place. Where the walk stayed in this frame's own storage the slot
        // survives as a slot of the value at the place; where it left it, the position is not
        // comparable any more and the root is the conservative answer.
        if(projection.opaque) {
            into.push(PositionMap { U16(1 + i), 0 });
            continue;
        }

        into.push(PositionMap { U16(1 + i), U16(slotAt(target, slot, walked)) });
    }
}

/*
 * Whether a read copies the referent of a loan field out into an owned value - `Projection::
 * copiedReferent`. `it.next()`'s `%a'I32` item read out of `Just`'s payload is the case: a pointer
 * into the caller's `xs`, copied out of the object, and rooted in the object only because the
 * payload's own slot landed on its root (J125 §5). A borrow of the referent does refer to the object,
 * and so does a copy that is itself a view or a guard, which is what the owned result excludes.
 */
bool Analysis::copiedReferent(NodeRead& read, const Occurrence& result) {
    if(read.place.mode() != UseMode::CopyTrivial || !result.contract.isOwned()) return false;

    // A raw pointer only, whose root is the address itself and whose referent is the pointee
    // position. A record's root may be where it keeps what it refers to - `src'shared.Iter` copied
    // out of `Holder.cursor` keeps `text` there and nowhere else (`iter/Signature.yana`), and taking
    // it off moved `drop (text)` above the loop that reads through the copy.
    if(!result.type || decl[result.type]->kind != Type::Pointer) return false;

    auto place = body.place(base, read.place.placeId());
    if(place->kind != Place::Deref || !place->parent) return false;

    auto parent = base[place->parent];
    return parent->kind == Place::Field && !parent->occurrence.contract.isOwned();
}

bool Analysis::operandCarriesOrigins(Operand operand) {
    if(!loanCount) return false;
    if(operand.source() == SourceKind::Result) return valuePositions(operand.nodeId()) != 0;
    if(operand.source() != SourceKind::Place) return false;

    auto root = rootOf(operand.placeId());
    return root->kind == Place::Local && localPositions(LocalId(root->payload)) != 0;
}

void Analysis::unionOperandOrigins(OriginCursor& cursor, Operand operand, U64* into) {
    forEachOperandOrigin(cursor, operand, [&](U32, const U64* row) {
        BitMatrix::unionInto(into, row, loanWords);
    });
}

/*
 * Which loans a **place** may reach - the storage this body names, plus what an address says it
 * points into.
 *
 * The second half is here rather than at each caller, and that is the fix rather than a tidy-up.
 * `forEachOperandOrigin` answers for a place rooted in a *local*, because a local is storage this
 * frame has state for; a place rooted in an `Address` has none, and what it may reach is what the
 * pointer's **type** said - `%source'a`'s one interior position, the pointer value's second. Two
 * callers wanted that and only `reachFrom` had it, so a write through a qualified pointer reached
 * `source`'s loan and reborrowed nothing: `store(base + count, 0)` in `withPathCString` conflicted
 * with the very `valuesMut(buffer)` loan it was writing through, which is what a `&` binding's
 * origins excuse one line further down.
 *
 * A pointer whose type names nothing says nothing about *where* it points, which is the conservative
 * half of the same sentence and is answered by `check`'s `anywhere` instead. What it does say is
 * what it was **reached through**, and that is the last case below.
 */
void Analysis::unionPlaceOrigins(OriginCursor& cursor, PlaceId place, U64* into) {
    unionOperandOrigins(cursor, Operand(SourceKind::Place, place.index, UseMode::Inspect), into);

    auto root = rootOf(place);
    if(root->kind != Place::Address) return;

    auto pointer = NodeId(root->payload);
    if(valuePositions(pointer) >= 2) {
        BitMatrix::unionInto(into, originsOfValue(pointer, 1), loanWords);
        return;
    }

    /*
     * An unqualified address **read out of storage** - `n.next` on a `data Node {next: %Node}`,
     * where a field's own type has nowhere to name an origin and the pointer therefore points
     * wherever it was made to point.
     *
     * Which storage it names is unknown and stays unknown: `anywhere` is what says so, and this does
     * not touch it. What is known is which loans the access was *made through* - the walk to this
     * pointer went through `n`, and `n` names `first` - and that is the reborrow question §2.5 asks.
     * An access through a pointer fetched out of `first` is an access through `first`'s loan, so it
     * does not conflict with it, exactly as a write through a `&` binding does not conflict with the
     * binding's own loan. Without it a linked list is unwritable: `n.next.value = ...` in
     * `test/resolve/Pointer.yana` conflicts with the `addressOf(first)` that produced `n`, and no
     * spelling an author could reach would say otherwise, since `Node.next` is a field.
     *
     * Recursive, because a chain is a chain: `a.next.next` is this case one level further out, and
     * the walk terminates because an operand names a node earlier than its own.
     */
    auto node = body.node(base, pointer);
    if(node->kind != Node::Read) return;

    auto& read = (NodeRead&)*node;
    if(read.place.source() != SourceKind::Place) return;

    unionPlaceOrigins(cursor, read.place.placeId(), into);
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
 * to; it changes what is at `c`. The written value's origins are unioned into the binding, which is
 * what comes back out of `d`, and into `c`'s own rows at the lender - `applyThrough` - which is what
 * a later read of `c` by its own name sees.
 *
 * A partial write - `r.at = 3`, or `r.held.id = 9` - is neither. It reaches only the positions its
 * projection selects and it unions into them, because writing *through* a slot does not change which
 * object the slot names: an external field is interned as the slot's `Field` followed by a `Deref`
 * of it, so there is no spelling in a body that re-points one and no strong update to make.
 */
Place* Analysis::replacedLocal(Node& node) {
    if(node.kind != Node::Write) return nullptr;

    auto& write = (NodeWrite&)node;
    if(write.destination.source() != SourceKind::Place) return nullptr;

    auto place = body.place(base, write.destination.placeId());
    if(place->parent || place->kind != Place::Local) return nullptr;

    /*
     * A binding's `Init` replaces too (J127): the local held nothing before it, so whatever its row
     * says came from an earlier pass and is discarded, not written through. It used to be a weak
     * update for a local that borrows, which kept the previous pass's loan beside the new one - and
     * with loans renamed at their mint (`generationOf`) that made `let &e = getMut(xs, i)` in a loop
     * conflict with its own previous pass. J124 cleared a `Many` region's locals at its entry for the
     * same reason; this is the same statement made where the binding is.
     */
    if(!place->occurrence.contract.isOwned() && write.mode != Destination::Init) return nullptr;

    return place;
}

/*
 * The local a fresh run ends up in: the run's value taken into an aggregate, that one into another,
 * and the last written whole into a local - `aggregate.Run`, `aggregate.Array`, `write.init`. None
 * where the chain is anything else, which leaves the store unrecorded exactly as it was.
 */
LocalId Analysis::runOwner(NodeId pointer) {
    if(!takenByKnown) {
        takenByKnown = true;
        takenBy.clear();
        for(U32 i = 0; i < body.nodes.size(); i++) takenBy.push(~U32(0));

        // Each taken result's one consumer - a value is taken at most once, so one entry each.
        for(U32 i = 0; i < body.nodes.size(); i++) {
            forEachOperand(base, *body.node(base, NodeId(i)), [&](Operand& operand, OperandRole) {
                if(operand.source() != SourceKind::Result || operand.mode() != UseMode::Take) return;
                takenBy[operand.nodeId().index] = i;
            });
        }
    }

    auto at = pointer.index;
    for(U32 steps = 0; steps < 4; steps++) {
        auto consumer = takenBy[at];
        if(consumer == ~U32(0)) return LocalId();

        auto node = body.node(base, NodeId(consumer));
        if(node->kind == Node::Aggregate) {
            at = consumer;
            continue;
        }

        if(node->kind != Node::Write) return LocalId();

        auto& write = (NodeWrite&)*node;
        if(write.destination.source() != SourceKind::Place) return LocalId();

        auto place = body.place(base, write.destination.placeId());
        if(place->parent || place->kind != Place::Local) return LocalId();

        return LocalId(place->payload);
    }

    return LocalId();
}

/*
 * Which rows of the local origin state one `Write` writes, asked without the state in hand.
 *
 * Structural on purpose: it is resolved once per body, into `writes`, before the fixpoint climbs,
 * and read at every visit after. A condition that depended on the state would be answered once
 * against a state the analysis had not settled yet.
 *
 * The last condition is the one that keeps the log from growing with the slot axis. A **weak**
 * update from a value that carries no origins at all adds nothing to any row, and whether it does is
 * a property of the value's occurrence rather than of the state - so a partial write like
 * `m.write.v = 3`, which reaches two positions of `m` and stores an `I32` into neither, is not a
 * write to the state and costs no entry. A **strong** update writes even from an empty value,
 * because emptying the row is the update.
 */
bool Analysis::localWriteOf(Node& node, StateWrite& into, SmallArray<PositionMap, 8>& positions,
                            SmallArray<U16, 4>& through) {
    if(node.kind != Node::Write) return false;

    auto& write = (NodeWrite&)node;
    if(write.destination.source() != SourceKind::Place) return false;

    auto id = write.destination.placeId();
    auto root = rootOf(id);
    auto& placeOccurrence = body.place(base, id)->occurrence;
    Projection projection;

    /*
     * An element a container **literal** hands over through its fresh run's address - `[Store {held:
     * source}]`. The store is outside the borrow model by construction (`resolveGrowableLiteral`),
     * but what it stores is not: the loans go to the local the run was moved into, at its element
     * position, as `push` puts them there through its open group. Found by following the run's value
     * forward rather than by writing the element through the local, which would put a field of
     * `Array` - a library record the compiler reads no field of - into every literal's MIR.
     *
     * Any other address stays unrecorded: raw access carries no validity promise, and the pointer's
     * own origins are what a read through it reaches.
     */
    if(root->kind == Place::Address && write.mode == Destination::Init &&
       body.node(base, NodeId(root->payload))->kind == Node::RunSlots &&
       operandCarriesOrigins(write.value)) {
        auto owner = runOwner(NodeId(root->payload));
        if(!owner.valid() || !localPositions(owner)) return false;

        into.local = owner;
        into.placeCount = originPositions(decl, placeOccurrence);

        projection.opaque = true;
        selectPositions(projection, body.local(base, owner)->occurrence, placeOccurrence, positions);
        return true;
    }

    if(root->kind != Place::Local) return false;

    into.local = LocalId(root->payload);
    if(!localPositions(into.local)) return false;

    into.placeCount = originPositions(decl, placeOccurrence);
    into.strong = replacedLocal(node) != nullptr;

    /*
     * The **interior** slots a write replaces outright, where the root row is not replaced (J132).
     *
     * A place reached from a local by fields alone - no index, no pointer, no loan field crossed -
     * is bytes of one object, and a write there overwrites every slot at or under it: the old
     * value's loans are not in it any more. That holds for storage this frame owns and for storage
     * a **unique** borrow names, which is the only path to that object while it is live. Those slots
     * were unioned into like every other partial write, so `s = {s | held: owned}` then
     * `s = {s | held: xs}` over `&s: Store(kept')` left `owned`'s loan in `s.held`'s row - twice: the
     * update's own `local.held = xs` kept the copied field's loan, and the whole write through `s`
     * kept the previous contents - and the destination check refused the function for a loan the
     * caller can no longer reach (review 2 §2.3).
     *
     * What stays a union: the root row (a write through a borrow does not re-point it, see
     * `replacedLocal`), an element position (one element of many), a slot the place is *inside*
     * (J128's `o.held.ref = ...`, which writes one field of an object the slot only names), and the
     * lender's own rows through `applyThrough`, which is the conservative side. Slots are in path
     * order, so the ones under one prefix are a run; a shape that is not is left to the union.
     */
    auto& rootContract = body.local(base, into.local)->occurrence.contract;
    auto fieldsOnly = true;
    for(auto p = body.place(base, id); p->parent; p = base[p->parent]) {
        if(p->kind != Place::Field) fieldsOnly = false;
    }

    auto schema = shapeOf(decl, body.local(base, into.local)->occurrence);
    if(!into.strong && fieldsOnly && schema &&
       (rootContract.isOwned() || rootContract.access() == Access::Unique)) {
        Projection written;
        projectionOf(id, written);

        auto& slots = *decl[schema];
        auto first = U32(0), end = U32(0);
        auto contiguous = true;

        for(U32 i = 0; i < slots.slots.size(); i++) {
            auto& slot = *decl[slots.slots.get(decl, i)];
            auto under = !written.opaque && slot.path.size() >= written.fields.size();
            for(U32 step = 0; under && step < written.fields.size(); step++) {
                under = slot.path.get(decl, step) == written.fields[step];
            }

            if(!under) continue;
            if(end && end != 1 + i) contiguous = false;
            if(!end) first = 1 + i;
            end = 2 + i;
        }

        if(contiguous && end) {
            into.strongFirst = U16(first);
            into.strongEnd = U16(end);
        }
    }

    if(!into.strong && into.strongFirst == into.strongEnd && !operandCarriesOrigins(write.value)) {
        return false;
    }

    auto& localOccurrence = body.local(base, into.local)->occurrence;
    projectionOf(id, projection);
    selectPositions(projection, localOccurrence, placeOccurrence, positions, true);

    // An `Init` establishes a binding rather than writing through it: `let &chosen = pick(x, y)`
    // makes `chosen` the loan, and writes nothing into `x` or `y`.
    if(write.mode != Destination::Init) {
        throughPositions(localOccurrence.contract.isOwned(), positions, through);
    }

    return positions.size() != 0;
}

bool Analysis::callParts(Node& node, DeclPtr<Function>& callee, OperandList*& arguments) {
    if(node.kind == Node::Call) {
        auto& call = (NodeCall&)node;
        if(call.callsValue()) return false;

        callee = call.callee;
        arguments = &call.arguments;
        return true;
    }

    // Never without a callee: `Builder::addLensCall` is the one constructor and takes a `Function&`.
    if(isLensCall(node.kind)) {
        auto& call = (NodeLensCall&)node;
        callee = call.callee;
        arguments = &call.arguments;
        return true;
    }

    return false;
}

U32 Analysis::operandPositions(Operand operand) {
    if(operand.source() == SourceKind::Result) return valuePositions(operand.nodeId());
    if(operand.source() == SourceKind::Place) {
        return originPositions(decl, body.place(base, operand.placeId())->occurrence);
    }

    return 0;
}

/*
 * Which group position `p` of a declared occurrence is in - the key an argument, the result and a
 * `&` parameter's interior are related by at a call.
 *
 * A published slot is the group it names. An interior position of an occurrence whose type is still
 * **open** - `x: a`, `self: Array(a)` - is in one group of its own, `kOpenGroup`, shared by every open
 * position of the signature. The declaration cannot say what is inside a value of `a`, so nothing
 * was checked about it in the callee's body either: `fn (a) put(&x: a, ->v: a)` may put `v` into `x`
 * and the body compiled once over `a` has no position to object with. At the call the operands have
 * the substituted type's positions, and the only sound reading of a signature that described none of
 * them is that any of them may have come from any other. Deliberately one group rather than one per
 * variable and position: `Array(a)` and `a` number their positions differently, and pairing them
 * would be a substitution this walk does not have - the node names the generic callee, not a
 * specialization (`NodeCall::typeArguments`).
 *
 * The root is never open. It is the occurrence's own contract, which says what it says whatever the
 * type is.
 */
U16 Analysis::groupAt(const Occurrence& declared, U32 position) {
    auto slot = originSlotAt(decl, declared, position);
    if(slot != kNoOriginSlot || !position) return slot;

    return declared.type && !isConcreteType(decl, declared.type) ? kOpenGroup : kNoOriginSlot;
}

U32 Analysis::callArguments(NodeId node) {
    auto starts = nodeTables[node.index + 1].calls - nodeTables[node.index].calls;
    return starts ? starts - 2 : 0;
}

U16 Analysis::callGroup(NodeId node, U32 argument, U32 position) {
    auto at = nodeTables[node.index].calls + argument;
    auto first = callStarts[at];
    if(first + position < callStarts[at + 1]) return callGroups[first + position];

    /*
     * A position past the ones the operand has. The table holds a group for every position the
     * operand's occurrence has, which is every position `forEachOperandOrigin` hands out, so no
     * walk asks this; answered from the declaration anyway rather than as "no group", because a
     * wrong "no group" is a loan silently not related.
     */
    Occurrence declared;
    auto found = callPosition(*body.node(base, node), argument, callArguments(node), declared);
    assertTrue(found);
    return found ? groupAt(declared, position) : kNoOriginSlot;
}

bool Analysis::callPosition(Node& node, U32 argument, U32 count, Occurrence& into) {
    DeclPtr<Function> callee;
    OperandList* arguments;
    if(callParts(node, callee, arguments)) {
        auto& function = *decl[callee];
        if(argument == count) { into = function.result; return true; }
        if(argument >= function.args.size()) return false;

        into = decl[function.args.get(decl, argument)]->occurrence;
        return true;
    }

    // A value call, which `callParts` refuses - see `valueCallType`.
    if(node.kind != Node::Call) return false;

    auto type = valueCallType((NodeCall&)node);
    if(!type) return false;

    auto& fun = *(FunType*)decl[type];
    if(argument == count) { into = fun.result; return true; }
    if(argument >= fun.params.size()) return false;

    into = fun.params.get(decl, argument).occurrence;
    return true;
}

bool Analysis::installSource(NodeId node, U32 argument, U32 at, U16 group, U32 j, U32 p) {
    // Never the destination's own root, which is the loan the caller lent, and never the position
    // being filled - an installation from itself is no installation.
    if(j == argument && (p == 0 || p == at)) return false;
    return callGroup(node, j, p) == group;
}

/*
 * Every row that fills position `at` of `&` argument `argument` - `applyInstall`'s union and
 * `checkGlobalConformance`'s, which have to be the same union or the check would be judging a
 * different installation from the one the state records.
 */
template<class F>
void Analysis::forEachInstallSource(OriginCursor& cursor, Node& node, U32 argument, U32 at, F&& f) {
    DeclPtr<Function> callee;
    OperandList* arguments;
    if(!callParts(node, callee, arguments)) return;

    auto operands = arguments->contents(base);
    auto count = callArguments(node.id);
    auto group = callGroup(node.id, argument, at);

    for(U32 j = 0; j < count; j++) {
        forEachOperandOrigin(cursor, *operands.pointerAt(j), [&](U32 p, const U64* row) {
            if(installSource(node.id, argument, at, group, j, p)) f(row);
        });
    }
}

/*
 * One call's group table, and from it the positions of each argument the callee may install a loan
 * into - the signature half, into `installs`.
 *
 * The groups are `groupAt` of every position of every argument and of the result, asked once: three
 * walks relate a call's positions by group - the installation, the global check's copy of it and the
 * call-result rule in `computeOrigins` - and they asked it of the same (call, argument, position) at
 * every visit, which for an open position is a walk over the type (`isConcreteType`).
 *
 * An installation is into a `&` parameter, an **interior** position of it, and a group some other
 * position of the signature is also in. Interior only: the root of `&'T` is the loan the caller
 * lent, which no body can re-point ("There is no operation that rebinds what a borrow refers to"),
 * and a group nobody else is in has nothing to be filled from - `&store: Store(kept')` alone may
 * reshuffle what `store` already holds but cannot bring anything new into it.
 *
 * Read from the declaration the call names, so a class member called from a body compiled once is
 * the class signature's answer: the body cannot see which implementation runs, and the signature is
 * what every implementation was checked against. A generic callee is answered the same way, through
 * `groupAt`'s open group - the operands have the substituted positions, which are what is asked.
 */
void Analysis::computeCallTables(Node& node) {
    DeclPtr<Function> callee;
    OperandList* arguments;
    if(!callParts(node, callee, arguments)) return;

    auto& function = *decl[callee];
    auto operands = arguments->contents(base);
    auto count = min(U32(operands.size()), U32(function.args.size()));
    auto calls = U32(callStarts.size());

    for(U32 j = 0; j < count; j++) {
        callStarts.push(U32(callGroups.size()));

        auto& declared = decl[function.args.get(decl, j)]->occurrence;
        auto width = operandPositions(*operands.pointerAt(j));
        for(U32 p = 0; p < width; p++) callGroups.push(groupAt(declared, p));
    }

    // The result's, read only by the call-result rule - a lens call's result is the continuation's
    // answer, and what it hands over is related there by a rule of its own.
    callStarts.push(U32(callGroups.size()));
    if(node.kind == Node::Call) {
        auto positions = valuePositions(node.id);
        for(U32 p = 0; p < positions; p++) callGroups.push(groupAt(function.result, p));
    }

    callStarts.push(U32(callGroups.size()));

    auto width = [&](U32 j) { return callStarts[calls + j + 1] - callStarts[calls + j]; };

    for(U32 argument = 0; argument < count; argument++) {
        auto& destination = *decl[function.args.get(decl, argument)];
        if(destination.occurrence.contract.access() != Access::Unique) continue;

        for(U32 at = 1; at < width(argument); at++) {
            auto group = callGroup(node.id, argument, at);
            if(group == kNoOriginSlot) continue;

            auto published = false;
            for(U32 j = 0; j < count && !published; j++) {
                for(U32 p = 0; p < width(j) && !published; p++) {
                    published = installSource(node.id, argument, at, group, j, p);
                }
            }

            if(published) installs.push(Installed { U16(argument), U16(at) });
        }
    }
}

/*
 * The same table for a call through a function **value** whose type publishes origins (J141, J94
 * §3.4's `LoanGroup`) - `f: (&a: l'Cell, &b: r'Cell) -> &l'Cell`.
 *
 * A function type numbers its origins in a scope of its own, which is the value's signature, so its
 * parameters and result are read exactly as a declaration's are and the call-result rule relates
 * them the same way: `f(first, second)` is rooted in `first` and not in `second`. A type that
 * publishes nothing gets no table, and the rule leaves such a result rooted in nothing - the
 * conservative answer, and the true one, since a result with no slot is owned.
 *
 * No installation: `resolveType` refuses a function type whose `&` parameter has an interior
 * position in a group another position shares, which is the one thing an install needs.
 */
void Analysis::computeValueCallTables(Node& node) {
    if(node.kind != Node::Call) return;

    auto& call = (NodeCall&)node;
    auto type = valueCallType(call);
    if(!type) return;

    auto operands = call.arguments.contents(base);
    auto count = min(U32(operands.size()), U32(((FunType*)decl[type])->params.size()));

    // Read through `callPosition`, which is also what `callGroup` answers a position past the table
    // from - one reading of a function type's positions rather than two.
    for(U32 j = 0; j < count; j++) {
        callStarts.push(U32(callGroups.size()));

        Occurrence declared;
        callPosition(node, j, count, declared);
        auto width = operandPositions(*operands.pointerAt(j));
        for(U32 p = 0; p < width; p++) callGroups.push(groupAt(declared, p));
    }

    callStarts.push(U32(callGroups.size()));
    Occurrence result;
    callPosition(node, count, count, result);
    auto positions = valuePositions(node.id);
    for(U32 p = 0; p < positions; p++) callGroups.push(groupAt(result, p));
    callStarts.push(U32(callGroups.size()));
}

/*
 * The function type a value call goes through, where it publishes an origin - see
 * `computeValueCallTables`. Null for every other call, which is nearly all of them.
 */
DeclPtr<Type> Analysis::valueCallType(NodeCall& call) {
    if(!call.callsValue() || !call.targetType) return nullptr;
    if(decl[call.targetType]->kind != Type::Fun) return nullptr;

    auto& fun = *(FunType*)decl[call.targetType];
    auto publishes = [&](const Occurrence& occurrence) {
        if(occurrence.contract.origin() == Origin::Slot) return true;
        if(!occurrence.shape) return false;
        for(auto slot: decl[occurrence.shape]->slots.contents(decl)) {
            if(decl[slot]->contract.origin() == Origin::Slot) return true;
        }
        return false;
    };

    if(publishes(fun.result)) return call.targetType;
    return nullptr;
}

/*
 * Everything structural the walks over the state ask of a node, once per body - see `nodeTables`.
 *
 * The state writes are resolved here from `localWriteOf` and `installWriteOf`, which used to run at
 * every visit of every `Write` and installing call: each fixpoint round, both passes of the log, the
 * check walk and the global check at the same node - a projection walk, a schema selection and two
 * occurrence copies each time for an answer that could not change. The log's bound is summed on the
 * way, which is what the log's counting pass was for.
 */
void Analysis::computeNodeTables() {
    nodeTables.clear();
    installs.clear();
    callStarts.clear();
    callGroups.clear();
    writes.clear();
    writePositions.clear();
    writeThrough.clear();
    logBound = 0;

    SmallArray<PositionMap, 8> positions;
    SmallArray<U16, 4> through;

    auto commit = [&](StateWrite& write) {
        write.positionsAt = U32(writePositions.size());
        write.positionCount = U16(positions.size());
        for(auto entry: positions) writePositions.push(entry);

        write.throughAt = U32(writeThrough.size());
        write.throughCount = U16(through.size());
        for(auto at: through) writeThrough.push(at);

        // Every row the write names, and - through a loan - every row a lender can have: see
        // `logPrevious`.
        logBound += write.positionCount + (write.goesThrough() ? lenderRows : 0);
        writes.push(write);
    };

    for(U32 i = 0; i < body.nodes.size(); i++) {
        nodeTables.push(NodeTable { U32(installs.size()), U32(writes.size()), U32(callStarts.size()) });

        auto& node = *body.node(base, NodeId(i));

        StateWrite write;
        positions.clear();
        through.clear();
        if(localWriteOf(node, write, positions, through)) {
            commit(write);
            continue;
        }

        DeclPtr<Function> callee;
        OperandList* arguments;
        if(!callParts(node, callee, arguments)) {
            computeValueCallTables(node);
            continue;
        }

        computeCallTables(node);

        // One write per argument the callee installs into, in argument order - which is the order
        // `installs` holds them in, so each argument's positions are one run of it.
        auto operands = arguments->contents(base);
        auto end = U32(installs.size());

        for(auto first = nodeTables[i].installs; first < end;) {
            auto argument = U32(installs[first].argument);
            auto next = first + 1;
            while(next < end && installs[next].argument == argument) next++;

            StateWrite install;
            install.argument = argument;
            install.installedAt = first;
            install.installedCount = U16(next - first);

            positions.clear();
            through.clear();
            if(installWriteOf(*operands.pointerAt(argument), install, positions, through)) {
                commit(install);
            }

            first = next;
        }
    }

    nodeTables.push(NodeTable { U32(installs.size()), U32(writes.size()), U32(callStarts.size()) });
}

void Analysis::installedPositions(Node& node, U32 argument, SmallArray<U16, 4>& into) {
    into.clear();

    auto end = nodeTables[node.id.index + 1].installs;
    for(auto i = nodeTables[node.id.index].installs; i < end; i++) {
        if(installs[i].argument == argument) into.push(installs[i].position);
    }
}

bool Analysis::callInstalls(Node& node) {
    return nodeTables[node.id.index].installs != nodeTables[node.id.index + 1].installs;
}

/*
 * The storage half: which rows of the local state an installation into `operand` reaches.
 *
 * The destination is the place the `&` argument was borrowed from, and what the callee does to it
 * is a **weak** update of the positions it may fill - the union of what they held and what the
 * group brought. Weak rather than strong for two reasons that are one: the callee may keep the old
 * loan (`store = Store {held: store.held}` conforms), and the spec binds the group to the *set*, so
 * after the call `box` may be rooted in `seed` or in `source` and both stay live. The old loan is
 * released where `box` is next replaced whole, exactly as for any other value it holds.
 *
 * A destination that is not a place of a local this frame has rows for - a reference a call
 * returned - is reached through the loans the argument's value is rooted in, which `applyThrough`
 * follows to the local that lent them. One rooted in nothing this body minted (an address with no
 * published origin) installs nowhere the walk can record. One that is a global's own place is no
 * state write at all: a global has no rows here, and `checkGlobalConformance` asks about it directly.
 */
bool Analysis::installWriteOf(Operand operand, StateWrite& into, SmallArray<PositionMap, 8>& positions,
                              SmallArray<U16, 4>& through) {
    auto place = installDestination(operand);
    auto root = place.valid() ? rootOf(place) : nullptr;
    if(root && root->kind == Place::Global) return false;

    /*
     * A destination that is not a place this frame has rows for is reached **through** whatever the
     * argument's value is rooted in - `remember(itself(box), source)` installs into `box` as surely
     * as `remember(box, source)` does, and the returned reference's root is the loan of `box`.
     */
    if(!root || root->kind != Place::Local || !localPositions(LocalId(root->payload))) {
        if(operand.source() != SourceKind::Result || !valuePositions(operand.nodeId())) return false;

        into.throughValue = operand.nodeId();
        into.placeCount = valuePositions(operand.nodeId());
        through.push(0);
        return true;
    }

    into.local = LocalId(root->payload);

    auto& localOccurrence = body.local(base, into.local)->occurrence;
    auto& placeOccurrence = body.place(base, place)->occurrence;
    into.placeCount = originPositions(decl, placeOccurrence);

    Projection projection;
    projectionOf(place, projection);
    selectPositions(projection, localOccurrence, placeOccurrence, positions);

    // The positions an installation reaches: an installed one where the place keeps it, and every
    // slot of the local the place is inside of. Never the local's own root, which is the frame's
    // storage or the loan a `&` binding holds - neither is what the callee fills.
    U32 kept = 0;
    for(U32 i = 0; i < positions.size(); i++) {
        auto entry = positions[i];
        auto reached = entry.to == 0 ? entry.from != 0 : false;

        for(U32 k = 0; k < into.installedCount; k++) {
            reached = reached || entry.to == installs[into.installedAt + k].position;
        }

        if(reached) positions[kept++] = entry;
    }

    positions.resize(kept);

    throughPositions(localOccurrence.contract.isOwned(), positions, through);
    return positions.size() != 0;
}

/*
 * The place a `&` argument was borrowed from, or none. A borrowing read is how the walk hands one
 * over; a place operand is the same statement without the node.
 */
PlaceId Analysis::installDestination(Operand operand) {
    if(operand.source() == SourceKind::Place) return operand.placeId();
    if(operand.source() != SourceKind::Result) return PlaceId();

    auto node = body.node(base, operand.nodeId());
    if(node->kind != Node::Read) return PlaceId();

    auto& read = (NodeRead&)*node;
    if(read.place.source() != SourceKind::Place) return PlaceId();

    return read.place.placeId();
}

/*
 * A write that lands **through a loan** changes the lender's storage, not the local's.
 *
 * `r.held = ...` over `data R {held: &'Store}`, a `&` binding assigned to, and a call installing into
 * either of them all reach an object this frame named somewhere else - `box`, where the loan was
 * taken. The local's own row is unioned into, which is true of what comes back out through it; the
 * lender's rows are too, because `box` read *directly* afterwards holds the new loan as well - with
 * the local's row alone, `source.value = 9` before `box.held.value` is admitted and reads 9 back.
 *
 * Which lender is a question about the state - the loans the through-positions hold at this point -
 * and that is `gatherLenders` and `applyThrough`. What is structural is only *that* a write goes
 * through, and the positions that say where. The log needs nothing more, because `writeStateRow`
 * logs whichever lender rows the write actually moved.
 */
void Analysis::throughPositions(bool owned, const SmallArray<PositionMap, 8>& positions,
                                SmallArray<U16, 4>& into) {
    if(!owned) into.push(0);

    for(auto& entry: positions) {
        if(entry.to == 0 && entry.from != 0) into.push(entry.from);
    }
}

void Analysis::gatherLenders(OriginCursor& cursor, const StateWrite& write, U64* into) {
    BitMatrix::zero(into, loanWords);

    if(write.throughValue.valid()) {
        BitMatrix::unionInto(into, originsOfValue(write.throughValue, 0), loanWords);
        return;
    }

    for(U32 k = 0; k < write.throughCount; k++) {
        auto position = writeThrough[write.throughAt + k];
        expandInto(originsOfLocal(cursor.state(), write.local, position), expanded.pointer());
        BitMatrix::unionInto(into, expanded.pointer(), loanWords);
    }
}

/*
 * Every **unique** loan a write through `lenders` reaches, and the root of the place each one lent:
 * the lenders themselves, and for a lender that is itself a borrow, the loans *it* holds at its root,
 * which the write reaches one step further on. `visited` bounds it. Unique only, since nothing writes
 * through a shared loan - so a shared one never enters the walk, which `uniqueLoans` masks out word
 * by word. `applyThrough` acts on the locals among them and `checkGlobalConformance` on the globals,
 * and they are one walk so that the two cannot disagree about which storage a write went into.
 */
template<class F>
void Analysis::forEachUniqueLender(OriginCursor& cursor, const U64* lenders, F&& f) {
    auto pending = throughRow(ThroughRow::Pending);
    auto visited = throughRow(ThroughRow::Visited);
    auto round = throughRow(ThroughRow::Round);

    for(U32 w = 0; w < loanWords; w++) pending[w] = lenders[w] & uniqueLoans[w];
    BitMatrix::zero(visited, loanWords);

    while(BitMatrix::any(pending, loanWords)) {
        BitMatrix::copy(round, pending, loanWords);
        BitMatrix::zero(pending, loanWords);

        BitMatrix::forEach(round, loanWords, [&](Size m) {
            auto l = U32(m);
            if(BitMatrix::test(visited, l)) return;
            BitMatrix::set(visited, l);

            auto root = loanRoots[l];
            f(l, *root);

            // A borrow lends what it holds at its root, which a write through it does not re-point.
            if(root->kind != Place::Local) return;

            auto lender = LocalId(root->payload);
            if(!localPositions(lender) || body.local(base, lender)->occurrence.contract.isOwned()) {
                return;
            }

            expandInto(originsOfLocal(cursor.state(), lender, 0), expanded.pointer());
            for(U32 w = 0; w < loanWords; w++) pending[w] |= expanded[w] & uniqueLoans[w];
        });
    }
}

/*
 * Every unique lender's storage gains what was written - weakly, and at every slot the lent place
 * reaches, which is the conservative end of a mapping that would otherwise have to line the written
 * value's positions up against a place the write never named. Which slots those are is the loan's,
 * and `computeLoanTables` read them off once (`lent`).
 */
void Analysis::applyThrough(OriginCursor& cursor, const U64* lenders, const U64* written) {
    if(!BitMatrix::any(written, loanWords) || !BitMatrix::any(lenders, loanWords)) return;

    auto own = throughRow(ThroughRow::Own);

    forEachUniqueLender(cursor, lenders, [&](U32 l, Place& root) {
        if(root.kind != Place::Local || lentRange[l] == lentRange[l + 1]) return;

        auto lender = LocalId(root.payload);

        /*
         * Never a loan of the lender itself. `let &smallest = xs[first]` followed by a write into
         * `smallest` of a value read out of `xs` would otherwise leave `xs` rooted in its own loan,
         * and every later borrow of it a conflict with itself - a record cannot hold a loan of its
         * own storage ("Records holding external values"), so that answer is never the true one.
         */
        BitMatrix::copy(own, written, loanWords);
        BitMatrix::forEach(written, loanWords, [&](Size m) {
            auto other = loanRoots[m];
            if(other && other->kind == Place::Local && other->payload == root.payload) BitMatrix::clear(own, m);
        });

        // Never the lender's root: an owned local's is its own storage, and a borrow's is the loan
        // it holds, which a write through it does not re-point - `forEachUniqueLender` follows that
        // loan instead. `lent` holds the other positions only.
        for(auto k = lentRange[l]; k < lentRange[l + 1]; k++) {
            writeStateRow(cursor, localRow[lender.index] + lent[k], own, false);
        }
    });
}

/*
 * One installation, applied.
 *
 * Every installed position gets the union of what every other member of its group carries, at
 * whichever position of *that* argument names the group - the result rule's mapping, pointed at an
 * argument instead of at the result. Row zero of the scratch is the union of all of them, which is
 * what a place inside one of the local's slots receives.
 *
 * The rows are read from the arguments' values, which were produced before the call, so a second
 * installation at the same node reads what the first one did not touch.
 */
void Analysis::applyInstall(OriginCursor& cursor, Node& node, const StateWrite& write) {
    auto rows = write.placeCount ? write.placeCount : 1;
    auto width = Size(rows) * loanWords;
    while(writeScratch.size() < width) writeScratch.push(0);
    BitMatrix::zero(writeScratch.pointer(), width);

    for(U32 k = 0; k < write.installedCount; k++) {
        auto at = U32(installs[write.installedAt + k].position);
        if(at >= rows) continue;

        auto into = writeScratch.pointer() + Size(at) * loanWords;
        forEachInstallSource(cursor, node, write.argument, at, [&](const U64* row) {
            BitMatrix::unionInto(into, row, loanWords);
            BitMatrix::unionInto(writeScratch.pointer(), row, loanWords);
        });
    }

    applyRows(cursor, write);
}

/*
 * One `Write`, applied.
 *
 * The value's positions are gathered before any row is touched, so that what is applied is the state
 * on *entry* to the write. A local assigned from itself therefore reads the rows it is about to
 * replace, and the copies are rows onto themselves. The cursor carries one buffer, so a transfer that
 * wrote before it read would be reading what it had already replaced - and with a position per slot
 * that is no longer one row against itself but a record's second field against its first.
 */
void Analysis::applyWrite(OriginCursor& cursor, Node& node, const StateWrite& write) {
    auto rows = write.placeCount ? write.placeCount : 1;
    auto width = Size(rows) * loanWords;
    while(writeScratch.size() < width) writeScratch.push(0);
    BitMatrix::zero(writeScratch.pointer(), width);

    forEachOperandOrigin(cursor, ((NodeWrite&)node).value, [&](U32 position, const U64* row) {
        auto to = position < write.placeCount ? position : 0;
        BitMatrix::unionInto(writeScratch.pointer() + Size(to) * loanWords, row, loanWords);
    });

    applyRows(cursor, write);
}

/*
 * The half a write and an installation share, once `writeScratch` holds a row per position of the
 * place: into the local's rows - replacing them for a strong update, unioning otherwise - and
 * through whatever loans the place is reached by, into each lender's.
 *
 * What reaches a lender is the whole value, every position of it: `pair.a = Store {held: source}`
 * over `a: &'Store` lands `source` in `box.held`, and `pair` has no row for `a.held` - its one
 * position there is the loan of `box` - so the local's own mapping has nowhere to put it. For an
 * installation row zero is already that union.
 *
 * Most writes land in this frame's own storage and go through nothing, and those skip the lender
 * half outright - gathering no lenders is what made `applyThrough` a no-op for them, after a union
 * of every scratch row it then never read.
 */
void Analysis::applyRows(OriginCursor& cursor, const StateWrite& write) {
    auto rows = write.placeCount ? write.placeCount : 1;
    auto through = write.goesThrough();
    auto lenders = throughRow(ThroughRow::Lenders);
    auto written = throughRow(ThroughRow::Written);

    // The lenders before the rows change: which loans the place is reached through is the state on
    // entry to the write.
    if(through) {
        gatherLenders(cursor, write, lenders);

        BitMatrix::zero(written, loanWords);
        for(U32 row = 0; row < rows; row++) {
            BitMatrix::unionInto(written, writeScratch.pointer() + Size(row) * loanWords, loanWords);
        }
    }

    /*
     * A row the write replaces is replaced by its **first** entry and unioned into by the rest: a
     * write into a slot lands every position of the value on it (`selectPositions`), which is
     * several entries for one row, and replacing at each would keep only the last.
     */
    SmallArray<U16, 8> replaced;
    for(U32 k = 0; k < write.positionCount; k++) {
        auto& entry = writePositions[write.positionsAt + k];
        auto to = entry.to < rows ? entry.to : 0;

        auto replace = write.strong || (entry.from >= write.strongFirst && entry.from < write.strongEnd);
        for(auto from: replaced) replace = replace && from != entry.from;
        if(replace) replaced.push(entry.from);

        writeStateRow(cursor, localRow[write.local.index] + entry.from,
                      writeScratch.pointer() + Size(to) * loanWords, replace);
    }

    if(through) applyThrough(cursor, lenders, written);
}

void Analysis::writeStateRow(OriginCursor& cursor, U32 row, const U64* full, bool replace) {
    auto into = cursor.state() + Size(row) * stateWords;

    if(!cursor.logging) {
        compactInto(into, full, replace);
        return;
    }

    auto before = rowBefore.pointer();
    BitMatrix::copy(before, into, stateWords);
    compactInto(into, full, replace);

    // A row that did not move has nothing for an undo to put back.
    auto moved = false;
    for(U32 w = 0; w < stateWords && !moved; w++) moved = into[w] != before[w];
    if(!moved) return;

    auto entry = U32(log.size());
    assertTrue(entry < logPrevious.rows());

    log.push(OriginEdit { cursor.position, row });
    BitMatrix::copy(logPrevious.row(entry), before, stateWords);
}

/*
 * Loan `from` becomes `to` in every row of the state that holds it - one generation handed on
 * (`generationOf`). Logged row by row as `writeStateRow` logs, so the walks that undo their way up
 * a block see the old name above the read and the new one below it.
 *
 * Only the state. A value is one node's result and a node inside a loop computes it afresh on every
 * pass, so what carries a loan around a back edge is storage; a block argument could too, and none
 * that carries a loan to its own mint is built today.
 */
void Analysis::renameGeneration(OriginCursor& cursor, U32 from, U32 to) {
    auto source = stateIndex[from];
    auto target = stateIndex[to];
    if(source == kNotStored || target == kNotStored) return;

    for(U32 row = 0; row < originRowCount; row++) {
        auto cells = cursor.state() + Size(row) * stateWords;
        if(!BitMatrix::test(cells, source)) continue;

        if(cursor.logging) {
            auto entry = U32(log.size());
            assertTrue(entry < logPrevious.rows());

            log.push(OriginEdit { cursor.position, row });
            BitMatrix::copy(logPrevious.row(entry), cells, stateWords);
        }

        BitMatrix::clear(cells, source);
        BitMatrix::set(cells, target);
    }
}

/*
 * Whether any loan reached its own mint held by something - `carriedLoans`, which `buildOriginLog`
 * filled - and if so, one generation for each of them. The caller runs the origin state again: the
 * ids widen every row, and a state computed without the rename is not one the rename can be applied
 * to afterwards, since the fixpoint unioned the two generations wherever their paths met.
 */
bool Analysis::findGenerations() {
    if(generationSource.size() || !BitMatrix::any(carriedLoans.pointer(), carriedLoans.size())) return false;

    generationOf.clear();
    for(U32 l = 0; l < mirLoans; l++) generationOf.push(kNoGeneration);

    BitMatrix::forEach(carriedLoans.pointer(), carriedLoans.size(), [&](Size l) {
        generationOf[l] = mirLoans + U32(generationSource.size());
        generationSource.push(U32(l));
    });

    return true;
}

void Analysis::stepOriginsForward(OriginCursor& cursor, Node& node) {
    // Only a `Write` and a call that installs change the state, and `computeNodeTables` resolved
    // what each one writes - most nodes have an empty range.
    if(!loanCount) return;

    // And the read that mints a loan some holder carried back to it - `generationOf`. What the
    // holders have from here on is the previous pass's, and the loan this read takes is new.
    if(generationSource.size() && node.kind == Node::Read) {
        auto& read = (NodeRead&)node;
        if(read.loan.valid() && generationOf[read.loan.index] != kNoGeneration) {
            renameGeneration(cursor, read.loan.index, generationOf[read.loan.index]);
        }
    }

    auto end = nodeTables[node.id.index + 1].writes;
    for(auto w = nodeTables[node.id.index].writes; w < end; w++) {
        auto& write = writes[w];
        if(write.argument == ~U32(0)) applyWrite(cursor, node, write);
        else applyInstall(cursor, node, write);
    }
}

/*
 * Which loans the state is kept over - see `Analysis::stateIndex`.
 *
 * Read off the nodes: the read that mints each loan, and whether that read's value is used by
 * anything other than a call whose result carries no origins. That is the whole rule, and it is
 * conservative in the only direction that matters - a use this walk does not understand is a use
 * that stores.
 */
void Analysis::classifyStored() {
    stateIndex.clear();
    stateLoan.clear();
    for(U32 l = 0; l < loanCount; l++) stateIndex.push(kNotStored);

    auto nodes = body.nodes.size();

    Array<U32> mintedBy;
    for(U32 l = 0; l < loanCount; l++) mintedBy.push(~U32(0));

    Array<U8> escapes;
    for(U32 i = 0; i < nodes; i++) escapes.push(0);

    // One walk for both tables, which nothing orders against each other.
    for(U32 i = 0; i < nodes; i++) {
        auto node = body.node(base, NodeId(i));

        if(node->kind == Node::Read) {
            auto& read = *(NodeRead*)node;
            if(read.loan.valid()) mintedBy[read.loan.index] = i;
        }

        // A call that installs is storing its arguments, into the `&` one - see
        // `computeCallTables` - whatever its own result carries.
        auto harmlessCall = node->kind == Node::Call && valuePositions(NodeId(i)) == 0 &&
                            !callInstalls(*node);

        forEachOperand(base, *node, [&](Operand& operand, OperandRole role) {
            if(operand.source() != SourceKind::Result) return;
            if(!harmlessCall || role != OperandRole::Argument) escapes[operand.nodeId().index] = 1;
        });
    }

    // An edge's arguments are not operands of its terminator - `forEachEdge` is their walk - and a
    // value carried to a block parameter can be stored by the block that receives it.
    for(U32 index = 0; index < blockCount; index++) {
        auto count = block(index)->nodes.size();
        if(!count) continue;

        forEachEdge(base, *base[block(index)->nodes.get(base, count - 1)], [&](Edge& edge) {
            for(auto supplied: edge.arguments.contents(base)) {
                if(supplied.source() == SourceKind::Result) escapes[supplied.nodeId().index] = 1;
            }
        });
    }

    for(U32 l = 0; l < loanCount; l++) {
        if(mintedBy[l] != ~U32(0) && !escapes[mintedBy[l]]) continue;

        stateIndex[l] = U32(stateLoan.size());
        stateLoan.push(l);
    }

    stateWords = U32((stateLoan.size() + 63) / 64);

    expanded.clear();
    for(U32 w = 0; w < loanWords; w++) expanded.push(0);

    rowBefore.clear();
    for(U32 w = 0; w < stateWords; w++) rowBefore.push(0);
}

void Analysis::expandInto(const U64* compact, U64* full) {
    BitMatrix::zero(full, loanWords);
    BitMatrix::forEach(compact, stateWords, [&](Size at) {
        BitMatrix::set(full, stateLoan[at]);
    });
}

void Analysis::compactInto(U64* compact, const U64* full, bool replace) {
    if(replace) BitMatrix::zero(compact, stateWords);

    BitMatrix::forEach(full, loanWords, [&](Size l) {
        auto at = stateIndex[l];

        // The classification's one claim: nothing a value carries into storage was classified out.
        assertTrue(at != kNotStored);
        if(at != kNotStored) BitMatrix::set(compact, at);
    });
}

/*
 * What each loan is to a write through it - see `loanRoots`. Every loan is rooted; the unique ones
 * are the lenders, and a lender that is a local with positions is where a write through it lands.
 */
void Analysis::computeLoanTables() {
    loanRoots.clear();
    uniqueLoans.clear();
    lentRange.clear();
    lent.clear();
    lenderRows = 0;

    for(U32 w = 0; w < loanWords; w++) uniqueLoans.push(0);

    IndexSet seen;
    seen.reset(body.locals.size());

    Projection projection;
    SmallArray<PositionMap, 8> map;

    for(U32 l = 0; l < loanCount; l++) {
        lentRange.push(U32(lent.size()));

        // A call bound has no place, and no write goes through one.
        if(callBound(l)) {
            loanRoots.push(nullptr);
            continue;
        }

        auto& loan = *loanAt(U32(l));
        auto root = rootOf(loan.place);
        loanRoots.push(root);

        if(loan.capability != Access::Unique) continue;
        BitMatrix::set(uniqueLoans.pointer(), l);

        if(root->kind != Place::Local) continue;

        auto lender = LocalId(root->payload);
        if(!localPositions(lender)) continue;
        if(seen.add(root->payload)) lenderRows += localPositions(lender);

        projectionOf(loan.place, projection);
        map.clear();
        selectPositions(projection, body.local(base, lender)->occurrence,
                        body.place(base, loan.place)->occurrence, map);

        for(auto& entry: map) {
            if(entry.from != 0) lent.push(entry.from);
        }
    }

    lentRange.push(U32(lent.size()));
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
 * What it leaves behind is `localIn` and `localOut`, a row per block. The per-point answer the
 * walks after it need is `buildOriginLog` plus an `OriginCursor` over it, because those walks read
 * storage's origins at the point they are asking about and three of them run backwards.
 */
/*
 * The call bounds - `bodyLoans` - one per lens call whose hand-over has a position only the call
 * bounds (`callBoundedAt`): the root of an unqualified hand-over, which is a view the callee may take
 * out of its own frame, and an interior position written `yield'`, which is what `%yield'T` says of
 * its pointee.
 *
 * Such a position's origins used to be the lens-call rule's union of the arguments' loans and nothing
 * else. That keeps the arguments alive for as long as the pointer is, which is right, and says
 * nothing about where the pointer points - so a copy of it looked like a statement to every check
 * that judges a position by its loans. A `&` parameter's `%kept'I32` field and a global's `%'I32`
 * accepted it because the set held `xs`'s loan, or accepted it because the set was empty where the
 * lens was lent nothing, and a caller's own writable local carried it past the call. All three read
 * a dead frame (J123). J122 had closed the result and the slot-naming hand-over by asking the value's
 * *type* instead (`boundedByCall`), which works only where a value is at hand, and a `&` parameter
 * or a global is judged by what storage holds.
 *
 * So the bound is a loan. It is carried where the call bounds, beside the arguments' loans, and every
 * holder that judges lenders meets it as `Lender::CallBound`, which none of them accepts; and its
 * extent is where a copy of the pointer is still live, which must stay inside the region the call
 * opened (`reportCallBoundEscapes`). What it is a loan *of* is the callee's frame, which this body has
 * no place for - so it has no `Loan` record, and every reader of loan places stops at `bodyLoans`.
 *
 * **The root too** (J129). It was left out on the premise that a binding of the hand-over is read
 * through a loan of the binding, which every holder refuses as `Borrowed` - true of a result, a `&`
 * parameter, a global and a hand-over, and not of the consumer's **own** storage: `b = {b | held: v}`
 * inside `let v = held(xs) in:` put nothing but the binding's origins into `b`, and those were the
 * arguments' loans, so `b.held` read after the call - or on the next pass of a `for` - read the
 * callee's freed local. With the bound at the root the same record carries it out of the region, and
 * J123's escape and J124's stale pass refuse it. A traversal that hands over the caller's storage
 * says so by naming the slot (`items`, `lines`), which puts no bound there. A position the
 * hand-over publishes never has one: a published slot says exactly what filled it.
 */
// Whether position `p` of a hand-over is one only the call bounds - the root of an unqualified
// hand-over, or an interior position written `yield'`. `findCallBounds` and the lens-call rule ask
// it, and must agree: the first decides which calls have a bound, the second where it goes.
bool Analysis::callBoundedAt(const Occurrence& handover, U32 p) {
    return bindsOriginAt(decl, handover, p) && originContractAt(decl, handover, p).origin() == Origin::CallBounded;
}

void Analysis::findCallBounds() {
    boundRegions.clear();
    boundCalls.clear();

    // The body's own region is the first and opens no call.
    for(U32 i = 1; i < body.regions.size(); i++) {
        auto region = body.region(base, ContinuationId(i));
        if(!region || region->kind != Continuation::Handover) continue;

        auto positions = originPositions(decl, region->handover);
        for(U32 p = 0; p < positions; p++) {
            if(!callBoundedAt(region->handover, p)) continue;

            boundRegions.push(i);
            boundCalls.push(NodeId());
            break;
        }
    }

    if(!boundRegions.size()) return;

    for(U32 i = 0; i < body.nodes.size(); i++) {
        auto& node = *body.node(base, NodeId(i));
        if(!isLensCall(node.kind)) continue;

        auto region = ((NodeLensCall&)node).region;
        for(U32 k = 0; k < boundRegions.size(); k++) {
            if(boundRegions[k] == region.index) boundCalls[k] = node.id;
        }
    }
}

/*
 * The storage a `Many` region carries around to its own entry, and out to its skip block - the two
 * edges `forEachSuccessor` adds and `Block::incoming` does not have (J124).
 *
 * The state is the one fixpoint here that read `incoming` rather than the successor relation, so the
 * back edge was missing from exactly the analysis that says what storage refers to. Liveness and the
 * loan extents had it; the origin state did not, so a local assigned a borrow in an `iter fn`'s `for`
 * body arrived at the next pass holding only what it held before the loop. `q = values(inner)` with
 * `inner` a local of the body then read the previous pass's `inner` through `q` with nothing keeping
 * it alive, and the unoptimized build printed what a freed run held.
 *
 * Only a block that **falls out** of the region, which is the `Jump` `forEachSuccessor` reads: a
 * `break` does not come back, and neither does a block that stays inside.
 *
 * And the one place a stale bound is minted. What arrives at the entry this way was stored on an
 * earlier pass, so bound `k` in it is that pass's frame - `staleBounds[k]` - and not the one the
 * parameter brings. Renamed here rather than refused here, because holding one is legal: a `let &q`
 * the body overwrites before reading is the ordinary shape, and only a *use* of it is the error.
 */
void Analysis::findRepeatedPasses() {
    repeatedRegions.clear();
    repeatedExitStart.clear();
    repeatedExits.clear();
    repeatedRowStart.clear();
    repeatedRows.clear();

    for(U32 i = 1; i < body.regions.size(); i++) {
        auto region = body.region(base, ContinuationId(i));
        if(!region || region->multiplicity != Continuation::Many) continue;

        repeatedRegions.push(i);
        repeatedExitStart.push(U32(repeatedExits.size()));
        repeatedRowStart.push(U32(repeatedRows.size()));

        for(U32 b = 0; b < blockCount; b++) {
            auto from = this->block(b);
            if(from->region != region->id) continue;

            auto count = from->nodes.size();
            if(!count) continue;

            auto last = base[from->nodes.get(base, count - 1)];
            if(last->kind != Node::Jump) continue;

            auto fallsOut = false;
            forEachEdge(base, *last, [&](Edge& edge) {
                if(edge.target && base[edge.target]->region != region->id) fallsOut = true;
            });
            if(fallsOut) repeatedExits.push(b);
        }

        for(U32 l = 0; l < body.locals.size(); l++) {
            auto local = body.local(base, LocalId(l));
            if(!local->region.valid() || !body.encloses(base, region->id, local->region)) continue;

            auto positions = localPositions(LocalId(l));
            for(U32 p = 0; p < positions; p++) repeatedRows.push(localRow[l] + p);
        }
    }

    repeatedExitStart.push(U32(repeatedExits.size()));
    repeatedRowStart.push(U32(repeatedRows.size()));
}

void Analysis::joinRepeatedPasses(U32 index, U64* state, SmallArray<U64, 8>& scratch) {
    if(!repeatedRegions.size()) return;

    auto block = this->block(index);
    auto width = localWidth();

    for(U32 j = 0; j < repeatedRegions.size(); j++) {
        auto region = body.region(base, ContinuationId(repeatedRegions[j]));

        auto entry = region->entry == block->index;
        if(!entry && !(region->otherwise.valid() && region->otherwise == block->index)) continue;

        // The bounds of this region that have a previous pass - once per region, not per exit.
        SmallArray<U32, 2> renamed;
        if(entry) {
            for(U32 k = 0; k < boundRegions.size(); k++) {
                if(boundRegions[k] == region->id.index && staleBounds[k] != kNoStaleBound) {
                    renamed.push(k);
                }
            }
        }

        for(U32 e = repeatedExitStart[j]; e < repeatedExitStart[j + 1]; e++) {
            auto row = localsAt(localOut, repeatedExits[e]);

            // Into the skip block, what the pass left is what the alternative finds.
            if(!entry) {
                BitMatrix::unionInto(state, row, width);
                continue;
            }

            scratch.clear();
            for(Size w = 0; w < width; w++) scratch.push(row[w]);

            /*
             * A local the region declares does not survive the pass: it is the continuation's, and
             * the next pass starts with a new frame and writes it before reading it. Carried, a
             * binding's weak update - `write.init (r)` of a borrowing local unions rather than
             * replaces - kept the previous pass's row beside the new one, and `r` looked stale.
             */
            for(U32 at = repeatedRowStart[j]; at < repeatedRowStart[j + 1]; at++) {
                auto cells = scratch.pointer() + Size(repeatedRows[at]) * stateWords;
                for(U32 w = 0; w < stateWords; w++) cells[w] = 0;
            }

            for(U32 row = 0; row < originRowCount; row++) {
                auto cells = scratch.pointer() + Size(row) * stateWords;
                for(auto k: renamed) {
                    // Both are in the state's domain: no `Read` mints a bound or a stale bound, and
                    // `classifyStored` keeps every loan no read mints.
                    auto current = stateIndex[bodyLoans + k];
                    auto stale = stateIndex[staleBounds[k]];
                    assertTrue(current != kNotStored && stale != kNotStored);
                    if(!BitMatrix::test(cells, current)) continue;

                    BitMatrix::clear(cells, current);
                    BitMatrix::set(cells, stale);
                }
            }

            BitMatrix::unionInto(state, scratch.pointer(), width);
        }
    }
}

void Analysis::computeOrigins() {
    mirLoans = body.loans.size();
    bodyLoans = mirLoans + generationSource.size();

    /*
     * The second run, with generations (J127), keeps what the first found about the body's shape:
     * the call bounds, the node tables and the repeated regions depend on the nodes, the regions and
     * the locals' positions, and not on how many loans there are. What does depend on the count -
     * every row's width, the loan tables and which loans the state is kept over - is built again.
     */
    auto again = generationSource.size() != 0;
    if(!again) findCallBounds();
    loanCount = bodyLoans + boundRegions.size();

    // The previous pass of each bound whose region repeats - `staleBounds`.
    staleBounds.clear();
    for(U32 k = 0; k < boundRegions.size(); k++) {
        auto region = body.region(base, ContinuationId(boundRegions[k]));
        staleBounds.push(region->multiplicity == Continuation::Many ? loanCount++ : kNoStaleBound);
    }

    loanWords = U32((loanCount + 63) / 64);

    localRow.clear();
    valueRow.clear();
    logStart.clear();
    log.clear();
    originRowCount = 0;
    if(!loanCount) return;

    // The two prefix sums, and they are built the same way from the same question: how many origin
    // positions this occurrence has. A local or a value that carries none contributes an empty range
    // rather than a reserved index - see `Analysis::valueRow`.
    for(U32 i = 0; i < body.locals.size(); i++) {
        localRow.push(originRowCount);
        originRowCount += originPositions(decl, body.local(base, LocalId(i))->occurrence);
    }

    localRow.push(originRowCount);

    U32 valueRowCount = 0;
    for(U32 i = 0; i < body.nodes.size(); i++) {
        valueRow.push(valueRowCount);

        auto node = body.node(base, NodeId(i));
        if(hasResult(node->kind)) valueRowCount += originPositions(decl, valueOccurrence(*node));
    }

    valueRow.push(valueRowCount);

    // After the prefix sums, which all three read. The loan tables before the node tables, whose
    // log bound counts `lenderRows`; both before `classifyStored`, which asks `callInstalls` and
    // sizes the tables below.
    computeLoanTables();
    if(!again) computeNodeTables();
    classifyStored();

    throughScratch.clear();
    for(U32 w = 0; w < U32(ThroughRow::Count) * loanWords; w++) throughScratch.push(0);

    lendersSeen.clear();
    for(U32 w = 0; w < loanWords; w++) lendersSeen.push(0);

    destinationParameters.clear();
    for(U32 i = 0; i < body.locals.size(); i++) {
        auto& local = *body.local(base, LocalId(i));
        if(!local.isParameter() || frameOwnedParameter(local) || localPositions(LocalId(i)) < 2) {
            continue;
        }

        auto& parameter = *decl[function.args.get(decl, local.parameter)];
        if(parameter.occurrence.contract.access() == Access::Unique) {
            destinationParameters.push(LocalId(i));
        }
    }

    valueOrigins.reset(valueRowCount, loanCount);
    if(!again) findRepeatedPasses();

    localIn.resetWords(blockCount, localWidth());
    localOut.resetWords(blockCount, localWidth());

    auto width = localWidth();

    /*
     * The running state, refilled from the block's predecessors at the top of each block.
     *
     * A cursor rather than a bare buffer, because the transfer and the node rules both ask
     * `forEachOperandOrigin` what storage holds *here* - and the answer has to be this walk's own state
     * rather than a published row. It used to be a published row: the state was unioned into
     * `nodeLocals` before each run and read back out of it, which made every node's operands see the
     * accumulated union over every iteration so far rather than this iteration's answer. Reading the
     * running state is the same fixpoint arrived at directly, since the lattice only grows and the
     * last visit to a block is the one that decides it.
     */
    OriginCursor cursor { *this };
    SmallArray<U64, 8> passScratch;

    // Nothing walks the log while it is being built, so an empty range per block is what the cursor
    // reads until `buildOriginLog` fills it in.
    for(U32 i = 0; i <= blockCount; i++) logStart.push(0);

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
        BitMatrix::zero(cursor.state(), width);

        for(auto predecessorPointer: block->incoming.contents(base)) {
            auto from = localsAt(localOut, base[predecessorPointer]->index.index);
            BitMatrix::unionInto(cursor.state(), from, width);
        }

        joinRepeatedPasses(index, cursor.state(), passScratch);

        BitMatrix::unionInto(localsAt(localIn, index), cursor.state(), width);
        cursor.blockIndex = index;

        for(U32 position = 0; position < count; position++) {
            auto& node = nodeAt(index, position);

            switch(node.kind) {
                case Node::Read: {
                    auto& read = (NodeRead&)node;
                    auto positions = valuePositions(node.id);
                    if(!positions) break;

                    // The loan a borrow mints is what the *reference* points at, so it lands on the
                    // result's root and never on one of its interior slots.
                    if(read.loan.valid()) {
                        BitMatrix::set(originsOfValue(node.id, 0), read.loan.index);
                    }

                    /*
                     * What the storage was rooted in, where the value that comes out of it can
                     * still refer to that storage - position by position.
                     *
                     * A borrow inherits its lender's origins, which is what makes a reborrow carry
                     * the outer loan along. A *copy* or a *move* of a record with an external field
                     * inherits them too, and that is the half a rule about borrowing alone would
                     * miss: what travels is the record, and the reference inside it goes on
                     * referring to the same object.
                     *
                     * The positions are what keeps those two from being one statement. A whole read
                     * of a `Two` carries both of its fields' origins and carries each to the field
                     * it came from; a read of `t.right` carries one, and to the result's root,
                     * because what comes out is the guard rather than a record containing it. That
                     * mapping is `selectPositions` and this rule does not repeat it.
                     *
                     * A result with no positions inherits nothing, and the empty range says so: an
                     * `I32` copied out of a borrowed record is a freshly rooted value, and a read
                     * that passed the lender's origins on to it would make
                     * `fn depth(r: a'Reader) -> I32 = r.at` a function returning a reference to its
                     * argument.
                     */
                    forEachOperandOrigin(cursor, read.place, [&](U32 position, const U64* from) {
                        auto to = position < positions ? position : 0;
                        BitMatrix::unionInto(originsOfValue(node.id, to), from, loanWords);
                    }, copiedReferent(read, valueOccurrence(node)));

                    break;
                }

                /*
                 * A record is rooted in whatever its components were - §5.11's "a constructor maps
                 * component slots into result slots", now one set per result slot.
                 *
                 * Component `i` fills the schema position whose path is `[i]`, which is what makes
                 * `Two {left: g, right: h}` two relationships rather than one union of two guards.
                 * A component the record *owns* has no slot to fill: `classifyTypes` refuses an
                 * owned field whose type refers to storage the record does not name. The one owned
                 * component that carries anything is a container's - `Array {run: ...}` over a
                 * `Run(Store)` - and it fills the result's element position (`hidesLoans`).
                 */
                case Node::Aggregate: {
                    auto& built = (NodeAggregate&)node;
                    auto positions = valuePositions(node.id);
                    if(!positions) break;

                    auto schema = shapeOf(decl, valueOccurrence(node));
                    auto components = built.components.contents(base);

                    // A component no slot names is inside the elements of a container that has
                    // them - `Array {run: ...}` over a `Run(Store)` - and at the root of anything
                    // else, which is the conservative answer.
                    auto unnamed = elementPosition(decl, valueOccurrence(node));

                    /*
                     * Component `i` is field `first + i` of the constructor built, which is the index
                     * a schema path names - a sum's second constructor starts past the first one's
                     * fields. A record type and a matching arity are the case with a declaration to
                     * read; anything else keeps the component index, which is what it always was.
                     */
                    U32 first = 0;
                    if(auto type = valueOccurrence(node).type; type && decl[type]->kind == Type::Record) {
                        auto& record = *(RecordType*)decl[type];
                        if(built.constructor < record.cons.size()) {
                            auto con = record.cons.get(decl, built.constructor);
                            if(con.count == components.size()) first = con.first;
                        }
                    }

                    auto startsAt = [&](ExternalSlot& slot, U32 field) {
                        return slot.path.size() >= 1 &&
                               slot.path.get(decl, 0) == PathStep::field(U16(field));
                    };

                    for(U32 i = 0; i < components.size(); i++) {
                        U32 into = unnamed;
                        auto field = first + i;

                        if(schema) {
                            auto& slots = *decl[schema];
                            for(U32 j = 0; j < slots.slots.size(); j++) {
                                auto& slot = *decl[slots.slots.get(decl, j)];
                                if(slot.path.size() != 1 || !startsAt(slot, field)) continue;

                                into = 1 + j;
                                break;
                            }
                        }

                        auto row = originsOfValue(node.id, into < positions ? into : 0);
                        auto& component = *components.pointerAt(i);

                        /*
                         * A component that is itself a record holding references - `L2 {a:
                         * wrap1(n)}` (J141). Its positions are the result's at `[field]` followed by
                         * the component's own path, which is how `foldRecordSchema` placed them, so
                         * each goes where its path says rather than all of them into one row. The
                         * root and anything unmatched keep the answer above.
                         */
                        // A record component only: an interior position the field's own
                        // occurrence named - `at: %buf'U8` - is at the root's row as it was.
                        DeclPtr<OwnershipSchema> nested = nullptr;
                        if(schema && into == unnamed) {
                            const Occurrence* held = nullptr;
                            if(component.source() == SourceKind::Result) {
                                held = &valueOccurrence(*body.node(base, component.nodeId()));
                            } else if(component.source() == SourceKind::Place) {
                                held = &body.place(base, component.placeId())->occurrence;
                            }

                            if(held && held->type && decl[held->type]->kind == Type::Record) {
                                nested = shapeOf(decl, *held);
                            }
                        }

                        /*
                         * Where each of the component's own positions lands: `nestedAt[k]` is the
                         * result position of the component's slot `k`, or zero for its root row.
                         * One merge walk, because `foldRecordSchema` pushes a nested field's slots
                         * contiguously and in the inner schema's order (skipping the ones that name
                         * no set) - so each inner slot is found at or after where the last was,
                         * rather than by a search of the whole schema per position.
                         */
                        SmallArray<U32, 8> nestedAt;
                        if(nested) {
                            auto& inner = *decl[nested];
                            auto& outer = *decl[schema];
                            U32 j = 0;

                            for(U32 k = 0; k < inner.slots.size(); k++) {
                                auto& want = *decl[inner.slots.get(decl, k)];
                                nestedAt.push(0);

                                for(U32 at = j; at < outer.slots.size(); at++) {
                                    auto& slot = *decl[outer.slots.get(decl, at)];
                                    if(slot.path.size() != 1 + want.path.size() || !startsAt(slot, field)) continue;

                                    auto same = true;
                                    for(Size step = 0; step < want.path.size() && same; step++) {
                                        same = slot.path.get(decl, 1 + step) == want.path.get(decl, step);
                                    }
                                    if(!same) continue;

                                    if(1 + at < positions) nestedAt[k] = 1 + at;
                                    j = at + 1;
                                    break;
                                }
                            }
                        }

                        forEachOperandOrigin(cursor, component, [&](U32 p, const U64* from) {
                            auto target = row;
                            if(p && p <= nestedAt.size() && nestedAt[p - 1]) {
                                target = originsOfValue(node.id, nestedAt[p - 1]);
                            }

                            BitMatrix::unionInto(target, from, loanWords);
                        });

                        /*
                         * JavaScript's literal: the run is `host.array` over the elements, a `%a`
                         * with no position of its own, so what the elements carried is read off the
                         * host node's operands. Native's writes the elements through the run's
                         * address instead, which is `localWriteOf`'s `RunSlots` case.
                         */
                        if(!unnamed || component.source() != SourceKind::Result) continue;

                        auto producer = body.node(base, component.nodeId());
                        if(producer->kind != Node::Host || ((NodeHost&)*producer).op != HostOp::Array) {
                            continue;
                        }

                        for(auto element: ((NodeHost&)*producer).arguments.contents(base)) {
                            forEachOperandOrigin(cursor, element, [&](U32, const U64* from) {
                                BitMatrix::unionInto(row, from, loanWords);
                            });
                        }
                    }

                    break;
                }


                /*
                 * A **closure** is rooted, at its one interior position, wherever its environment
                 * was - `Shapes::closure`.
                 *
                 * One line and not a rule of its own: the environment operand is a borrow of a
                 * local of this frame, so it arrives here carrying that loan and whatever the
                 * captures inside it carry, and what this says is only *where in the value* those
                 * end up. The root stays empty, which is the true statement about it - the pair is
                 * this frame's own and refers to nothing; what refers to something is the word
                 * inside it.
                 */
                case Node::FunValue: {
                    auto& value = (NodeFunValue&)node;
                    if(!value.capturing) break;

                    auto positions = valuePositions(node.id);
                    if(positions < 2) break;

                    auto into = originsOfValue(node.id, 1);
                    forEachOperandOrigin(cursor, value.environment, [&](U32, const U64* from) {
                        BitMatrix::unionInto(into, from, loanWords);
                    });

                    break;
                }

                /*
                 * Pointer arithmetic and a pointer-to-pointer cast keep what the pointer refers
                 * to: `advance(values(xs), i)` names the same storage `values(xs)` did, one element
                 * along, and `bitcast(p) :: %U8` the same bytes read differently. The pointee
                 * position is the result's second, exactly as `Address` below writes it, and it is
                 * filled from the first operand's - a pointer's provenance is not something an
                 * offset contributes to. A result with no pointee position - an integer, a pointer
                 * whose type published nothing - carries nothing and is left alone.
                 */
                case Node::Binary:
                case Node::Unary: {
                    auto positions = valuePositions(node.id);
                    if(positions < 2) break;
                    if(decl[valueOccurrence(node).type]->kind != Type::Pointer) break;

                    auto& operand = node.kind == Node::Binary ? ((NodeBinary&)node).lhs
                                                              : ((NodeUnary&)node).value;

                    auto into = originsOfValue(node.id, 1);
                    forEachOperandOrigin(cursor, operand, [&](U32 at, const U64* from) {
                        if(at != 1) return;
                        BitMatrix::unionInto(into, from, loanWords);
                    });

                    break;
                }

                /*
                 * A **raw pointer** is rooted, at its one interior position, wherever the loan it was
                 * taken from was - Analysis-Lens.md §12.2, and `FunValue`'s rule one type over.
                 *
                 * Position one and not the root, and the difference is the whole of §12.3. The
                 * pointer *value* is an owned scalar this frame may copy anywhere; what refers to
                 * somebody's storage is the address inside it, which is exactly what an interior
                 * position is for. Putting it at the root would make a copied pointer a borrow of
                 * the frame it was copied in, and putting it nowhere is the use-after-free §11.5
                 * exists to prevent - `addressOf(v)` returned from the function that declared `v`,
                 * with `checkReturnConformance` finding an empty set and nothing to object to.
                 *
                 * That check is then the whole of §12.3(c) with nothing added: a result declared
                 * `%T` has no origin at either position, so a pointer carrying one is reported where
                 * a result borrowing without saying from where already is.
                 */
                case Node::Address: {
                    auto& taken = (NodeAddress&)node;
                    auto positions = valuePositions(node.id);
                    if(positions < 2) break;

                    auto into = originsOfValue(node.id, 1);
                    forEachOperandOrigin(cursor, taken.of, [&](U32, const U64* from) {
                        BitMatrix::unionInto(into, from, loanWords);
                    });

                    break;
                }

                case Node::Call: {
                    auto& call = (NodeCall&)node;

                    /*
                     * A call through a function **value** relates only what its type publishes.
                     *
                     * A function type numbers its origins in a scope of its own (J141), and
                     * `computeValueCallTables` reads it as a declaration is read. A type publishing
                     * nothing in its result has no table, and its result comes back rooted in
                     * nothing - the true answer, since that result is owned.
                     */
                    if(call.callsValue() && !valueCallType(call)) break;

                    auto positions = valuePositions(node.id);
                    if(!positions) break;

                    /*
                     * Every position of the result, bound to whatever filled the slot it names.
                     *
                     * One loop over the slot axis rather than a rule for the root and a spread for
                     * the rest. A signature can now say what the result's *interior* positions refer
                     * to - `-> Reader(a')` - so the position and the slot are two indexes and
                     * `originSlotAt` is the map between them: the root reads the result's own
                     * contract and an interior position reads the bound shape, with `kNoOriginSlot`
                     * where the declaration said nothing.
                     *
                     * A position with no slot is left empty on purpose. `checkReturnConformance`
                     * refuses a body that puts a loan there, so an empty set is what a conforming
                     * callee actually produces - and spreading the root across it, which is what
                     * this did while `Reader(a')` could not be written, would credit the result with
                     * loans it does not hold the moment one of these is real.
                     *
                     * Every argument whose parameter publishes the same slot contributes, at
                     * whichever of *its* positions names it, and the result may derive from any of
                     * them - §2.2's `choose`, whose result binds `source` to the union and keeps
                     * both alive. Deliberately conservative, and deliberately not a claim that the
                     * members are one object.
                     *
                     * A generic callee relates its positions through `groupAt`'s open group, which
                     * is the same statement for a type the signature could not describe: `fn (a)
                     * id(->x: a) -> a` hands back whatever `x` held, and a `Store` with a loan in
                     * it is one of the things `x` can be.
                     *
                     * Arguments outer and result positions inner, so that each argument's origins
                     * are asked for once rather than once per result position; the groups are the
                     * call's table (`computeCallTables`), and a union is a union in either order.
                     */
                    auto arguments = call.arguments.contents(base);
                    auto count = callArguments(node.id);

                    for(U32 i = 0; i < count; i++) {
                        forEachOperandOrigin(cursor, *arguments.pointerAt(i),
                                             [&](U32 at, const U64* row) {
                            auto group = callGroup(node.id, i, at);
                            if(group == kNoOriginSlot) return;

                            for(U32 p = 0; p < positions; p++) {
                                if(callGroup(node.id, count, p) != group) continue;
                                BitMatrix::unionInto(originsOfValue(node.id, p), row, loanWords);
                            }
                        });
                    }

                    break;
                }

                /*
                 * A lens call, whose result arrives at the **continuation's parameter** rather than
                 * at this node.
                 *
                 * The same mapping the ordinary call above makes, and it has to be here rather than
                 * with the edges below: what travels this edge is produced inside the callee, so the
                 * terminator supplies no argument for the entry's parameter to be rooted in and the
                 * edge walk finds nothing to union. `Builder::addLensCall` says as much where it
                 * fills the edge by hand.
                 *
                 * The slot is read off `Continuation::handover` and not off `callee.result`, which
                 * is the one difference from a call: an `iter fn`'s declared result is the
                 * continuation's answer - `$r` - and what is handed over is the continuation
                 * *parameter's* type, which is what the region records.
                 *
                 * Without it a container is dead the moment the traversal starts. `for x in
                 * items(xs)` borrows `xs`, hands a window into its storage to the body, and nothing
                 * relates the two - so the container's last use is the call, and `placeDrops` puts
                 * its teardown at the top of the loop body, releasing the run the window points
                 * into before the first element is read.
                 */
                case Node::LensCall:
                case Node::LensTry: {
                    auto& call = (NodeLensCall&)node;
                    if(!call.to.target) break;

                    auto entry = base[call.to.target];
                    auto region = body.region(base, call.region);
                    if(!region || !entry->arguments.size()) break;

                    // The handover is argument zero of the entry, which is where `handoverCall`
                    // declares it - before any carried value, since the block is made for it.
                    auto parameter = base[entry->arguments.get(base, 0)];
                    auto positions = valuePositions(parameter->id);
                    if(!positions) break;

                    auto& callee = *decl[call.callee];
                    auto arguments = call.arguments.contents(base);
                    auto changed = false;

                    for(U32 p = 0; p < positions; p++) {
                        auto slot = originSlotAt(decl, region->handover, p);
                        auto into = originsOfValue(parameter->id, p);

                        for(U32 i = 0; i < arguments.size(); i++) {
                            auto& declared = *decl[callee.args.get(decl, i)];

                            forEachOperandOrigin(cursor, *arguments.pointerAt(i),
                                                 [&](U32 at, const U64* row) {
                                /*
                                 * An **unqualified** hand-over names no slot, and takes every
                                 * argument's loans rather than none.
                                 *
                                 * `iter fn (Chunked(c, a)) items(self: c) -> a` is the one the
                                 * library writes, and `-> a` is already a loan bounded by the
                                 * continuation - Analysis-Borrows-Revised.md §3.3 - so there is no
                                 * `'` for the matching below to key on and the slot-keyed walk
                                 * related nothing. The container's last mention was then the
                                 * traversal itself, and `placeDrops` released it at the top of the
                                 * loop body: `for x in items(build())` read the first element and
                                 * then walked a freed run.
                                 *
                                 * Every argument and not a guess about which, because that is what
                                 * a continuation-bounded hand-over says: the value is the callee's
                                 * to describe and the callee is still running, so what is behind it
                                 * is anything the call was lent. Conservative in the only direction
                                 * that is safe - it keeps storage alive rather than releasing it -
                                 * and exact wherever the signature published a slot, which is the
                                 * arm above.
                                 */
                                if(slot != kNoOriginSlot &&
                                   originSlotAt(decl, declared.occurrence, at) != slot) {
                                    return;
                                }

                                changed = BitMatrix::unionInto(into, row, loanWords) || changed;
                            });
                        }
                    }

                    // And the call's bound where only the call bounds - `findCallBounds`.
                    for(U32 k = 0; k < boundRegions.size(); k++) {
                        if(boundRegions[k] != call.region.index) continue;

                        for(U32 p = 0; p < positions; p++) {
                            if(!callBoundedAt(region->handover, p)) continue;

                            auto into = originsOfValue(parameter->id, p);
                            if(BitMatrix::test(into, bodyLoans + k)) continue;

                            BitMatrix::set(into, bodyLoans + k);
                            changed = true;
                        }
                    }

                    if(changed) wake(entry->index.index);
                    break;
                }

                default:
                    break;
            }

            // Storage last, so that a write's own value operand was read under the state the node
            // started in - `stepOriginsForward` is the only thing that changes it.
            stepOriginsForward(cursor, node);
        }

        auto moved = BitMatrix::unionInto(localsAt(localOut, index), cursor.state(), width);

        /*
         * The edges, which is where a block parameter's set comes from: the operand a predecessor
         * supplied is what the parameter holds on that path, so the parameter is rooted in the union
         * over the predecessors.
         *
         * Read with the cursor standing at the block's exit - the state *after* the terminator,
         * not on entry to it. The two differ at one kind of node: a lens call is a terminator, and
         * one that installs into a `&` argument steps the state at itself. An installation is a
         * weak update, so the exit state holds everything the entry state did and an edge operand
         * read under it can only be credited with more origins than it had - the conservative
         * direction, and the one the continuation, entered after the callee ran, would see anyway.
         */
        if(count) {
            auto last = base[block->nodes.get(base, count - 1)];
            forEachEdge(base, *last, [&](Edge& edge) {
                if(!edge.target) return;

                auto target = base[edge.target];
                auto supplied = edge.arguments.contents(base);
                auto changed = false;

                for(U32 i = 0; i < supplied.size() && i < target->arguments.size(); i++) {
                    auto parameter = base[target->arguments.get(base, i)];
                    auto positions = valuePositions(parameter->id);
                    if(!positions) continue;

                    // Position for position: the operand and the parameter are the same occurrence
                    // on this edge, so slot `i` of what was supplied is slot `i` of what arrives.
                    forEachOperandOrigin(cursor, *supplied.pointerAt(i),
                                         [&](U32 position, const U64* from) {
                        auto to = position < positions ? position : 0;
                        changed |= BitMatrix::unionInto(originsOfValue(parameter->id, to), from,
                                                        loanWords);
                    });
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
 * The undo log, from the settled fixpoint - one pass, and the only walk that writes `log`.
 *
 * After the fixpoint rather than inside it, and that is exact rather than an approximation. Every
 * table here only ever gains bits and the transfer is monotone, so a block's last visit is the one
 * that decides it: walking each block once more from its settled `localIn` reproduces the state the
 * fixpoint ended with at every point in it. Doing it during the sweep would mean logging an edit per
 * visit and undoing the wrong one, since the row a write replaced is different on each round up.
 *
 * `localOut` is written rather than unioned into, so that the state a backward walk is seeded with
 * is by construction the one this forward walk ended at. At convergence that changes nothing; what
 * it removes is the possibility of the two ends of a block disagreeing without anything saying so.
 */
void Analysis::buildOriginLog() {
    if(!loanCount) return;

    logStart.clear();
    log.clear();

    // Sized to the bound `computeNodeTables` summed while it resolved the writes - see `logPrevious`
    // - and a row per state row at every read that renames a generation, which may move them all.
    auto bound = logBound;
    if(generationSource.size()) {
        for(U32 i = 0; i < body.nodes.size(); i++) {
            auto node = body.node(base, NodeId(i));
            if(node->kind != Node::Read || !((NodeRead*)node)->loan.valid()) continue;
            if(generationOf[((NodeRead*)node)->loan.index] != kNoGeneration) bound += originRowCount;
        }
    }

    logPrevious.resetWords(bound, stateWords);

    // Which loans a holder carries back to the read that mints them - see `generationOf`. Asked
    // only before there are generations: after, every such loan has one.
    auto detect = !generationSource.size();
    carriedLoans.clear();
    for(U32 w = 0; w < loanWords; w++) carriedLoans.push(0);

    /*
     * And which locals can carry one there at all - the rows the detection reads.
     *
     * A local only ever *bound* - every write to it an `Init` of the whole local - cannot: a `let`
     * inside the loop is out of scope before the read that mints its loan again, and is bound anew
     * before anything reads it, so the previous pass's row it still has at the mint is dead. Only a
     * local something assigns, or writes part of, or installs into, or that lends a unique loan a
     * write can go through (`applyThrough` moves the lender's row), is asked. Without the filter
     * every `let x = xs[i]` in a loop made its body run the origin state twice: 140 bodies of
     * `test/base`, and +0.8% instructions on `-mode mir`.
     */
    SmallArray<U32, 16> carrierRows;
    if(detect && loanCount) {
        SmallArray<U8, 16> carries;
        for(U32 l = 0; l < body.locals.size(); l++) carries.push(0);

        auto mark = [&](PlaceId place) {
            if(!place.valid()) return;

            auto root = rootOf(place);
            if(root->kind == Place::Local) carries[root->payload] = 1;
        };

        for(U32 i = 0; i < body.nodes.size(); i++) {
            auto node = body.node(base, NodeId(i));

            if(node->kind == Node::Write) {
                auto& write = *(NodeWrite*)node;
                if(write.destination.source() != SourceKind::Place) continue;

                auto place = body.place(base, write.destination.placeId());
                if(write.mode != Destination::Init || place->parent) mark(write.destination.placeId());
            } else if(callInstalls(*node)) {
                forEachOperand(base, *node, [&](Operand& operand, OperandRole) {
                    mark(installDestination(operand));
                });
            }
        }

        for(U32 l = 0; l < mirLoans; l++) {
            auto loan = body.loan(base, LoanId(l));
            if(loan->capability == Access::Unique) mark(loan->place);
        }

        for(U32 l = 0; l < body.locals.size(); l++) {
            if(!carries[l]) continue;
            for(U32 row = localRow[l]; row < localRow[l + 1]; row++) carrierRows.push(row);
        }
    }

    OriginCursor cursor { *this };
    cursor.logging = true;
    auto width = localWidth();

    for(U32 index = 0; index < blockCount; index++) {
        logStart.push(log.size());

        // Seeded from `localIn` directly rather than through `cursor.enter`, which reads the very
        // `logStart` this loop is still filling in.
        cursor.blockIndex = index;
        if(width) BitMatrix::copy(cursor.state(), localsAt(localIn, index), width);

        auto count = nodeCount(index);

        /*
         * The step itself logs: `writeStateRow` records each row it changes with what the row held
         * before that write, so the entries are what the transfer did - one per row a whole write
         * replaced, and for a write through a loan whichever lender rows `applyThrough` moved.
         *
         * This used to be a prediction. The rows a write names were logged before the step,
         * whether or not it moved them, and around a write through a loan every row of every
         * local that could be a lender was saved and compared after it - a copy and a compare of
         * all of them at every such write, whatever it reached. A node may log one row twice, from
         * two installations or from a write and a lender: `undo` restores in reverse, so the
         * earlier entry's contents - the row on entry to the node - win.
         */
        for(U32 position = 0; position < count; position++) {
            cursor.position = position;
            auto& node = nodeAt(index, position);

            if(detect && node.kind == Node::Read && ((NodeRead&)node).loan.valid()) {
                auto l = ((NodeRead&)node).loan.index;
                auto at = stateIndex[l];

                if(at != kNotStored && !BitMatrix::test(carriedLoans.pointer(), l)) {
                    for(auto row: carrierRows) {
                        if(!BitMatrix::test(cursor.state() + Size(row) * stateWords, at)) continue;

                        BitMatrix::set(carriedLoans.pointer(), l);
                        break;
                    }
                }
            }

            stepOriginsForward(cursor, node);
        }

        if(width) BitMatrix::replace(localsAt(localOut, index), cursor.state(), width);
    }

    logStart.push(log.size());
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
    heldRegions.clear();
    regionCall.clear();
    if(!loanCount) return;

    loanIn.reset(blockCount, loanCount);
    loanOut.reset(blockCount, loanCount);

    SmallArray<U64, 4> live;
    for(U32 w = 0; w < loanWords; w++) live.push(0);

    /*
     * Which loans may exist at each block's exit: minted on some path from the entry to it.
     *
     * A block argument's origins are the union over every edge into it, so a merge's `loanIn` holds
     * the loans of *all* its predecessors, and the backward union below would hand the `else` arm's
     * loans to the `then` arm - where they were never taken. `let c = if f then pick(x, y) else
     * pick(y, x)` then refused the `then` arm's borrow of `x` as "already borrowed exclusively" by
     * the `else` arm's. A loan cannot be live on a path it was not minted on, so a block's exit is
     * masked by what can reach it; a loop's loans reach its header through the back edge, so they
     * stay live around it as before. Masking the edge union rather than removing the block
     * arguments' loans from it, because a loan a block argument carries may also be held by a local
     * that is live across the merge.
     */
    BitMatrix minted;
    minted.reset(blockCount, loanCount);

    /*
     * What a traversal holds for as long as its region runs - every loan its arguments carry, per
     * `lens.call` region (Implementation-Streams P4, J185).
     *
     * A lens call lends its arguments to a callee that is **still running** while the region's
     * blocks run: an `iter fn` resumes on every fall-out and a lens runs its cleanup after the last
     * one, both with those arguments in hand. So the loans are live on every block the region
     * encloses, exactly as an argument's loan is live across an ordinary call. Before this they were
     * live only through the hand-over's origins, so once the element's last use had passed nothing
     * held them: `for x in blocks(s): ...; s.at = 0` was accepted while `blocks` still had `&s`, and
     * looped forever. Stated here as a use at every block of the region, rather than as origins of
     * the hand-over, because it is a fact about the call's extent and not about any value - a lens
     * handing over `{}` holds `&m` all the same. `computeLiveness` reads the same rows, so the
     * storage behind them is not released inside the region either.
     */
    {
        OriginCursor replay { *this };
        for(U32 b = 0; b < blockCount; b++) {
            replay.enter(b);

            auto count = nodeCount(b);
            for(U32 n = 0; n < count; n++) {
                auto& node = nodeAt(b, n);

                if(isLensCall(node.kind)) {
                    auto& call = (NodeLensCall&)node;
                    if(call.region.valid()) {
                        if(!heldRegions.size()) regionHeld.reset(U32(body.regions.size()), loanCount);
                        heldRegions.push(call.region.index);

                        while(regionCall.size() <= call.region.index) regionCall.push(kNoCall);
                        regionCall[call.region.index] = b;

                        auto row = regionHeld.row(call.region.index);
                        for(auto operand: call.arguments.contents(base)) {
                            forEachOperandOrigin(replay, operand, [&](U32, const U64* origins) {
                                BitMatrix::unionInto(row, origins, loanWords);
                            });
                        }
                    }
                }

                stepOriginsForward(replay, node);
            }
        }
    }

    sweepForward([&](U32 index) {
        auto block = this->block(index);
        BitMatrix::zero(live.pointer(), loanWords);

        forEachPredecessor(index, [&](U32 from) {
            BitMatrix::unionInto(live.pointer(), minted.row(from), loanWords);
        });

        for(U32 n = 0; n < block->nodes.size(); n++) {
            auto& node = nodeAt(index, n);
            if(node.kind != Node::Read) continue;

            auto& read = (NodeRead&)node;
            if(!read.loan.valid()) continue;

            BitMatrix::set(live.pointer(), read.loan.index);

            // And its previous generation, which the same read names - `renameGeneration`.
            if(generationSource.size() && generationOf[read.loan.index] != kNoGeneration) {
                BitMatrix::set(live.pointer(), generationOf[read.loan.index]);
            }
        }

        // A call bound exists from where the continuation is entered - `findCallBounds`.
        for(U32 k = 0; k < boundRegions.size(); k++) {
            if(body.region(base, ContinuationId(boundRegions[k]))->entry.index == index) {
                BitMatrix::set(live.pointer(), bodyLoans + k);

                // And its previous pass, which arrives there by the back edge - `joinRepeatedPasses`.
                if(staleBounds[k] != kNoStaleBound) BitMatrix::set(live.pointer(), staleBounds[k]);
            }
        }

        if(BitMatrix::replace(minted.row(index), live.pointer(), loanWords)) wakeSuccessors(index);
    });

    OriginCursor cursor { *this };

    SmallArray<U64, 4> entering;
    for(U32 w = 0; w < loanWords; w++) entering.push(0);

    /*
     * Which loans anything **holds** at a block's exit - K8, J219 §4.
     *
     * `minted` says a loan may exist there, which is a fact about paths and not about holders: a
     * local that lent from `b` before a loop and is re-pointed inside it holds the new loan once the
     * assignment has run, but the old one still reaches the latch along the path that skipped the
     * assignment, so `minted` kept it and it was carried round the back edge and up through the very
     * block that had replaced it - where the replacing read of `b` was refused as "already borrowed
     * exclusively". What can carry a loan across an edge is exactly two things, because every value
     * operand names a node of its own block (`verify.cpp`): a local, whose row is the exit state,
     * and a value the edge hands to a block argument. A loan neither holds is dead on the edge
     * whatever the successor goes on to use, since nothing the successor reads can be rooted in it.
     *
     * Only loans the state is kept over are masked. The rest (`kNotStored`) never cross an edge by
     * construction, and a call bound is not held by anything - it is a region's extent.
     *
     * "A local" is a local that is **read again** before it is replaced: the state keeps a row for
     * every local whether or not anything will read it, and the `{x | at: ...}` temporary that built
     * the previous value of `x` still names the old loan long after it is dead. So the rows are
     * filtered by a holder liveness computed here - a use is any operand rooted in the local, a kill
     * is an `Init` or `Assign` of the whole local - which is not `computeLiveness`, because that one
     * runs after this and keeps storage alive *for* the loans this computes. Three kinds of local are
     * taken as live rather than argued: a parameter, everywhere (a `&` one lends back to the caller);
     * a local something borrowed, wherever one of its own loans crosses the edge (what is reached
     * through that loan is its contents); and one replaced by an `Assign` of a value with a teardown,
     * which is a use of the value it releases.
     */
    SmallArray<U64, 4> heldCompact;
    SmallArray<U64, 4> held;
    for(U32 w = 0; w < stateWords; w++) heldCompact.push(0);
    for(U32 w = 0; w < loanWords; w++) held.push(0);

    auto localCount = U32(body.locals.size());
    auto localWords = (localCount + 63) / 64;

    SmallArray<U64, 4> alwaysHeld;
    for(U32 w = 0; w < localWords; w++) alwaysHeld.push(0);

    for(U32 l = 0; l < localCount; l++) {
        if(body.local(base, LocalId(l))->isParameter()) BitMatrix::set(alwaysHeld.pointer(), l);
    }

    // The local each loan was taken of, for the third kind above: a borrowed local's row counts
    // wherever one of its own loans is live across the edge.
    SmallArray<U32, 16> loanRoot;
    for(U32 l = 0; l < mirLoans; l++) {
        auto loan = body.loan(base, LoanId(l));
        auto root = loan->place.valid() ? rootOf(loan->place) : nullptr;
        loanRoot.push(root && root->kind == Place::Local ? root->payload : ~U32(0));
    }

    // The whole local a write replaces, or invalid - the one kill the holder liveness has.
    auto replaced = [&](Node& node) -> U32 {
        if(node.kind != Node::Write) return ~U32(0);

        auto& write = (NodeWrite&)node;
        if(write.destination.source() != SourceKind::Place) return ~U32(0);
        if(write.mode != Destination::Init && write.mode != Destination::Assign) return ~U32(0);

        auto place = body.place(base, write.destination.placeId());
        if(place->parent || place->kind != Place::Local) return ~U32(0);

        auto local = body.local(base, LocalId(place->payload));
        if(write.mode == Destination::Assign &&
           !isTriviallyCopyableAt(*program.decl, local->occurrence)) {
            return ~U32(0);
        }

        return place->payload;
    };

    auto stepHolders = [&](U64* holders, Node& node) {
        auto killed = replaced(node);
        if(killed != ~U32(0)) BitMatrix::clear(holders, killed);

        forEachOperand(base, node, [&](Operand& operand, OperandRole) {
            if(operand.source() != SourceKind::Place) return;
            if(killed != ~U32(0) && &operand == &((NodeWrite&)node).destination) return;

            auto root = rootOf(operand.placeId());
            if(root->kind == Place::Local) BitMatrix::set(holders, root->payload);
        });
    };

    BitMatrix holderOut;
    holderOut.reset(blockCount, localCount);
    {
        BitMatrix holderIn;
        holderIn.reset(blockCount, localCount);

        SmallArray<U64, 4> holders;
        for(U32 w = 0; w < localWords; w++) holders.push(0);

        for(auto changed = true; changed && localCount;) {
            changed = false;

            for(U32 index = blockCount; index-- > 0;) {
                BitMatrix::zero(holders.pointer(), localWords);
                forEachSuccessor(index, [&](U32 target) {
                    BitMatrix::unionInto(holders.pointer(), holderIn.row(target), localWords);
                });

                BitMatrix::replace(holderOut.row(index), holders.pointer(), localWords);

                for(U32 n = nodeCount(index); n > 0; n--) stepHolders(holders.pointer(), nodeAt(index, n - 1));
                if(BitMatrix::replace(holderIn.row(index), holders.pointer(), localWords)) changed = true;
            }
        }
    }

    SmallArray<U64, 4> lent;
    for(U32 w = 0; w < localWords; w++) lent.push(0);

    auto collectHeld = [&](U32 index, Node& last, const U64* crossing) {
        BitMatrix::zero(lent.pointer(), localWords);
        for(U32 l = 0; l < mirLoans; l++) {
            if(loanRoot[l] != ~U32(0) && BitMatrix::test(crossing, l)) BitMatrix::set(lent.pointer(), loanRoot[l]);
        }

        BitMatrix::zero(heldCompact.pointer(), stateWords);
        if(localWidth()) {
            auto state = localsAt(localOut, index);
            auto holding = holderOut.row(index);

            for(U32 l = 0; l < localCount; l++) {
                if(!BitMatrix::test(holding, l) && !BitMatrix::test(alwaysHeld.pointer(), l) &&
                   !BitMatrix::test(lent.pointer(), l)) {
                    continue;
                }

                for(U32 row = localRow[l]; row < localRow[l + 1]; row++) {
                    BitMatrix::unionInto(heldCompact.pointer(), state + Size(row) * stateWords,
                                         stateWords);
                }
            }
        }

        expandInto(heldCompact.pointer(), held.pointer());

        forEachEdge(base, last, [&](Edge& edge) {
            for(auto operand: edge.arguments.contents(base)) {
                if(operand.source() != SourceKind::Result) continue;

                auto node = operand.nodeId();
                for(U32 p = 0; p < valuePositions(node); p++) {
                    BitMatrix::unionInto(held.pointer(), originsOfValue(node, p), loanWords);
                }
            }
        });
    };

    auto maskHeld = [&](U64* loans) {
        for(U32 l = 0; l < loanCount; l++) {
            if(!BitMatrix::test(loans, l) || callBound(l) || stateIndex[l] == kNotStored) continue;
            if(!BitMatrix::test(held.pointer(), l)) BitMatrix::clear(loans, l);
        }
    };

    sweepBackward([&](U32 index) {
        auto block = this->block(index);
        BitMatrix::zero(live.pointer(), loanWords);

        auto count = block->nodes.size();
        if(count) {
            auto last = base[block->nodes.get(base, count - 1)];

            forEachEdge(base, *last, [&](Edge& edge) {
                if(!edge.target) return;

                auto into = base[edge.target]->index.index;
                BitMatrix::copy(entering.pointer(), loansAt(loanIn, into), loanWords);

                /*
                 * A call bound does not exist above the entry of the region it bounds - the minted
                 * pass starts it there - and neither does its previous pass, which arrives by the
                 * back edge this walk does not have. Without the cut a bound live where the region
                 * starts - the root of `for c in chars(text)`, written into `c` at the entry (J129) -
                 * was live at the call, and inside an outer loop the minted pass had brought it round
                 * to there too, so it looked held past the call.
                 */
                for(U32 k = 0; k < boundRegions.size(); k++) {
                    auto region = body.region(base, ContinuationId(boundRegions[k]));
                    if(body.block(base, region->entry)->index.index != into) continue;
                    if(body.encloses(base, region->id, block->region)) continue;

                    BitMatrix::clear(entering.pointer(), bodyLoans + k);
                    if(staleBounds[k] != kNoStaleBound) BitMatrix::clear(entering.pointer(), staleBounds[k]);
                }

                BitMatrix::unionInto(live.pointer(), entering.pointer(), loanWords);
            });

            auto reachable = minted.row(index);
            for(U32 w = 0; w < loanWords; w++) live[w] &= reachable[w];

            collectHeld(index, *last, live.pointer());
            maskHeld(live.pointer());
        }

        // Every traversal whose region this block is inside holds its arguments here - see
        // `regionHeld` above. At the exit, and never killed inside the block, since the loans were
        // taken before the call.
        for(auto r: heldRegions) {
            if(!body.encloses(base, ContinuationId(r), block->region)) continue;

            auto reachable = minted.row(index);
            auto held = regionHeld.row(r);
            for(U32 w = 0; w < loanWords; w++) live[w] |= held[w] & reachable[w];
        }

        BitMatrix::replace(loansAt(loanOut, index), live.pointer(), loanWords);

        // The origin state runs the other way from this walk, so the cursor is seeded at the block's
        // exit and undoes its way up alongside it - see `OriginCursor::undo` for why the undo comes
        // before the node is read rather than after.
        cursor.enterAtExit(index);

        for(U32 n = count; n > 0; n--) {
            cursor.undo(n - 1);
            stepLoansBackward(live.pointer(), cursor, nodeAt(index, n - 1));
        }

        if(BitMatrix::replace(loansAt(loanIn, index), live.pointer(), loanWords)) {
            wakePredecessors(index);
        }
    });
}

void Analysis::stepLoansBackward(U64* live, OriginCursor& cursor, Node& node) {
    // The loan this node took does not exist before it. Killed first, so that a read whose own
    // result is its only use leaves nothing live above it.
    if(node.kind == Node::Read) {
        auto& read = (NodeRead&)node;
        if(read.loan.valid()) BitMatrix::clear(live, read.loan.index);

        /*
         * Below the read a carried loan is its generation, and above it the same holder has it under
         * the loan's own name - `renameGeneration` run backwards. So a previous pass's loan still in
         * use after the read is live above it, around the back edge to the read that took it, where
         * it is killed as any loan is. The generation stays live too: what holds it may have been
         * renamed at an earlier pass already.
         */
        if(read.loan.valid() && generationSource.size()) {
            auto g = generationOf[read.loan.index];
            if(g != kNoGeneration && BitMatrix::test(live, g)) BitMatrix::set(live, read.loan.index);
        }
    }

    /*
     * Every operand, values and places alike - see `forEachOperandOrigin`. A loan bound to a name is
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

        unionOperandOrigins(cursor, operand, live);
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

// No loan was minted by this node - see `loanOfNode`, which is indexed by node and holds a loan
// index. `~U32(0)` rather than `LoanId()`, because the table is a plain array of indices.
constexpr U32 kNoReservation = ~U32(0);

} // namespace

/*
 * The element exemption - `doc/spec/core.md`'s "two elements of one container are the exception,
 * and these two are the only things exempt from it".
 *
 * `swap(xs[i], xs[j])` is two exclusive loans of one container live at once, and on every other
 * operation that is refused whatever the subscripts say: telling `xs[i]` from `xs[j]` would take a
 * proof about two indices, and `Reject.ArrayBorrow.yana` is the record of the refusal. A swap needs
 * no such proof, because it reads both places before it writes either - so the two naming one
 * element restore what they saved - and an exchange has one place to begin with.
 *
 * What is excused is a **pair**, and each half is asked the same question: is this loan an
 * *element's*, and is it used by nothing but a relocation? The walk is over what the loan reaches
 * structurally rather than over the origin environment, because the answer is about every use the
 * loan has anywhere in the body and not about one program point. From the read that minted it:
 *
 *  - a **subscript** call (`NodeCall::subscript`) is the accessor handing the element back, so the
 *    loan is an element's from here on and the call's result carries it;
 *  - a **derivation** call (`NodeCall::derivation`) is `adaptToView` turning the owner into the
 *    window the accessor takes - a path from `self` and no more, Manifestations §5.4 - and its result
 *    carries the loan on;
 *  - an `Init` into a whole local is `Resolver::hold` parking the accessor's result, and the local's
 *    reads carry it on;
 *  - an `address.exchange` is the relocation reaching the storage, and the reads and writes through
 *    it are the relocation itself.
 *
 * Anything else - a function the program called, a return, an aggregate, a plain write - is a use
 * the argument above says nothing about, and the loan is not exempt. A fixed array's element is an
 * `Index` projection the place walk can see, so there the loan is an element's at the read.
 */
bool Analysis::exchangeOnlyLoan(LoanId id) {
    auto& loan = *loanAt(id);
    auto nodes = U32(body.nodes.size());

    auto wholeLocal = [&](Operand operand) -> Maybe<U32> {
        if(operand.source() != SourceKind::Place) return Nothing();
        auto place = body.place(base, operand.placeId());
        if(place->kind != Place::Local || place->parent) return Nothing();
        return Just(place->payload);
    };

    /*
     * Whether the loan is of an element to begin with - the half the walk below cannot see, because
     * it runs forwards from the read and the element may have been reached before it.
     *
     * A fixed array's element is an `Index` step of the place. A window's element is what the
     * accessor handed back, parked in a local by `Resolver::hold`, and the loan the `&` argument
     * takes is a reborrow of *that* local: so the question walks back from the local to the write
     * that parked it, and from there to the subscript call, through however many holds a place
     * expression made on the way.
     */
    auto element = false;
    auto place = loan.place;

    for(U32 depth = 0; depth < 16 && !element; depth++) {
        Place* root = nullptr;
        for(auto p = body.place(base, place); p; p = p->parent ? base[p->parent] : nullptr) {
            if(p->kind == Place::Index) element = true;
            root = p;
        }

        if(element || root->kind != Place::Local) break;

        Node* parked = nullptr;
        for(U32 i = 0; i < nodes && !parked; i++) {
            auto node = body.node(base, NodeId(i));
            if(node->kind != Node::Write) continue;

            auto& write = *(NodeWrite*)node;
            if(write.mode != Destination::Init) continue;

            auto local = wholeLocal(write.destination);
            if(!local || local.unwrap() != root->payload) continue;
            if(write.value.source() != SourceKind::Result) break;

            parked = body.node(base, NodeId(write.value.id()));
        }

        if(!parked) break;

        if(parked->kind == Node::Call && ((NodeCall*)parked)->subscript) {
            element = true;
        } else if(parked->kind == Node::Read && ((NodeRead*)parked)->place.source() == SourceKind::Place) {
            place = ((NodeRead*)parked)->place.placeId();
        } else {
            break;
        }
    }

    // What is being followed: a value the loan is carried by, a local it was parked in, and an
    // address a relocation reaches it through. Three lists rather than one tagged list, because
    // what a use of each may be differs.
    SmallArray<U32, 8> values, locals, addresses;

    for(U32 i = 0; i < nodes; i++) {
        auto node = body.node(base, NodeId(i));
        if(node->kind != Node::Read) continue;

        auto& read = *(NodeRead*)node;
        if(read.loan.valid() && read.loan.index == id.index) values.push(i);
    }

    if(values.size() == 0) return false;

    auto rootIs = [&](Operand operand, Place::Kind kind, U32 payload) {
        if(operand.source() != SourceKind::Place) return false;
        auto root = rootOf(operand.placeId());
        return root->kind == kind && root->payload == payload;
    };

    auto ok = true;
    Size v = 0, l = 0, a = 0;

    while(ok && (v < values.size() || l < locals.size() || a < addresses.size())) {
        if(v < values.size()) {
            auto value = values[v++];

            for(U32 i = 0; i < nodes && ok; i++) {
                auto& node = *body.node(base, NodeId(i));

                forEachOperand(base, node, [&](Operand& operand, OperandRole role) {
                    if(!ok || operand.source() != SourceKind::Result || operand.id() != value) return;

                    switch(node.kind) {
                        case Node::Call: {
                            auto& call = (NodeCall&)node;
                            if(role != OperandRole::Argument || !(call.subscript || call.derivation)) {
                                ok = false;
                                return;
                            }

                            if(call.subscript) element = true;
                            values.push(i);
                            return;
                        }
                        case Node::Write: {
                            auto& write = (NodeWrite&)node;
                            auto local = wholeLocal(write.destination);

                            if(role != OperandRole::Stored || write.mode != Destination::Init || !local) {
                                ok = false;
                                return;
                            }

                            locals.push(local.unwrap());
                            return;
                        }
                        case Node::Address:
                            if(!((NodeAddress&)node).exchange) { ok = false; return; }
                            addresses.push(i);
                            return;
                        default:
                            ok = false;
                            return;
                    }
                });
            }
        } else if(l < locals.size()) {
            auto local = locals[l++];

            for(U32 i = 0; i < nodes && ok; i++) {
                auto& node = *body.node(base, NodeId(i));

                forEachOperand(base, node, [&](Operand& operand, OperandRole) {
                    if(!ok || !rootIs(operand, Place::Local, local)) return;

                    // The parking write itself names the local too; every other write is a use.
                    if(node.kind == Node::Write && ((NodeWrite&)node).mode == Destination::Init &&
                       wholeLocal(((NodeWrite&)node).destination)) {
                        return;
                    }

                    if(node.kind != Node::Read) { ok = false; return; }
                    values.push(i);
                });
            }
        } else {
            auto address = addresses[a++];

            for(U32 i = 0; i < nodes && ok; i++) {
                auto& node = *body.node(base, NodeId(i));

                forEachOperand(base, node, [&](Operand& operand, OperandRole) {
                    if(!ok) return;

                    // The pointer as a value anywhere is a use the relocation did not make.
                    if(operand.source() == SourceKind::Result && operand.id() == address) {
                        ok = false;
                        return;
                    }

                    if(!rootIs(operand, Place::Address, address)) return;
                    if(node.kind != Node::Read && node.kind != Node::Write) ok = false;
                });
            }
        }
    }
    return ok && element;
}

// Whether a place is what a `swap` or `exchange` reaches its storage through - rooted at an
// `address.exchange`.
bool Analysis::exchangeAddressRoot(PlaceId place) {
    auto root = rootOf(place);
    if(root->kind != Place::Address) return false;

    auto address = body.node(base, NodeId(root->payload));
    return address->kind == Node::Address && ((NodeAddress*)address)->exchange;
}

// Whether a write is a `swap` or `exchange` relocating through the storage.
bool Analysis::exchangeAccess(Node& node) {
    if(node.kind != Node::Write) return false;

    auto& write = (NodeWrite&)node;
    return write.destination.source() == SourceKind::Place && exchangeAddressRoot(write.destination.placeId());
}

// The overlap and conformance checks - §6.4, over settled extents.
/*
 * A place as a diagnostic names it - `placeText`, except for storage behind a raw address, which the
 * dump spells `[%%36]` after the node that made the pointer. A message cannot say that: the author
 * wrote `borrow(q)`. So an address read out of storage is spelled through the place it was read from
 * - "what q points at" - the same chain `unionPlaceOrigins` follows, and a projection below it as
 * "part of" that. A pointer computed rather than read has no place to name.
 */
String Analysis::accessText(PlaceId place) {
    auto root = rootOf(place);
    if(root->kind != Place::Address) return placeText(context, decl, base, body, *body.place(base, place));

    auto whole = body.place(base, place) == root;
    auto node = body.node(base, NodeId(root->payload));

    if(node->kind == Node::Read && ((NodeRead*)node)->place.source() == SourceKind::Place) {
        auto held = accessText(((NodeRead*)node)->place.placeId());
        if(whole) return Tritium::format("what %@ points at", held);
        return Tritium::format("part of what %@ points at", held);
    }

    return String(whole ? "what the pointer here points at" : "part of what the pointer here points at");
}

bool Analysis::checkLoans() {
    if(!loanCount) return errors == 0;

    /*
     * The scratch rows the check below works in - and, deliberately, no table of loans.
     *
     * This walk used to build, at every revision of the origin state, an `L x L` **closure**: which
     * loans each loan's place reaches through the environment, so that `d` where `let d = c`
     * reaches `c`. It then asked two questions of it per access - the union of the rows of every
     * live loan, and a scan of every loan for membership - and both are proportional to the loan
     * count. A body's loan count grows with the body, since every argument handed to a borrowing
     * parameter mints one, so an access cost the whole body and the walk was cubic in it: shape C of
     * `test/bench/ownership-scaling.py` took 1.3 s at 320 x 16 and 10.4 s at 640 x 16, three
     * quarters of it here, and the table itself was `L^2 / 8` bytes rebuilt at every revision.
     *
     * The two questions are asked the other way round now and neither reads a table of loans:
     *
     *  - **forward** - what one place reaches: its own origins, then theirs, until nothing new
     *    arrives. A closure over a handful of loans, taken on demand by `reachFrom`, because the set
     *    a place is rooted in is small however many loans the body holds.
     *  - **backward** - which loans reach an overlapping one: the loans whose place is rooted in `m`
     *    are the loans on the locals whose state holds `m`, and `holdersInto` reads that off the
     *    origin state itself. A scan of the state's positions for one bit is a scan of the locals,
     *    not of the loans.
     *
     * And which loans can overlap a place at all is indexed once, by the root the loan's place is
     * on (`loansByRoot`): two places on different roots are disjoint, so the loans on any other root
     * are never asked. What is left per access is proportional to what the access touches.
     */
    SmallArray<U64, 4> scratch, reachedSet, overlapping, conflict, frontier, probe, skipSet;
    SmallArray<U64, 4> exclusiveConflict, through;
    for(U32 w = 0; w < loanWords; w++) {
        scratch.push(0); reachedSet.push(0); overlapping.push(0); conflict.push(0);
        frontier.push(0); probe.push(0); skipSet.push(0); exclusiveConflict.push(0);
        through.push(0);
    }

    /*
     * The loans whose taking was refused, so that what they produced is not refused again.
     *
     * `both(xs[0], xs[1])` on a window is two `getMut(&xs, ..)` calls, and the second's borrow of
     * `xs` conflicts with the first's while its result is alive - one mistake, and the report at the
     * second call names it. The value that call returns is rooted in the refused loan, and every
     * access through it - the reborrow that hands it to `both` - overlaps the same live loan for the
     * same reason, so without this the author read the mistake twice, the second time against a
     * temporary with no name. A refused loan is a loan the program does not have.
     */
    SmallArray<U64, 4> refused;
    for(U32 w = 0; w < loanWords; w++) refused.push(0);

    /*
     * The loans on each root, and the ones whose root may be anywhere.
     *
     * A place's root is a local or a global this body names, a raw address, or a loan value; the
     * first two are disjoint from every other root, and the last two overlap everything
     * (`placesOverlap`). So the loans that can overlap a place are the ones on its root plus the
     * ones on a root that may be anywhere, and the index is by root so that an access asks about
     * those and no others. Keyed by kind and payload, which together name the root.
     */
    auto rootKey = [](Place::Kind kind, U32 payload) { return (U64(kind) << 32) | payload; };

    HashMap<U64, SmallArray<U32, 2>> loansByRoot;
    SmallArray<U32, 8> anyRootLoans;

    for(U32 l = 0; l < bodyLoans; l++) {
        auto root = rootOf(loanAt(U32(l))->place);
        if(root->kind == Place::Address || root->kind == Place::Loan) anyRootLoans.push(l);
        else loansByRoot[rootKey(root->kind, root->payload)].push(l);
    }

    /*
     * And which storage those loans may actually be pointing at - every place but this frame's own
     * private locals.
     *
     * "May be anywhere" is a statement about the storage a *pointer* can name, and there is one
     * kind of storage no pointer in this body can name: a local of this frame whose address was
     * never taken. Its bytes came into existence when the frame did, and `Address` is the only
     * producer of a pointer this stage has - so what a pointer here can be pointing at is what some
     * `addressOf` in this body reached, and a local no `addressOf` reached was never pointed at by
     * anybody, a recursive call of this very function included.
     *
     * **Reached**, not written at. `addressOf` reshapes a *borrow* and the borrow may be a reborrow,
     * so the storage an address names is every loan the value handed to it is rooted in:
     * `addressOf(me.slot)` under `let &me = borrowMut(p)` names `me`'s storage and what `p` points
     * at alike, and the origin environment already relates the two. `originsOfValue` is that answer,
     * which is why this needs no walk of its own beyond finding the `addressOf`es.
     *
     * A borrow handed to a *call* is deliberately not an address, however the callee receives it: a
     * borrowed occurrence is call-bounded, so a callee may read through it and may not keep it, and
     * a pointer this frame was handed before the borrow existed cannot be pointing at it either.
     * What escapes a frame is what `addressOf` made, which is the one thing carrying no lifetime.
     *
     * A **borrowed parameter** is addressable whether or not this body took its address, because
     * the storage behind one is the caller's: `&self: Fiber` names a place the caller already holds,
     * and a pointer the caller handed over beside it may be pointing straight at it. An **owned**
     * parameter is this frame's like any other local - the caller gave the value up, and a call that
     * both moved a value out of storage and lent a pointer to that same storage is one this check
     * refuses at the call site.
     *
     * Without this, a live borrow through a raw pointer made every writable local in the body
     * unwritable - a `for` loop's induction variable included, since `anyRootLoans` is added to
     * every access's conflict set:
     *
     *     fn bump(self: %Counter):
     *         let &me = borrowMut(self)
     *         for i in 0..me.limit:      -- `i` is not storage `self` could be pointing at
     *             me.other += i
     *
     * which is `doc/inflight/Analysis-External-Iterators.md`'s substrate written twice over, since
     * every operation a fiber context performs is a borrow through the address it was handed.
     */
    Array<bool> addressable;
    for(U32 i = 0; i < body.locals.size(); i++) {
        auto local = body.local(base, LocalId(i));
        addressable.push(local->isParameter() && !local->occurrence.contract.isOwned());
    }

    SmallArray<U64, 4> addressed;
    for(U32 w = 0; w < loanWords; w++) addressed.push(0);

    for(U32 index = 0; index < blockCount; index++) {
        auto block = this->block(index);

        for(U32 at = 0; at < block->nodes.size(); at++) {
            auto& node = nodeAt(index, at);
            if(node.kind != Node::Address) continue;

            auto& taken = (NodeAddress&)node;

            // A place handed straight over - no borrow was minted, so the root is the whole answer.
            if(taken.of.source() == SourceKind::Place) {
                auto root = rootOf(taken.of.placeId());
                if(root->kind == Place::Local) addressable[root->payload] = true;
                continue;
            }

            if(taken.of.source() != SourceKind::Result) continue;

            auto value = taken.of.nodeId();
            auto positions = valuePositions(value);

            for(U32 p = 0; p < positions; p++) {
                BitMatrix::unionInto(addressed.pointer(), originsOfValue(value, p), loanWords);
            }
        }
    }

    BitMatrix::forEach(addressed.pointer(), loanWords, [&](Size m) {
        if(callBound(U32(m))) return;

        auto root = rootOf(loanAt(U32(m))->place);
        if(root->kind == Place::Local) addressable[root->payload] = true;
    });

    // Whether a raw address may be pointing into the storage this place is rooted in - the question
    // above, asked of a root rather than of a local, since a global's address is anybody's to take.
    auto rootIsAddressable = [&](Place* root) {
        if(root->kind != Place::Local) return true;
        return addressable[root->payload];
    };

    // Which local each origin position belongs to - `localRow` inverted, for the backward walk.
    Array<U32> positionLocal;
    for(U32 local = 0; local + 1 < localRow.size(); local++) {
        for(U32 at = localRow[local]; at < localRow[local + 1]; at++) positionLocal.push(local);
    }

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
    /*
     * `exclusiveOnly` is the same walk through **unique** loans alone, which is what an exclusive
     * access or a write reaches *with its own capability*. A shared loan the storage holds hands
     * out shared access to its place and nothing more - `data Store {held: kept'Cell}` accessed
     * uniquely reaches the cell for reading - so a place behind one is not a place the access can
     * write, and a shared loan of it does not exclude the access (`check`, below).
     */
    auto reachFrom = [&](OriginCursor& cursor, PlaceId place, SmallArray<PlaceId, 8>& into,
                         bool exclusiveOnly = false) {
        into.clear();

        auto keepExclusive = [&](U64* set) {
            if(!exclusiveOnly) return;
            for(U32 w = 0; w < loanWords; w++) set[w] &= uniqueLoans[w];
        };

        /*
         * A raw address may point anywhere, so `placesOverlap` says it overlaps everything - except
         * the one address whose target is known: a `swap` or `exchange` reaches its place through
         * `address.exchange`, and what that names is exactly the loan it was taken from, whose
         * place the loop below adds. Listing the address as well would make every relocation
         * conflict with every unrelated loan live across it.
         */
        if(!exchangeAddressRoot(place)) into.push(place);

        // The storage behind it, and what an address says it points into - both are
        // `unionPlaceOrigins`, which is where that sentence is argued.
        BitMatrix::zero(reachedSet.pointer(), loanWords);
        unionPlaceOrigins(cursor, place, reachedSet.pointer());
        keepExclusive(reachedSet.pointer());

        // Then what those loans' places are rooted in, until nothing new arrives - one round per
        // level of reborrowing, and a body has one or two. Over a frontier rather than over the
        // whole set, so that a round costs the loans it added and not the loans it has.
        BitMatrix::copy(frontier.pointer(), reachedSet.pointer(), loanWords);

        while(BitMatrix::any(frontier.pointer(), loanWords)) {
            BitMatrix::zero(scratch.pointer(), loanWords);
            BitMatrix::forEach(frontier.pointer(), loanWords, [&](Size m) {
                if(callBound(U32(m))) return;
                unionPlaceOrigins(cursor, loanAt(U32(m))->place, scratch.pointer());
            });

            keepExclusive(scratch.pointer());
            for(U32 w = 0; w < loanWords; w++) scratch[w] &= ~reachedSet[w];
            BitMatrix::unionInto(reachedSet.pointer(), scratch.pointer(), loanWords);
            BitMatrix::copy(frontier.pointer(), scratch.pointer(), loanWords);
        }

        // A call bound has no place: nothing in this body can reach the frame it stands for.
        BitMatrix::forEach(reachedSet.pointer(), loanWords, [&](Size m) {
            if(!callBound(U32(m))) into.push(loanAt(U32(m))->place);
        });
    };

    /*
     * Every loan rooted, transitively, in one of `seeds` - added to `into`.
     *
     * The backward half of the same closure: a loan reaches `m` when its place's origins hold `m`,
     * and a place's origins are its root local's state at the positions the place selects. So the
     * loans that reach `m` are found among the loans on the locals whose state holds `m` - a scan
     * of the positions for one bit - and then the loans that reach *those*, until nothing new
     * arrives. A loan on the bare local is confirmed by the position alone, since the bare place
     * selects every position; one on a projection of it is asked, since a field of the local may
     * hold what another field does not, and answering for it from the whole local would report a
     * conflict between two disjoint fields.
     */
    auto holdersInto = [&](OriginCursor& cursor, const U64* seeds, U64* into) {
        BitMatrix::copy(frontier.pointer(), seeds, loanWords);

        while(BitMatrix::any(frontier.pointer(), loanWords)) {
            BitMatrix::zero(scratch.pointer(), loanWords);

            BitMatrix::forEach(frontier.pointer(), loanWords, [&](Size m) {
                // A loan the state is not kept over is in no local, so nothing is rooted in it.
                auto slot = stateIndex[m];
                if(slot == kNotStored) return;

                for(U32 at = 0; at < originRowCount; at++) {
                    if(!BitMatrix::test(cursor.state() + Size(at) * stateWords, slot)) continue;

                    auto held = loansByRoot.get(rootKey(Place::Local, positionLocal[at]));
                    if(!held) continue;

                    for(auto l: held.unwrap()) {
                        if(BitMatrix::test(into, l) || BitMatrix::test(scratch.pointer(), l)) continue;

                        auto& loan = *loanAt(U32(l));
                        if(body.place(base, loan.place)->parent) {
                            BitMatrix::zero(probe.pointer(), loanWords);
                            unionPlaceOrigins(cursor, loan.place, probe.pointer());
                            if(!BitMatrix::test(probe.pointer(), m)) continue;
                        }

                        BitMatrix::set(scratch.pointer(), l);
                    }
                }
            });

            for(U32 w = 0; w < loanWords; w++) scratch[w] &= ~into[w];
            BitMatrix::unionInto(into, scratch.pointer(), loanWords);
            BitMatrix::copy(frontier.pointer(), scratch.pointer(), loanWords);
        }
    };

    SmallArray<PlaceId, 8> reached;

    /*
     * What an access reborrows, as one set - the `skip` argument below.
     *
     * A set rather than a row pointer, because an origin set is per position now and what an access
     * reborrows is everything the value or the storage may reach, whichever part of it holds them.
     * Over-collecting here would *silence* a conflict, so it is deliberately the union: the two
     * callers are a borrow whose own result already carries what it reborrows, and a write or a move
     * whose destination carries what is being displaced.
     */
    auto skipOfPlace = [&](OriginCursor& cursor, PlaceId place) {
        BitMatrix::zero(skipSet.pointer(), loanWords);
        unionPlaceOrigins(cursor, place, skipSet.pointer());
        return (const U64*)skipSet.pointer();
    };

    auto skipOfValue = [&](NodeId node) {
        BitMatrix::zero(skipSet.pointer(), loanWords);

        auto positions = valuePositions(node);
        for(U32 p = 0; p < positions; p++) {
            BitMatrix::unionInto(skipSet.pointer(), originsOfValue(node, p), loanWords);
        }

        return (const U64*)skipSet.pointer();
    };

    /*
     * One access, against every loan that is still live after it.
     *
     * A loan that dies at this node does not extend past it, so `live` is the set *after* the node -
     * which is also what makes a loan created here not conflict with itself. `skip` is what the
     * access reborrows: writing through a `&` binding is what the binding is for, and a reborrow of
     * a loan is not a conflict with the loan it reborrows (§2.5).
     *
     * The overlap is decided in two halves, and that is what keeps the comparison out of the loop.
     * `overlapping` is "whose own place does this access touch", asked of the loans on the roots
     * the access reaches; and a loan conflicts when it is in that set or when anything it reaches
     * is - which `holdersInto` answers backward from the origin state, so that the loop below runs
     * over the conflicts alone rather than over every live loan.
     */
    /*
     * The unique loans that have been **taken and not yet used** - two-phase borrowing, and the
     * three `lib/Core` lines that need it.
     *
     * `pushFill(to, value.fill, value.width - (length(to) - start))` reads `to` in an argument
     * evaluated *after* the one that borrowed it exclusively, and the same shape is
     * `remove(self.entries, entryCount(self) - 1)` and `indexPlace(self, at, hashAt(self, at))`.
     * `doc/spec/expressions.md` evaluates arguments left to right and the callee runs after all of
     * them, so nothing writes through the exclusive borrow while the later arguments are being
     * computed: what the author wrote is safe, and all three are accepted.
     *
     * So a unique loan is only a **reservation** until something uses it. Between the read that
     * minted it and the first operand naming that read's result it excludes a write and a move -
     * the two that would leave it pointing at something else - and it excludes **a loan only where
     * that loan is still live where the reservation activates**. The activation point is then
     * checked as the take itself would have been, against the loans live there, so nothing is lost
     * by admitting the ones that end first: a loan taken and released inside a later argument is
     * gone by the call, and one that survives is refused there.
     *
     * That last clause is what `f(&x, g(&x))` needed - `readColor(options, takeValue(options, ...))`
     * in `lib/Test/Options.yana`, eight times. The nested call's own exclusive loan is dead the
     * moment it returns, so it can no more observe the reservation than a shared read can.
     *
     * **The liveness is asked here rather than at the activation**, and that is what keeps the blame
     * where an author can act on it: `twice(b, b)` and `both(g, g)` are refused at the second
     * argument, which is the one to change, and moving them to the call would have said "these two
     * conflict" about a line naming neither. A *shared* take is still excused outright and reported
     * at the activation if it survives - `holds(g, peek(g))` in `Reject.TwoPhase.yana` - because a
     * read cannot be the mistake: what is wrong there is handing the borrow on, which is the call.
     *
     * **Carried across blocks as a must-fact** (J141): a block starts with what every predecessor
     * left reserved, so the arms of a later argument that branches - `f(xs, if c then length(xs)
     * else 0)` - still see the reservation, and the activation in the join block is checked as it
     * would be in one. Until J141 it was per block and cleared at every entry, which refused that
     * read. **Past a branch the block's exit is the activation** (J140): a take meeting a
     * reservation whose activation is not in this block is judged against the block's live-out.
     * Before J140 the shared read was excused outright there and the activation never checked it -
     * `holds3(g, peek(g), if c then 1 else 2)` compiled, and the callee wrote through `g` while
     * reading `peek`'s borrow of the same field (`Reject.TwoPhase.yana`). A loan dead at the block's
     * exit is dead at the activation too, so that is where such a take is judged; one live there is
     * refused where it is taken, which keeps the blame on the argument rather than the call.
     *
     * **A window is a reservation too** (J140). `setAt(xs, 0, length(xs))` over `&xs: [I32]` reads
     * `xs` and then calls the derivation `adaptToView` placed in front of the call to turn the owner
     * into the window the parameter takes - and the derivation was the first operand naming the
     * read, so the reservation activated there, one node after it was made, and `length(xs)` met an
     * active loan. `Recursive.Borrow.yana` and `Words.yana` were that line. A derivation is a path
     * from `self` and nothing else (Manifestations §5.4, `NodeCall::derivation`), so it is not a use:
     * its result carries the reservation on and the call naming *it* is the activation.
     *
     * What the derivation does compute is the window itself - where the elements are and how many -
     * and that is what the plain reservation's one concession cannot survive. A second exclusive
     * loan that ends before the call (`readColor(options, takeValue(options, ...))`) is harmless
     * against the address of `options`, which no write through a borrow can move; against a window
     * already read out of `xs` it is `push` reallocating the run the window points into. So a
     * **windowed** reservation excludes every exclusive loan outright, and only a shared read is
     * excused - `setAt(xs, 0, pushLen(xs))` in `Reject.TwoPhase.yana`.
     */
    Array<U32> loanOfNode, readOfNode;
    loanOfNode.reserve(U32(body.nodes.size()));
    readOfNode.reserve(U32(body.nodes.size()));
    for(Size i = 0; i < body.nodes.size(); i++) {
        loanOfNode.push(kNoReservation);
        readOfNode.push(kNoReservation);
    }

    SmallArray<U64, 4> windowed;
    for(U32 w = 0; w < loanWords; w++) windowed.push(0);

    /*
     * The unique loans a **closure environment** holds - a `&` capture (J140).
     *
     * `doc/spec/functions.md`, Closures: a by-reference capture is a mutable borrow that stays alive
     * as long as the closure does, and while it is alive the enclosing frame may not touch the
     * binding. The loan was always there - the environment's aggregate names it and the origin
     * analysis keeps it live for as long as the closure value is - and it already excluded a borrow
     * or a write. What it did not exclude is the **copy**: `AccessKind::Copy` conflicts with nothing,
     * because reading a `TrivialCopy` value out *through* a live `&` is legal (ownership.md), and a
     * capture is nearly always an `Int` counter. That exemption is about the borrow's holder reading
     * through it, and the frame reading `count` is not reading through `bump` - it is the other side
     * of a borrow whose holder runs later and writes, so `let seen = count` before `bump()` would read
     * a value the closure is about to change under it. So a copy is checked against these loans and
     * no others: `Reject.Closure.yana`'s `borrowedWhileCaptured`.
     *
     * **J141 made the rule general and left this row as the sentence it picks.** A closure is one
     * holder of a unique loan, and a record field written `&'Int` is another: `let &r = Ref {p:
     * count}`, then `let seen = count`, then `r.p = r.p + 10` read `count` while `r` held it
     * exclusively and was about to write it. So a copy now meets every live unique loan of the
     * storage it names *directly* - see `uniqueLoans` - and this row only decides whether the report
     * says "a closure captured" or "is borrowed exclusively".
     */
    SmallArray<U64, 4> captured;
    for(U32 w = 0; w < loanWords; w++) captured.push(0);

    /*
     * Every unique loan of the body - the loans a **copy** can meet (J141).
     *
     * A copy meets a unique loan only of the storage it *names*: a place whose root is this frame's
     * local or a global, overlapping the loan's place. Reading through a reference or a pointer - a
     * place rooted in a loan or an address, or `r.p` below a `Deref` of the field holding it - is
     * reading through the borrow, which ownership.md makes legal for a `TrivialCopy` value, so it is
     * the lender's own name that is excluded and not the holder's path to it. A reservation excuses
     * a copy outright: a copied value is gone before the activation, so it can observe nothing the
     * callee does (`bump(r, r.p)`).
     */
    SmallArray<U64, 4> uniqueLoans;
    for(U32 w = 0; w < loanWords; w++) uniqueLoans.push(0);
    for(U32 l = 0; l < bodyLoans; l++) {
        if(loanAt(l)->capability == Access::Unique) BitMatrix::set(uniqueLoans.pointer(), l);
    }

    /*
     * A derivation carries the reservation of the read it adapts, and is not its activation - and
     * so does a **subscript** (J147). `addTo(xs[0], total(xs))` reads `xs` exclusively, and
     * `call.subscript getMut` of that read was the first operand naming it, so the reservation
     * activated at the subscript and `total(xs)` met an active loan, refusing a line that is safe.
     * A subscript is a path from `self` in exactly the derivation's sense - it computes where the
     * element is and hands back a loan of it - so its result carries the reservation on to the
     * call, **windowed**: the element's address is as much a snapshot of the run as a window's is,
     * so a later shared read is excused and a later exclusive loan (`addTo(xs[1], pushLen(xs))`, a
     * `push` that can move the run) stays refused. Only `self`: an index is an ordinary operand.
     */
    auto derivesFromSelf = [&](Node& node) {
        return node.kind == Node::Call &&
               (((NodeCall&)node).derivation || ((NodeCall&)node).subscript);
    };

    /*
     * A place argument (`&a: Int`) reaches the call through a **temporary** the subscript's loan is
     * written into and read back out of - `write.init (local2) borrow.unique %sub`, then
     * `read borrow.unique (local2) loan1` - so the write would be the first operand naming the
     * subscript and activate the reservation there. The write is a forwarding node too, and the
     * read's own loan is **chained** to the reservation: where `loan1` activates, so does what it
     * was filled from. `chainOf` is by loan, and `forwarding` marks the write.
     */
    Array<U32> chainOf;
    IndexSet forwarding, chainedLocal;
    Array<U32> chainedFrom;

    auto forwardsReservation = [&](Node& node) {
        if(forwarding.size() && forwarding.get(node.id.index)) return true;
        return derivesFromSelf(node) && loanOfNode[node.id.index] != kNoReservation;
    };

    for(U32 index = 0; index < blockCount; index++) {
        auto block = this->block(index);
        for(U32 position = 0; position < block->nodes.size(); position++) {
            auto& node = nodeAt(index, position);

            if(derivesFromSelf(node)) {
                auto& call = (NodeCall&)node;
                if(call.arguments.size() == 0 || (call.derivation && call.arguments.size() != 1)) continue;

                auto self = call.arguments.get(base, 0);
                if(self.source() != SourceKind::Result) continue;

                auto minted = loanOfNode[self.nodeId().index];
                if(minted == kNoReservation) continue;

                // Marked whatever the loan's capability: only a unique loan is ever reserved, so
                // the mark on a shared one is read by nothing.
                loanOfNode[node.id.index] = minted;
                readOfNode[node.id.index] = readOfNode[self.nodeId().index];
                BitMatrix::set(windowed.pointer(), minted);
                continue;
            }

            if(node.kind == Node::Aggregate) {
                auto type = ((NodeAggregate&)node).occurrence.type;
                if(!type || decl[type]->kind != Type::Record || !((RecordType*)decl[type])->lambda) continue;

                auto& components = ((NodeAggregate&)node).components;
                for(U32 i = 0; i < components.size(); i++) {
                    auto component = components.get(base, i);
                    if(component.source() != SourceKind::Result) continue;

                    auto minted = loanOfNode[component.nodeId().index];
                    if(minted != kNoReservation && loanAt(minted)->capability == Access::Unique) {
                        BitMatrix::set(captured.pointer(), minted);
                    }
                }

                continue;
            }

            if(node.kind == Node::Write) {
                auto& write = (NodeWrite&)node;
                if(write.mode != Destination::Init || write.value.source() != SourceKind::Result ||
                   write.destination.source() != SourceKind::Place) {
                    continue;
                }

                auto from = body.node(base, write.value.nodeId());
                auto minted = loanOfNode[write.value.nodeId().index];
                if(minted == kNoReservation || !derivesFromSelf(*from)) continue;

                auto root = rootOf(write.destination.placeId());
                if(root->kind != Place::Local || body.place(base, write.destination.placeId()) != root) continue;
                if(body.local(base, LocalId(root->payload))->name) continue;

                if(!forwarding.size()) {
                    forwarding.reset(U32(body.nodes.size()));
                    chainedLocal.reset(U32(body.locals.size()));
                    for(Size i = 0; i < body.locals.size(); i++) chainedFrom.push(kNoReservation);
                    for(U32 l = 0; l < loanCount; l++) chainOf.push(kNoReservation);
                }

                forwarding.add(node.id.index);
                chainedLocal.add(root->payload);
                chainedFrom[root->payload] = minted;
                continue;
            }

            if(node.kind != Node::Read) continue;

            auto& read = (NodeRead&)node;
            if(read.loan.valid()) {
                loanOfNode[node.id.index] = read.loan.index;
                readOfNode[node.id.index] = node.id.index;

                // The temporary's read-back - see `chainOf`.
                if(chainedLocal.size() && read.place.source() == SourceKind::Place) {
                    auto root = rootOf(read.place.placeId());
                    if(root->kind == Place::Local && chainedLocal.get(root->payload) &&
                       body.place(base, read.place.placeId()) == root) {
                        chainOf[read.loan.index] = chainedFrom[root->payload];
                    }
                }
            }
        }
    }

    // A reservation's chained predecessor, or `kNoReservation`.
    auto chained = [&](U32 minted) {
        return chainOf.size() && minted < chainOf.size() ? chainOf[minted] : kNoReservation;
    };

    SmallArray<U64, 4> reserved;
    for(U32 w = 0; w < loanWords; w++) reserved.push(0);

    // What each block leaves reserved at its exit, allocated the first time one does - J141.
    Array<U64> reservedOut;
    auto reservationsOut = false;

    /*
     * Where each reservation of the block being walked **activates**, as a position in it.
     *
     * The loans live before that node are what a reservation is judged against, so a take made
     * while it is reserved has to ask the same question one node early. `kNoReservation` is a
     * reservation nothing in this block uses, and it keeps the answer it had before two-phase:
     * a reservation whose activation is not here is treated as already active.
     */
    Array<U32> activationOf;
    for(U32 l = 0; l < loanCount; l++) activationOf.push(kNoReservation);

    // The loans live after each node, and the loans live *before* it - declared here because the
    // take-side question above reads the second of them at another node's position. `blockExit` is
    // the block's live-out, which stands in for an activation past it.
    BitMatrix after, before;
    const U64* blockExit = nullptr;

    /*
     * The live loans one access conflicts with, given the places it reaches - into `out`. A lambda
     * of its own because `check` asks it twice for an exclusive access: once of everything the
     * access reaches, and once of what it reaches through unique loans alone.
     */
    auto conflictsOf = [&](OriginCursor& cursor, const SmallArray<PlaceId, 8>& from, const U64* live,
                           U64* out) {
        /*
         * The loans whose place overlaps one this access reaches - asked of the loans on the roots
         * it reaches and of no others. A root that may be anywhere overlaps every loan, so every
         * live loan conflicts and there is nothing to compute.
         */
        BitMatrix::zero(overlapping.pointer(), loanWords);
        auto anywhere = false;

        for(auto x: from) {
            auto root = rootOf(x);

            if(root->kind == Place::Loan) {
                anywhere = true;
                break;
            }

            /*
             * An address is anywhere only where its **type** says nothing.
             *
             * `%source'a` says the address may point into `source`'s storage, and that is a
             * statement about which storage - not about whether anyone else writes through it, so
             * two pointers into one source still conflict and `doc/spec/native.md`'s "proves no
             * exclusivity" is untouched. What the qualifier removes is the false positive with no
             * remedy: `setVectorAt(out.units.run.items + i, ...)` writing through a buffer of this
             * frame's own conflicted with every live borrow in the body, and no spelling an author
             * could reach would have said otherwise.
             *
             * The loans it may reach were unioned into `reachedSet` by `reachFrom`, so a qualified
             * address is checked against exactly those and an unqualified `%a` still overlaps
             * everything.
             */
            if(root->kind == Place::Address) {
                if(valuePositions(NodeId(root->payload)) >= 2) continue;

                anywhere = true;
                break;
            }

            if(auto held = loansByRoot.get(rootKey(root->kind, root->payload))) {
                for(auto l: held.unwrap()) {
                    if(placesOverlap(base, decl, body, x, loanAt(U32(l))->place)) {
                        BitMatrix::set(overlapping.pointer(), l);
                    }
                }
            }
        }

        // And the loans whose own root may be anywhere overlap whatever the access reaches - as
        // far as an address reaches it at all, which is `rootIsAddressable`.
        for(auto x: from) {
            if(!rootIsAddressable(rootOf(x))) continue;

            for(auto l: anyRootLoans) BitMatrix::set(overlapping.pointer(), l);
            break;
        }

        // A live loan conflicts where it overlaps directly or reaches a loan that does - the
        // backward walk, restricted to what is live after the node.
        if(anywhere) {
            BitMatrix::copy(out, live, loanWords);
            for(auto l = bodyLoans; l < loanCount; l++) BitMatrix::clear(out, l);
        } else {
            BitMatrix::copy(out, overlapping.pointer(), loanWords);
            holdersInto(cursor, overlapping.pointer(), out);
            for(U32 w = 0; w < loanWords; w++) out[w] &= live[w];
        }
    };

    /*
     * Whether a live unique loan of `holder` can reach, with its own capability, any place the
     * access being checked reaches - `reached`, as `check` left it. The unique-only walk of
     * `reachFrom`; a root that may be anywhere is taken to overlap, which is the safe direction.
     * Every address counts as anywhere here, qualified or not - unlike `conflictsOf`, which exempts
     * a `%source'a` because the loans it may reach are already in `reachedSet`, this has no such set
     * to check a qualified address against. Clobbers `reachedSet`, so `check` asks it only after its
     * last use of that.
     */
    SmallArray<PlaceId, 8> holderReach;
    auto uniqueReachOverlaps = [&](OriginCursor& cursor, PlaceId holder) {
        reachFrom(cursor, holder, holderReach, true);

        auto anywhere = [&](PlaceId x) {
            auto kind = rootOf(x)->kind;
            return kind == Place::Loan || kind == Place::Address;
        };

        for(auto x: holderReach) {
            if(anywhere(x)) return true;

            for(auto y: reached) {
                if(anywhere(y) || placesOverlap(base, decl, body, x, y)) return true;
            }
        }

        return false;
    };

    /*
     * A loan of this frame's own storage in use where that storage is gone - one check (J128).
     *
     * MIR locals are frame slots and drops are placed by liveness, so a loan held in `q` keeps its
     * lender alive through `q`'s uses - but only back to the lender's `Init` above them, since
     * nothing can borrow a local before it has a value. A loan of it **live across its `Init`** was
     * taken on an earlier pass of a loop, from a value the drop placement already released: `let
     * owned = [..]; q = values(owned)` in a `while` read the previous pass's freed run through `q`
     * (J127's `W1`). A local a **continuation** declares is gone with one run of it, however the
     * blocks inside run, so a loan of one live on entry to that region's entry block or to a block
     * outside it is from a run that has finished (J124's `Y` and `Z`).
     *
     * Those were two checks and the seam between them is where J128's probes went through: the
     * `Init` test skipped a continuation's storage and left it to the region test, which looks only
     * at the region's own edges - so a `while` inside a `for x in items(xs)` body or a lens block had
     * no test at its back edge, and a stale use in any block of a pass but the first was not
     * reported and ended in the conversion. So each of the three is only where a loan is **found**
     * stale (`StaleLoans`), and all of them are reported by one walk: the loan stays armed in every
     * block it is live on entry to, and the first node reading it names it. That lands on the first
     * use in block order rather than necessarily on the stale one, which is still a use of a loan
     * that some path reaches without its storage.
     *
     * Owned storage only: a loan of a binding of a borrow - `x` in `for x in each(xs)` - is a
     * reborrow of what `x` refers to, which binding `x` again does not replace
     * (`lens/HandoverSlot.yana`). Only the loans on the local itself: a raw address may point
     * anywhere, and an `Init` is not a write through one.
     */
    StaleLoans stale;
    for(U32 w = 0; w < loanWords; w++) {
        stale.pending.push(0); stale.reported.push(0); stale.pass.push(0); stale.hit.push(0);
    }

    auto staleFoundAt = [&](U32 l) {
        U32 i = 0;
        while(stale.foundLoan[i] != l) i++;
        return i;
    };

    auto foundStale = [&](U32 l, bool pass, LocationId at, U64* escaped) {
        if(BitMatrix::test(stale.reported.pointer(), l)) return false;

        BitMatrix::set(escaped, l);
        if(BitMatrix::test(stale.pending.pointer(), l)) return true;

        BitMatrix::set(stale.pending.pointer(), l);
        if(pass) BitMatrix::set(stale.pass.pointer(), l);
        stale.foundLoan.push(l);
        stale.foundAt.push(at);
        return true;
    };

    auto findStaleInit = [&](Node& node, PlaceId place, const U64* live, U64* escaped) {
        auto root = rootOf(place);
        if(root->kind != Place::Local) return false;
        if(!body.local(base, LocalId(root->payload))->occurrence.contract.isOwned()) return false;

        auto held = loansByRoot.get(rootKey(Place::Local, root->payload));
        if(!held) return false;

        auto found = false;
        for(auto l: held.unwrap()) {
            if(!BitMatrix::test(live, l)) continue;
            if(!placesOverlap(base, decl, body, place, loanAt(l)->place)) continue;
            found |= foundStale(l, true, node.source, escaped);
        }

        return found;
    };

    /*
     * `discards` is a write that replaces a whole owned local whose old value has no teardown
     * (J128). Such a write reaches the local's storage and nothing its old value refers to: the old
     * value is overwritten unread, and with no teardown nothing runs that could reach through it. So
     * the write conflicts with a loan of the local and with what holds one, and not with a loan the
     * old value itself held or with a loan of what that one names - re-pointing `p = valuesMut(xs) +
     * k` at a fresh unique loan of `xs` is not an access to `xs`. Before J127 the loop form was
     * accepted only because the old loan and the fresh one had one id and the write's `skip` set
     * held it; the straight-line form was refused, as was every re-point of a record holding a
     * unique loan (`c = Cursor {at: xs, n: 1}`). A type with a teardown keeps the whole reach,
     * because its teardown runs on the old value and may reach through the old loans - the case
     * Rust's NLL refuses for the same reason.
     */
    auto check = [&](OriginCursor& cursor, Node& node, PlaceId place, AccessKind kind,
                     const U64* live, const U64* skip, LoanId taken = LoanId(), bool discards = false) {
        // A copy meets the unique loans of the storage it names and nothing else - see `uniqueLoans`.
        if(kind == AccessKind::Copy) {
            if(!BitMatrix::intersects(live, uniqueLoans.pointer(), loanWords)) return;

            auto root = rootOf(place);
            if(root->kind == Place::Loan || root->kind == Place::Address) return;

            auto held = loansByRoot.get(rootKey(root->kind, root->payload));
            if(!held) return;

            BitMatrix::zero(conflict.pointer(), loanWords);
            auto any = false;
            for(auto l: held.unwrap()) {
                if(!BitMatrix::test(live, l) || !BitMatrix::test(uniqueLoans.pointer(), l)) continue;
                if(BitMatrix::test(refused.pointer(), l)) continue;
                if(!placesOverlap(base, decl, body, place, loanAt(U32(l))->place)) continue;

                BitMatrix::set(conflict.pointer(), l);
                any = true;
            }

            if(!any) return;
            reached.clear();
            reached.push(place);
        } else if(!BitMatrix::any(live, loanWords)) {
            return;
        } else {
            if(discards) {
                reached.clear();
                reached.push(place);
                BitMatrix::zero(reachedSet.pointer(), loanWords);
            } else {
                reachFrom(cursor, place, reached);
            }

            // Through a loan already refused - see `refused`. `reachedSet` is what `reachFrom` left:
            // the loans this place's origins reach, closure included.
            if(BitMatrix::intersects(refused.pointer(), reachedSet.pointer(), loanWords)) return;

            // Kept for the reborrow test below, since the walks the candidates ask for overwrite it.
            BitMatrix::copy(through.pointer(), reachedSet.pointer(), loanWords);

            conflictsOf(cursor, reached, live, conflict.pointer());
        }

        /*
         * Whether this access **excludes** - an exclusive loan or a write - which is what decides
         * whether it can write what it reaches. What it writes is what it reaches with its own
         * capability: the walk through unique loans alone (`reachFrom`'s `exclusiveOnly`).
         */
        auto taking = kind == AccessKind::TakeLoan
            ? (taken.valid() ? taken : ((NodeRead&)node).loan) : LoanId();
        auto excluding = kind == AccessKind::Write ||
            (kind == AccessKind::TakeLoan && loanAt(taking)->capability == Access::Unique);

        // The access's unique-only conflicts, asked the first time a candidate needs them: most
        // excluding accesses meet no loan at all.
        auto directKnown = false;

        SmallArray<U32, 16> candidates;
        BitMatrix::forEach(conflict.pointer(), loanWords, [&](Size l) { candidates.push(U32(l)); });

        for(auto l: candidates) {
            if(skip && BitMatrix::test(skip, l)) continue;

            /*
             * A loan **this one is taken through** - an exclusive loan the place's origins reach at
             * any depth, rather than only at the first, which is all `skip` holds.
             *
             * `it.next()` over an object lent `&s` hands back a view whose origin is the loan of
             * `it`, and `it` holds the exclusive loan of `s`: binding `Just(x)` out of that result
             * reaches `s` through both, and met the loan of `s` as a conflict - "s is already
             * borrowed exclusively" - where `for x in it` was accepted (J188). Taking a loan through
             * a unique one is what the unique loan's holder lends, and every link between them
             * stays live for as long as the new loan does, so a later use of the holder is what
             * meets it. Only unique links are excused: `reachFrom` lists a place behind a shared one
             * too, and a unique loan taken through a shared one is not a reborrow.
             */
            if(kind == AccessKind::TakeLoan && BitMatrix::test(through.pointer(), l) &&
               BitMatrix::test(uniqueLoans.pointer(), l)) {
                continue;
            }
            // A copy is gone before a reservation activates - see `uniqueLoans`.
            if(kind == AccessKind::Copy && BitMatrix::test(reserved.pointer(), l)) continue;

            auto& other = *loanAt(U32(l));

            /*
             * **Two accesses conflict only where one of them can write what the other reaches.**
             *
             * `conflict` is the coarser question - whether the two reach a common place at all -
             * and a place one side reaches only *through a shared loan* its storage holds is a
             * place that side can read and nothing more: a shared loan hands out shared access to
             * its place, whatever capability the storage holding it was reached with. So a
             * candidate stands only where the writing side's unique-only reach meets the other's
             * whole reach, asked in both directions:
             *
             *  - this access writes what the loan reaches - its unique-only reach against the
             *    loan's, which is `conflictsOf` over that reach (`exclusiveConflict`);
             *  - the loan writes what this access reaches - the loan's unique-only reach against
             *    `reached` (`uniqueReachOverlaps`).
             *
             * A side that is shared writes nothing and asks nothing, and two shared sides never
             * conflict (below). Sound because both halves keep the *place* itself: `reachFrom`
             * always lists it, so two accesses to one place, or to overlapping ones, still meet
             * where either can write - and a write to `source` while a unique loan holding `source`
             * is live meets through that loan's own reach. What is admitted is only the pair
             * neither of which can write anything the other can see, which no order of their uses
             * can observe.
             *
             * `remember(box, source)` with `box` built from `source` is the exclusive-against-shared
             * shape (`resolve/LoanSlot.yana`); `rememberOwned(itself(box), source)`, whose returned
             * `&owner'Owned` reaches `source` through `box.held` alone, the shared-against-unique
             * one; and `touch(box1, box2)`, two exclusive borrows of records each holding a shared
             * loan of one source, the pair where both sides are exclusive
             * (`loans/SharedThroughExclusive.yana`). The last was refused while the rule was two
             * one-sided excuses, which left that pair comparing whole reach against whole reach.
             */
            // A copy's candidates overlap the place it names, which is already the meeting.
            if(kind != AccessKind::Copy && (excluding || other.capability == Access::Unique)) {
                auto meets = false;

                if(excluding) {
                    if(!directKnown) {
                        SmallArray<PlaceId, 8> direct;
                        if(discards) direct.push(place);
                        else reachFrom(cursor, place, direct, true);
                        conflictsOf(cursor, direct, live, exclusiveConflict.pointer());
                        directKnown = true;
                    }

                    meets = BitMatrix::test(exclusiveConflict.pointer(), l);
                }

                if(!meets && other.capability == Access::Unique) {
                    meets = uniqueReachOverlaps(cursor, other.place);
                }

                if(!meets) continue;
            }

            /*
             * A loan whose taking was refused, met by a **write** replacing what holds it - the
             * program was told it does not have it. With generations (J127) the refused `it.next()`
             * is what `q = p` stores over the previous pass's, and that is the one mistake the first
             * report named. A borrow meeting one is still reported (`Reject.SharedThroughExclusive`
             * asks for both lines).
             */
            if(kind == AccessKind::Write && BitMatrix::test(refused.pointer(), l)) continue;

            /*
             * At an **activation** - see `reserved` - a conflict with a loan the program has already
             * been told it does not have is the same mistake read a second time.
             *
             * Two forms of it: the loan itself was refused (`swap(x, x)`, where the second borrow is
             * reported where it is written and the first becoming exclusive at the call adds
             * nothing), and the loan is a *reborrow* of one that was (`takeTwo(xs[0], xs[1])`, where
             * each element is parked in a temporary whose own loan is rooted in the refused one).
             * `refused` is what the take side already consults through `reachFrom`, one level down.
             */
            if(taken.valid()) {
                if(BitMatrix::test(refused.pointer(), l)) continue;

                BitMatrix::zero(scratch.pointer(), loanWords);
                unionPlaceOrigins(cursor, other.place, scratch.pointer());
                if(BitMatrix::intersects(refused.pointer(), scratch.pointer(), loanWords)) continue;
            }

            // Any number of shared loans may coexist. Everything else - a unique loan meeting any
            // overlapping loan, and a write or a move meeting either - conflicts.
            if(kind == AccessKind::TakeLoan) {
                if(taking.index == l) continue;

                auto capability = loanAt(taking)->capability;
                if(capability == Access::Shared && other.capability == Access::Shared) continue;

                /*
                 * The reservation above. A shared read never excludes one; another loan does only
                 * where it is still live at the activation, which is the point the exclusion
                 * begins - see `reserved`, where both halves are argued.
                 */
                if(BitMatrix::test(reserved.pointer(), l)) {
                    /*
                     * An activation past this block stands in as the block's exit: a loan dead
                     * there is dead at the activation too, and one live there is refused here. Since
                     * J141 the reservation is carried to the join as a must-fact and the join
                     * checks the activation itself, so the exit matters where the must-fact *drops*
                     * a reservation (a predecessor not walked yet) and for keeping the blame on the
                     * argument rather than the call (J140, `holds3` in `Reject.TwoPhase`).
                     */
                    auto activation = activationOf[l];
                    auto survives = activation != kNoReservation
                        ? BitMatrix::test(before.row(activation), taking.index)
                        : BitMatrix::test(blockExit, taking.index);

                    if(capability == Access::Shared && (activation != kNoReservation || !survives)) {
                        continue;
                    }

                    if(!BitMatrix::test(windowed.pointer(), l) && !survives) continue;
                }
            }

            /*
             * The element exemption - `exchangeOnlyLoan`, above. The pair is excused when this
             * access is one half and the loan it conflicts with is the other: a second element
             * loan being taken, or a relocation writing through the first. Both halves are asked,
             * so a swap's loan meeting an ordinary borrow of the container is refused as any
             * other pair is.
             */
            auto half = kind == AccessKind::TakeLoan ? exchangeOnlyLoan(taking) : exchangeAccess(node);
            if(half && exchangeOnlyLoan(LoanId(l))) continue;

            auto here = accessText(place);
            auto there = accessText(other.place);
            auto exclusive = other.capability == Access::Unique;

            if(kind == AccessKind::Copy) {
                report(node.source,
                       BitMatrix::test(captured.pointer(), l)
                           ? "%@ cannot be read here, because a closure captured %@ by reference and that closure is still in use - a `&` capture is exclusive for as long as the closure lives"_v
                           : "%@ cannot be read here, because %@ is borrowed exclusively and that borrow is still in use"_v,
                       here, there);
            } else if(kind == AccessKind::Write) {
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

                /*
                 * And its previous generation (J128), which is what a stale copy holds: the take was
                 * refused because that copy is still in use, so every later access through it is the
                 * same mistake - `q` holding last pass's `it.next()` item was reported again at the
                 * borrow through it and at `setPtr(q, p)`.
                 */
                if(kind == AccessKind::TakeLoan) {
                    BitMatrix::set(refused.pointer(), taking.index);
                    if(generationSource.size() && taking.index < mirLoans &&
                       generationOf[taking.index] != kNoGeneration) {
                        BitMatrix::set(refused.pointer(), generationOf[taking.index]);
                    }
                }
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
    // Two rows and not one, because the two questions are asked at different points. An access is
    // judged against what survives it, which is what keeps a loan from conflicting with itself; an
    // **activation** - see `reserved` - is judged against what is live where the exclusion begins,
    // which includes the loans this very node is the last use of. `holds(g, peek(g))` is the
    // difference: the borrow the second argument hands over dies *at* the call, and the call is
    // where the first argument's reservation becomes exclusive.
    SmallArray<U64, 4> live;
    for(U32 w = 0; w < loanWords; w++) live.push(0);

    // Two cursors over one block and not one, because the two passes below run in opposite
    // directions. Declared out here rather than per block so that the state buffer is allocated
    // once for the body.
    OriginCursor backward { *this }, forward { *this };

    /*
     * The call bounds live in a block **outside** the region their call opened - a copy of a
     * `%yield'T` pointer kept past the call, whose frame it points into is gone. `boundState` is per
     * bound: 0 not seen escaping, 1 escaping and not yet reported, 2 reported. `escaped` is the row
     * of the ones escaping into the block being walked, which `reportCallBoundEscapes` reports at the
     * first node that uses one.
     */
    // The stale bounds after them, at `boundRegions.size() + k` - `staleBounds`, which escape
    // wherever they are live at all, since every block a previous pass can reach is too late.
    SmallArray<U8, 2> boundState;
    for(U32 k = 0; k < boundRegions.size() * 2; k++) boundState.push(0);

    SmallArray<U64, 4> escaped;
    for(U32 w = 0; w < loanWords; w++) escaped.push(0);

    // And the loans of a continuation's own storage - `scopedLoans` - which are found stale at the
    // region's edges (`StaleLoans`).
    scopedLoans.clear();
    scopedRegions.clear();
    for(U32 l = 0; l < (body.regions.size() > 1 ? bodyLoans : 0); l++) {
        auto root = rootOf(loanAt(U32(l))->place);
        if(root->kind != Place::Local) continue;

        /*
         * Owned storage only. A loan of a *binding* of a borrow - `x` in `for x in each(xs)` - is a
         * reborrow: what it points at is what `x` refers to, which `x`'s own origins say and which
         * the continuation's frame does not hold. Where that is the callee's frame - an unqualified
         * hand-over's root - the origins say so with the call's bound (`findCallBounds`, J129). Counting it made `cell = {cell | held: x}` over
         * `-> a'Int` a use of the previous pass's `x` (`lens/HandoverSlot.yana`).
         */
        auto local = body.local(base, LocalId(root->payload));
        if(!local->occurrence.contract.isOwned()) continue;

        auto declared = local->region;
        if(!declared.valid()) continue;

        auto region = body.region(base, declared);
        if(!region || region->kind != Continuation::Handover) continue;

        scopedLoans.push(l);
        scopedRegions.push(declared.index);
    }

    for(U32 index = 0; index < blockCount; index++) {
        auto block = this->block(index);
        auto count = block->nodes.size();
        if(!count) continue;

        after.reset(count, loanCount);
        before.reset(count, loanCount);
        blockExit = loansAt(loanOut, index);
        BitMatrix::copy(live.pointer(), loansAt(loanOut, index), loanWords);
        backward.enterAtExit(index);

        for(U32 n = count; n > 0; n--) {
            auto position = n - 1;
            BitMatrix::copy(after.row(position), live.pointer(), loanWords);
            backward.undo(position);
            stepLoansBackward(live.pointer(), backward, nodeAt(index, position));
            BitMatrix::copy(before.row(position), live.pointer(), loanWords);
        }

        forward.enter(index);

        /*
         * The reservations still waiting on entry - those every predecessor left reserved (J141).
         *
         * A must-fact, so the join is an intersection, and a predecessor not walked yet - a back
         * edge, or a block later in the list - contributes nothing, which is the conservative
         * direction: a reservation it drops is treated as already active, as every reservation was
         * before J141. What this lifts is a later argument that *branches* - `f(xs, if c then
         * length(xs) else 0)` - whose arms were walked with the first argument's loan active and
         * refused the read. `forEachPredecessor` rather than `incoming`, because a `Many` region's
         * back edge and skip edges are missing from the second and a must-fact over a missing edge
         * is unsound. Asked only once some block has left a reservation standing.
         */
        BitMatrix::zero(reserved.pointer(), loanWords);
        if(reservationsOut) {
            auto first = true;
            forEachPredecessor(index, [&](U32 from) {
                auto out = reservedOut.pointer() + Size(from) * loanWords;
                if(first) BitMatrix::copy(reserved.pointer(), out, loanWords);
                else for(U32 w = 0; w < loanWords; w++) reserved[w] &= out[w];
                first = false;
            });
        }

        auto escaping = false;
        for(U32 k = 0; k < boundRegions.size(); k++) {
            if(boundState[k] == 2 || !BitMatrix::test(loansAt(loanIn, index), bodyLoans + k)) continue;
            if(body.encloses(base, ContinuationId(boundRegions[k]), block->region)) continue;

            boundState[k] = 1;
            BitMatrix::set(escaped.pointer(), bodyLoans + k);
            escaping = true;
        }

        for(U32 k = 0; k < boundRegions.size(); k++) {
            auto stale = staleBounds[k];
            auto at = boundRegions.size() + k;
            if(stale == kNoStaleBound || boundState[at] == 2) continue;
            if(!BitMatrix::test(loansAt(loanIn, index), stale)) continue;

            boundState[at] = 1;
            BitMatrix::set(escaped.pointer(), stale);
            escaping = true;
        }

        // A continuation's storage live where one of its runs starts, or after it - found stale here.
        for(U32 i = 0; i < scopedLoans.size(); i++) {
            auto l = scopedLoans[i];
            if(!BitMatrix::test(loansAt(loanIn, index), l)) continue;

            auto region = body.region(base, ContinuationId(scopedRegions[i]));
            auto inside = body.encloses(base, region->id, block->region);
            if(inside && region->entry != block->index) continue;

            foundStale(l, inside && region->multiplicity == Continuation::Many,
                       nodeAt(index, 0).source, escaped.pointer());
        }

        // And every loan found stale anywhere, in each block it is live on entry to until a node
        // reads it - the report does not depend on the use being in the block it was found in.
        for(U32 w = 0; w < loanWords; w++) {
            auto armed = stale.pending[w] & loansAt(loanIn, index)[w];
            escaped[w] |= armed;
            if(armed) escaping = true;
        }

        // And where each of this block's reservations activates - the first operand naming the
        // read that minted it, which is the same walk the forward pass makes below.
        for(U32 l = 0; l < loanCount; l++) activationOf[l] = kNoReservation;

        for(U32 position = 0; position < count; position++) {
            auto& node = nodeAt(index, position);
            if(forwardsReservation(node)) continue;

            forEachOperand(base, node, [&](Operand& operand, OperandRole) {
                if(operand.source() != SourceKind::Result) return;

                auto minted = loanOfNode[operand.nodeId().index];
                for(; minted != kNoReservation; minted = chained(minted)) {
                    if(activationOf[minted] == kNoReservation) activationOf[minted] = position;
                }
            });
        }

        for(U32 position = 0; position < count; position++) {
            auto& node = nodeAt(index, position);
            auto at = after.row(position);

            /*
             * Every reservation this node **uses** - see `reserved`, where the rule is argued.
             *
             * Before the checks, so that a node which both activates a loan and takes another is
             * judged with the first one active; and checked here as the take itself would have been,
             * because that is the point at which the exclusion begins.
             */
            if(!forwardsReservation(node)) forEachOperand(base, node, [&](Operand& operand, OperandRole) {
                if(operand.source() != SourceKind::Result) return;

                auto minted = loanOfNode[operand.nodeId().index];
                auto read = readOfNode[operand.nodeId().index];

                for(; minted != kNoReservation; minted = chained(minted)) {
                    if(!BitMatrix::test(reserved.pointer(), minted)) continue;

                    BitMatrix::clear(reserved.pointer(), minted);

                    // A loan whose taking was already reported is a loan the program does not have -
                    // `refused`, and checking its activation would be the same mistake read twice.
                    if(BitMatrix::test(refused.pointer(), minted)) continue;

                    // The operand's own read is the skip for every link: what it reaches includes
                    // each loan the chain was filled from, and the temporary's own loan is one of
                    // them rather than a conflict with the reservation behind it.
                    auto& loan = *loanAt(U32(minted));
                    check(forward, node, loan.place, AccessKind::TakeLoan, before.row(position),
                          skipOfValue(NodeId(read)), LoanId(minted));
                }
            });

            if(escaping) {
                escaping = reportCallBoundEscapes(forward, node, escaped.pointer(), boundState);
                if(escaping) escaping = reportStaleStorage(forward, node, escaped.pointer(), stale);
            }

            switch(node.kind) {
                case Node::Read: {
                    auto& read = (NodeRead&)node;
                    auto place = read.place.placeId();

                    switch(read.place.mode()) {
                        case UseMode::BorrowShared:
                        case UseMode::BorrowUnique:
                            check(forward, node, place, AccessKind::TakeLoan, at,
                                  skipOfValue(node.id));

                            // And the reservation, which is what makes the *next* read of the same
                            // storage legal until something uses this one - see `reserved`.
                            if(read.loan.valid() &&
                               loanAt(read.loan)->capability == Access::Unique) {
                                BitMatrix::set(reserved.pointer(), read.loan.index);
                            }

                            break;

                        // A move out of storage is a write to it: what was there is gone, and a loan
                        // of it would be left pointing at a hole.
                        case UseMode::Take:
                            check(forward, node, place, AccessKind::Write, at,
                                  skipOfPlace(forward, place));
                            break;

                        case UseMode::CopyTrivial:
                        case UseMode::Inspect:
                        case UseMode::Forward:
                        case UseMode::Count:
                            check(forward, node, place, AccessKind::Copy, at, nullptr);
                            break;
                    }

                    break;
                }

                case Node::Write: {
                    auto& write = (NodeWrite&)node;
                    if(write.destination.source() != SourceKind::Place) break;

                    // Before the `Init` test: a global's slot is filled by `Init` too, and what
                    // this asks is what the slot is left holding rather than what it held.
                    checkGlobalConformance(forward, node);

                    // An `Init` establishes a slot rather than writing through one, so there is
                    // nothing in it for a loan to be pointing at - unless a loan of what an
                    // earlier pass put there is still in use, which is found stale here.
                    if(write.mode == Destination::Init) {
                        if(findStaleInit(node, write.destination.placeId(), at, escaped.pointer())) {
                            escaping = true;
                        }
                        break;
                    }

                    auto place = write.destination.placeId();
                    auto replaced = replacedLocal(node);
                    auto discards = replaced && !hasTeardownAt(decl, replaced->occurrence);
                    check(forward, node, place, AccessKind::Write, at, skipOfPlace(forward, place),
                          LoanId(), discards);
                    break;
                }

                case Node::Return:
                    checkReturnConformance(node);
                    checkDestinationConformance(forward, node);
                    break;

                case Node::Call:
                    checkHandoverConformance(node);
                    [[fallthrough]];

                case Node::LensCall:
                case Node::LensTry:
                    checkGlobalConformance(forward, node);
                    checkGlobalAccessAcrossCall(node, before.row(position), refused.pointer());
                    break;

                default:
                    break;
            }

            /*
             * A stale loan's own mint, again. Above the read that takes a loan, the backward walk
             * names the previous pass's loan by the loan's own id - it is the rename run backwards
             * (`stepLoansBackward`) - and below it the holders carry its generation. So what was
             * found stale under the loan's id is its generation from here on, and the loan this read
             * takes is fresh: left armed, the first use of the fresh loan was reported rather than
             * the stale copy's (`let r = owned` in a loop). Still pending, because another block may
             * be reached by a path that does not pass this read.
             */
            if(node.kind == Node::Read && ((NodeRead&)node).loan.valid()) {
                auto l = ((NodeRead&)node).loan.index;
                if(BitMatrix::test(stale.pending.pointer(), l)) {
                    BitMatrix::clear(escaped.pointer(), l);

                    if(generationSource.size() && l < mirLoans && generationOf[l] != kNoGeneration) {
                        auto found = staleFoundAt(l);
                        if(foundStale(generationOf[l], BitMatrix::test(stale.pass.pointer(), l),
                                      stale.foundAt[found], escaped.pointer())) {
                            escaping = true;
                        }
                    }
                }
            }

            // After the checks and outside the switch, because every case above asks what storage
            // held on *entry* to the node and two of them leave the switch early.
            stepOriginsForward(forward, node);
        }

        // What this block leaves reserved - see the entry, above.
        if(BitMatrix::any(reserved.pointer(), loanWords)) {
            if(!reservationsOut) {
                reservedOut.clear();
                for(Size w = 0; w < Size(blockCount) * loanWords; w++) reservedOut.push(0);
                reservationsOut = true;
            }

            BitMatrix::copy(reservedOut.pointer() + Size(index) * loanWords, reserved.pointer(), loanWords);
        }

        BitMatrix::zero(escaped.pointer(), loanWords);
    }

    // A bound live outside its region that no node there used - carried on an edge and nowhere
    // else. Reported at the call, since there is no use to point at.
    for(U32 k = 0; k < boundRegions.size(); k++) {
        if(boundState[k] != 1) continue;

        auto& call = (NodeLensCall&)*body.node(base, boundCalls[k]);
        report(call.source, "what %@ hands over is still held after the call returns, and what it refers to is gone by then - use it inside the block the call runs"_v,
               context.findName(decl[call.callee]->name));
    }

    // A loan found stale that no node read after it was found - its use is in a block walked
    // before that. Reported where it was found, which for an `Init` is J127's sentence.
    for(U32 i = 0; i < stale.foundLoan.size(); i++) {
        auto l = stale.foundLoan[i];
        if(!BitMatrix::test(stale.pending.pointer(), l)) continue;

        BitMatrix::clear(stale.pending.pointer(), l);
        auto lender = placeText(context, decl, base, body, *body.place(base, loanAt(l)->place));
        report(stale.foundAt[i],
               BitMatrix::test(stale.pass.pointer(), l)
                   ? "%@ is made again here while a value referring to what an earlier pass of the loop put there is still in use, and that is gone by then - use it inside the pass that made it"_v
                   : "a value referring to %@ is still in use after the block it is declared in has finished, and it is gone by then - that block is the call's continuation, so use it inside the block"_v,
               lender);
    }

    for(U32 k = 0; k < boundRegions.size(); k++) {
        if(boundState[boundRegions.size() + k] != 1) continue;

        auto& call = (NodeLensCall&)*body.node(base, boundCalls[k]);
        report(call.source, "what %@ hands over on one pass is still held on the next, and what it refers to is gone by then - use it inside the pass that received it"_v,
               context.findName(decl[call.callee]->name));
    }

    return errors == 0;
}

/*
 * The first use, at `node`, of a call bound `escaped` holds - a value a lens handed over for the
 * length of its call, still in use in a block the call's region does not enclose (`checkLoans`).
 * True while a bound in `escaped` is still unreported.
 *
 * Asked of what the node **reads**. A write replacing a local that holds the bound discards it
 * unread, which is where `stepLoansBackward` ends a loan's extent too; a write into part of one
 * reads nothing the pointer points at.
 */
bool Analysis::reportCallBoundEscapes(OriginCursor& cursor, Node& node, U64* escaped,
                                      SmallArray<U8, 2>& boundState) {
    forEachOperand(base, node, [&](Operand& operand, OperandRole role) {
        if(role == OperandRole::WriteTarget) return;

        forEachOperandOrigin(cursor, operand, [&](U32, const U64* row) {
            for(U32 k = 0; k < boundRegions.size(); k++) {
                auto& call = (NodeLensCall&)*body.node(base, boundCalls[k]);

                auto l = bodyLoans + k;
                if(BitMatrix::test(escaped, l) && BitMatrix::test(row, l)) {
                    BitMatrix::clear(escaped, l);
                    boundState[k] = 2;

                    report(node.source, "this uses a value that %@ handed over for the length of its call after that call returned, and what it refers to is gone by then - use it inside the block the call runs"_v,
                           context.findName(decl[call.callee]->name));
                }

                // The previous pass - `staleBounds`. One report per node: a value holding both is
                // the use the first sentence already names.
                auto stale = staleBounds[k];
                if(stale == kNoStaleBound || !BitMatrix::test(escaped, stale)) continue;
                if(!BitMatrix::test(row, stale)) continue;

                BitMatrix::clear(escaped, stale);
                if(boundState[k] == 2 && BitMatrix::test(row, l)) {
                    boundState[boundRegions.size() + k] = 2;
                    continue;
                }

                boundState[boundRegions.size() + k] = 2;
                report(node.source, "this uses a value that %@ handed over on an earlier pass of its loop, and what it refers to is gone by then - each pass hands over a new one, so use it inside the pass that received it"_v,
                       context.findName(decl[call.callee]->name));
            }
        });
    });

    return BitMatrix::any(escaped, loanWords);
}

/*
 * The first use, at `node`, of a loan of this frame's own storage found stale - `StaleLoans` - and
 * still unreported. What counts as a use is what `reportCallBoundEscapes` counts. One report per node:
 * a value holding two stale loans - a loan and its previous generation, or two lenders of one
 * record - is one mistake at one use. True while anything in `escaped` is still unreported.
 */
bool Analysis::reportStaleStorage(OriginCursor& cursor, Node& node, U64* escaped, StaleLoans& stale) {
    auto reported = false;

    forEachOperand(base, node, [&](Operand& operand, OperandRole role) {
        if(role == OperandRole::WriteTarget) return;

        forEachOperandOrigin(cursor, operand, [&](U32, const U64* row) {
            auto any = false;
            for(U32 w = 0; w < loanWords; w++) {
                stale.hit[w] = escaped[w] & row[w] & stale.pending[w];
                any |= stale.hit[w] != 0;
            }
            if(!any) return;

            BitMatrix::forEach(stale.hit.pointer(), loanWords, [&](Size l) {
                // Everything found at the same point is the same mistake - the loans of one local
                // made again, or of one continuation's storage at one block - and is done with it.
                U32 found = 0;
                while(stale.foundLoan[found] != l) found++;

                for(U32 i = 0; i < stale.foundLoan.size(); i++) {
                    if(stale.foundAt[i] != stale.foundAt[found]) continue;

                    auto m = stale.foundLoan[i];
                    BitMatrix::clear(escaped, m);
                    BitMatrix::clear(stale.pending.pointer(), m);
                    BitMatrix::set(stale.reported.pointer(), m);
                }

                if(reported) return;

                reported = true;

                // Which sentence is decided at the use: a loan found where its storage is made again
                // is from an earlier pass wherever the storage lives on, and one read after its
                // continuation has finished is that, whatever found it.
                auto& loan = *loanAt(U32(l));
                auto& local = *body.local(base, LocalId(rootOf(loan.place)->payload));
                auto region = local.region.valid() ? body.region(base, local.region) : nullptr;
                auto inside = !region || region->kind != Continuation::Handover ||
                    body.encloses(base, region->id, this->block(cursor.blockIndex)->region);
                auto lender = placeText(context, decl, base, body, *body.place(base, loan.place));

                if(inside && BitMatrix::test(stale.pass.pointer(), l)) {
                    report(node.source, "this uses a value referring to %@ from an earlier pass of the loop body it is declared in, and it is gone by then - each pass has its own, so use it inside the pass that made it"_v,
                           lender);
                } else {
                    report(node.source, "this uses a value referring to %@ after the block it is declared in has finished, and it is gone by then - that block is the call's continuation, so use it inside the block"_v,
                           lender);
                }
            });
        });
    });

    return BitMatrix::any(escaped, loanWords);
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
    // Without the call bounds, which are this analysis' own and name no `Loan` the conversion could
    // look up - so what is published is the environment the body's loans alone would have given.
    // And without the generations, each folded into the loan it is a pass of: the conversion builds
    // one loan per `Read`, and which pass a value was rooted in is this analysis' own question.
    auto words = U32((mirLoans + 63) / 64);
    out.loanCount = mirLoans;
    out.loanWords = words;
    if(!mirLoans) return;

    auto nodes = body.nodes.size();

    // The prefix sum goes with the rows, because without it a row number means nothing: a value's
    // positions are a range and the range is what says which of them is the root.
    out.originRows.reserve(program.mir, nodes + 1);
    for(U32 i = 0; i <= nodes; i++) out.originRows.push(program.mir, valueRow[i]);

    auto rows = valueRow[nodes];
    out.origins.reserve(program.mir, Size(rows) * words);

    SmallArray<U64, 4> folded;
    auto tail = mirLoans % 64;
    auto last = tail ? (U64(1) << tail) - 1 : ~U64(0);

    for(U32 i = 0; i < rows; i++) {
        auto row = valueOrigins.row(i);

        if(!generationSource.size()) {
            for(U32 w = 0; w < words; w++) out.origins.push(program.mir, w + 1 == words ? row[w] & last : row[w]);
            continue;
        }

        folded.clear();
        for(U32 w = 0; w < words; w++) folded.push(w + 1 == words ? row[w] & last : row[w]);

        for(U32 g = 0; g < generationSource.size(); g++) {
            if(BitMatrix::test(row, mirLoans + g)) BitMatrix::set(folded.pointer(), generationSource[g]);
        }

        for(U32 w = 0; w < words; w++) out.origins.push(program.mir, folded[w]);
    }
}

/*
 * Whether a parameter's storage is **this frame's** rather than the caller's.
 *
 * An owned parameter was moved in, so the slot holding it belongs to the callee exactly as a `let`
 * does, and a hand-over that refers to it is the same mistake as one that refers to a local. A
 * parameter the caller lent - a slot origin or a call-bounded loan - is storage the caller keeps,
 * which is `classifyLender`'s `Parameter` and is read the same way by every check below.
 *
 * A hand-over is not made to name the *caller's* origin the way a result is - its default is the
 * continuation's bound, which is what lets a lens yield a view of its own frame at all. Where it does
 * name one, it is held to it exactly as a result is (`checkHandoverConformance`, J122).
 */
bool Analysis::frameOwnedParameter(const Local& local) {
    return decl[function.args.get(decl, local.parameter)]->occurrence.contract.isOwned();
}

/*
 * What a loan's storage is, for a check that holds the loan past the point this frame can see - a
 * result, a `&` parameter's slot, a global, a hand-over position unqualified or naming a slot. One
 * classification, because they refuse the same lenders and differ only in what they accept and what
 * the author is told; two copies of the cascade had already come to disagree about a reborrow.
 *
 * Two kinds are **followed** rather than judged, and each is followed only where its origins say
 * what it refers to - not merely how long that lives. The difference is the whole soundness argument:
 *
 *  - **`Address`**: a raw address whose pointee position a signature published - `values(xs)` is
 *    `%a'T`, so `borrow(values(xs) + i)` is rooted in whatever `xs` was, and the pointer value's
 *    pointee row holds exactly that. A pointee only a call bounds - a `%yield'T` hand-over - holds
 *    every argument of the call, which is what keeps the arguments alive and says nothing about
 *    whose storage the pointer is into; that one is `Unnamed`. A *copy* of such a pointer carries
 *    the call's bound as well (`findCallBounds`), which is `CallBound` here and refused alike. A
 *    pointer whose type names no pointee at all is `OpaqueAddress` (ownership.md, "Raw pointer
 *    results").
 *
 *  - **`Reborrow`**: a borrowing local whose origins name its referent. `hold` gives a borrowing
 *    value a frame slot so that a projection has a place to name - `xs[i]` is a call whose result is
 *    parked in one - and `let w = gArr` is one too. The slot is this frame's; what is *in* it is
 *    somebody else's, and `computeOrigins`' read rule unions the local's origins into every value
 *    read out of it beside the loan it mints - "the local's own root is always reached"
 *    (`selectPositions`) - so the lender's loan is in the same set and is judged there. That holds
 *    for an `Origin::Slot` local, whose producer published the relationship, and for a call-bounded
 *    one every write of which is such a borrow (`exactBorrows`).
 *
 * What is *not* followed is a call-bounded local that holds a **hand-over**: `let view = held() in:`
 * binds a loan the lens granted for the length of the call, and the origin walk unions every
 * argument's loans into it - which says the arguments must outlive the continuation, not that `view`
 * points into them. Following it accepted `let view = held(gArr) in: gLoose = {gLoose | held: view}`
 * with `held` yielding a local of its own: the global kept a pointer into a dead frame. So such a
 * binding is `Borrowed`, and an unnamed one `Unnamed`, and every holder refuses both.
 */
Analysis::Lender Analysis::classifyLender(U32 l) {
    if(callBound(l)) return Lender::CallBound;

    auto& loan = *loanAt(U32(l));
    auto root = rootOf(loan.place);

    switch(root->kind) {
        case Place::Global:
            return Lender::Program;

        case Place::Address: {
            auto pointer = NodeId(root->payload);
            if(valuePositions(pointer) < 2) return Lender::OpaqueAddress;

            auto& occurrence = valueOccurrence(*body.node(base, pointer));
            auto published = originContractAt(decl, occurrence, 1).origin() == Origin::Slot;
            return published ? Lender::Address : Lender::Unnamed;
        }

        case Place::Local:
            break;

        // A `Place::Loan` root is never interned - `Builder` makes local, global and address roots
        // only - so there is no fourth kind of storage to write a sentence for. Refused as the
        // frame's where assertions are off, which is the answer that admits nothing.
        default:
            assertTrue(false);
            return Lender::Frame;
    }

    /*
     * A loan of a place that is **itself a borrow** - the payload of a `Result(Fail, '[U8])` held in
     * a temporary, which is where `take(2)?` and `match take(2): Ok(h) -> ...` read it from. What the
     * read hands on is the reference the field holds, and its roots already carry the field's slot,
     * so the lender to judge is whoever lent *that* - exactly the local case below, asked of the
     * place rather than of its root. Judged as the temporary holding it instead, the unwrapped view
     * of a parameter could not be returned ("refers to a temporary") while the same view bound
     * without the carrier could (P1, J184).
     *
     * Only a field of a local this frame **owns** and that is not a parameter: those are the two
     * whose own row below is "this frame's storage", and a field of one that is a borrow is the one
     * place that answer is wrong. A parameter's fields, and a borrowed local's, are judged by the
     * rows that already know whose they are - `Reject.ClassBorrow.yana` is a call-bounded parameter
     * a reborrow would have waved through.
     */
    auto& local = *body.local(base, LocalId(root->payload));

    if(body.place(base, loan.place) != root && !local.isParameter() && !local.environment &&
       local.occurrence.contract.isOwned()) {
        auto& placed = body.place(base, loan.place)->occurrence.contract;
        if(!placed.isOwned() && placed.origin() == Origin::Slot) return Lender::Reborrow;
    }

    if(local.environment) return Lender::Environment;
    if(temporaryLocal(local)) return staticLiteralLoan(loan, *root) ? Lender::Program : Lender::Temporary;
    if(local.isParameter()) return frameOwnedParameter(local) ? Lender::Frame : Lender::Parameter;

    auto& contract = local.occurrence.contract;
    if(contract.isOwned()) return staticLiteralLoan(loan, *root) ? Lender::Program : Lender::Frame;
    if(contract.origin() == Origin::Slot) return Lender::Reborrow;

    if(!exactBorrowsKnown) computeExactBorrows();
    if(exactBorrows.get(root->payload)) return Lender::Reborrow;
    return local.name ? Lender::Borrowed : Lender::Unnamed;
}

/*
 * One loan of a value going somewhere that outlives this frame, taken to what it is really rooted in,
 * and `judge(loan, kind)` asked of every lender that is not followed - see `classifyLender` for which
 * two are. False as soon as a judge is.
 *
 * The four conformance checks each used to walk this themselves, and each walked it differently: the
 * result followed an address and the destination refused it by the compiler's name for it, the
 * global followed a `let w = gArr` and the result refused it as this function's storage. The
 * disagreements were all accidents of which check was written when, and one walk removes them.
 *
 * A loan already asked about in the same question is not asked again - it either failed, which ended
 * the question, or it is being answered further up. The row is `lendersSeen`, zeroed by the caller per
 * question, and it is what bounds a cycle of addresses through a loop.
 */
template<class F>
bool Analysis::judgeLenders(U32 l, F&& judge) {
    if(BitMatrix::test(lendersSeen.pointer(), l)) return true;
    BitMatrix::set(lendersSeen.pointer(), l);

    auto kind = classifyLender(l);
    if(kind == Lender::Reborrow) return true;
    if(kind != Lender::Address) return judge(l, kind);

    auto pointer = NodeId(rootOf(loanAt(U32(l))->place)->payload);
    auto clean = true;
    BitMatrix::forEach(originsOfValue(pointer, 1), loanWords, [&](Size m) {
        clean = clean && judgeLenders(U32(m), judge);
    });

    return clean;
}

// Every loan of one origin row through `judgeLenders`, as one question - `lendersSeen` is zeroed here,
// which is what makes it one. False as soon as a judge is.
template<class F>
bool Analysis::judgeRow(const U64* row, F&& judge) {
    BitMatrix::zero(lendersSeen.pointer(), loanWords);

    auto clean = true;
    BitMatrix::forEach(row, loanWords, [&](Size l) {
        clean = clean && judgeLenders(U32(l), judge);
    });

    return clean;
}

/*
 * Which published slot of parameter `root` a loan of `place` sits in.
 *
 * A loan of a place *inside* one of a parameter's slots - `t.second` over `t: Two(p', q')` - is rooted
 * wherever that slot is, which is what `selectPositions` says of the place: the slot the projection
 * walks into lands on the place's root. Anything else is the parameter's own root contract. The
 * result and the destination both ask this, so `fn secondOf(t: Two(p', q')) -> q'[Int] = t.second`
 * is the same answer as `store = {store | held: t.second}`.
 *
 * The first entry is the only one: no path of a schema crosses two slots (J132). A loan field is one
 * slot at its own path and `foldRecordSchema` does not carry the interior of the object it names -
 * `data Outer {inner: r'Two(p', q')}` has the one slot `.inner`, in `r`, and nothing for `p` or `q`.
 * So `o.inner.second` under `-> q'[Int]` is refused as another origin (review 2 §2.1): the position
 * that would say `q` does not exist here, and choosing a deeper entry would choose nothing. Accepting
 * it is a schema change - a loan field's own slots folded in at `[field, ...]`, and every
 * construction and installation filling them - rather than a choice made in this function.
 */
U16 Analysis::grantedSlot(PlaceId place, const Local& root) {
    auto& declared = decl[function.args.get(decl, root.parameter)]->occurrence;

    Projection projection;
    SmallArray<PositionMap, 8> map;
    projectionOf(place, projection);
    selectPositions(projection, root.occurrence, body.place(base, place)->occurrence, map);

    for(auto& entry: map) {
        if(entry.from != 0 && entry.to == 0) return originSlotAt(decl, declared, entry.from);
    }

    auto contract = declared.contract;
    return contract.origin() == Origin::Slot ? contract.slot() : kNoOriginSlot;
}

// A lent parameter's loan against the slot `declared` a holder names: `Refusal::None` where the loan
// sits in that slot, and otherwise which of the three ways it missed it.
Analysis::Refusal Analysis::slotRefusal(U32 l, U16 declared) {
    if(declared == kNoOriginSlot) return Refusal::Unpublished;

    auto& loan = *loanAt(U32(l));
    auto granted = grantedSlot(loan.place, *body.local(base, LocalId(rootOf(loan.place)->payload)));
    if(granted == declared) return Refusal::None;

    return granted == kNoOriginSlot ? Refusal::CallBounded : Refusal::OtherOrigin;
}

/*
 * The sentences, one per holder and refusal - a table rather than one switch per check, because the
 * switches had drifted: "this frame's own storage" in one and "this function's" in three, a hand-over
 * binding called "own storage", and a temporary named `local2` in the one check that had no case for
 * it. A cell left empty is a refusal that holder never makes - a global accepts an opaque address, a
 * hand-over position has no slot to compare - and `reportRefusal` asserts on one.
 *
 * No sentence names an address-rooted place or an unnamed local, since what either would print is
 * the compiler's text (`[%%3]`, `local2`) rather than the program's. The **closure** row is worth its
 * own words for the same reason: its environment is a local the author never wrote, and the fix it
 * names is the true one. A closure that outlives its frame owns what it captured and its
 * environment is a heap-placed block, and which placement a lambda gets is decided by the position
 * it is written at (`resolve_lambda.cpp`) - so what reaches here is a lambda written one binding too
 * far from the place it belongs.
 */
namespace {

struct RefusalSentence {
    StringView text;
    bool holder;    // the first hole is the holder's name
    bool lender;    // the next is the lender's
};

using R = RefusalSentence;

const RefusalSentence refusalSentences[U32(Analysis::Holder::Count)][U32(Analysis::Refusal::Count)] = {
    // Result
    {
        R{"this closure captures storage of this frame, so it cannot leave it - write the lambda at the position that takes it, so its environment is placed where the value goes"_v, false, false},
        R{"this result refers to a temporary - a value with no storage of its own, which lives in this function's frame and is gone when it returns. Return an owner (`Array`, `String`) instead, or a loan of storage the caller lent"_v, false, false},
        R{"this result is reached through a pointer whose type names no origin, so nothing says whose storage it is - write `%source'a`"_v, false, false},
        R{"this result refers to %@, a value a call lent for the length of that call, which is gone when this function returns"_v, false, true},
        R{"this result refers to a value a call lent for the length of that call, which is gone when this function returns"_v, false, false},
        R{"%@ is this function's own storage, so a borrow of it cannot outlive the call"_v, false, true},
        R{"this result refers to %@, and its type has no origin to say so"_v, false, true},
        R{"%@ is borrowed for the length of this call, so a borrow of it cannot be returned - publish an origin on it with `'`"_v, false, true},
        R{"%@ is in a different origin from the one the result names"_v, false, true},
    },

    // Destination
    {
        R{"%@ is left holding a closure that captures storage of this frame, which is gone when the caller reads it - write the lambda at the position that takes it, so its environment is placed where the value goes"_v, true, false},
        R{"%@ is left holding a loan of a temporary - a value with no storage of its own, which lives in this function's frame and is gone when the caller reads it. Store an owner in the field instead, or a loan of storage the caller lent"_v, true, false},
        R{"%@ is left holding a loan reached through a pointer whose type names no origin, so nothing says whose storage it is - write `%source'a`"_v, true, false},
        R{"%@ is left holding a loan of %@, a value a call lent for the length of that call, which is gone when the caller reads it"_v, true, true},
        R{"%@ is left holding a loan of a value a call lent for the length of that call, which is gone when the caller reads it"_v, true, false},
        R{"%@ is left holding a loan of %@, which is this function's own storage and is gone when the caller reads it"_v, true, true},
        R{"%@ is left holding a loan of %@, and its type names no origin for the caller to hold it by - write the field's loan group on the parameter"_v, true, true},
        R{"%@ is left holding a loan of %@, which is borrowed for the length of this call - publish the same origin on it with `'`"_v, true, true},
        R{"%@ is left holding a loan of %@, which is in a different origin from the one its field names"_v, true, true},
    },

    // Global
    {
        R{"%@ cannot hold a closure that captures storage of this frame, which is gone when this function returns - write the lambda at the position that takes it, so its environment is placed where the value goes"_v, true, false},
        R{"%@ cannot hold a loan of a temporary - a value with no storage of its own, which lives in this function's frame and is gone when it returns. A global holds a loan only of another global: bind the value with a module-level `let` first"_v, true, false},
        R{},
        R{"%@ cannot hold a loan of %@, a value a call lent for the length of that call - a global holds a loan only of another global"_v, true, true},
        R{"%@ cannot hold a loan of a value a call lent for the length of that call - a global holds a loan only of another global"_v, true, false},
        R{"%@ cannot hold a loan of %@, which is this function's own storage and is gone when it returns - a global holds a loan only of storage that lives as long as the program: another global"_v, true, true},
        R{},
        R{"%@ cannot hold a loan of %@, which the caller lent to this call - the loan ends when the call does, and the global does not. Give the global the value itself, or a loan of storage that lives as long as the program: another global"_v, true, true},
        R{},
    },

    // Handover
    {
        R{"this hands over a value referring to a closure's environment, which is this function's own storage, and its type has no origin to say so - write `yield'` on the position that refers to it"_v, false, false},
        R{"this hands over a value referring to a temporary, which lives in this function's frame, and its type has no origin to say so - write `yield'` on the position that refers to it"_v, false, false},
        R{},
        R{"this hands over a value referring to %@, a value a call lent for the length of that call, and its type has no origin to say so - write `yield'` on the position that refers to it"_v, false, true},
        R{"this hands over a value referring to something a call lent for the length of that call, and its type has no origin to say so - write `yield'` on the position that refers to it"_v, false, false},
        R{"this hands over a value referring to %@, which is this function's own storage, and its type has no origin to say so - write `yield'` on the position that refers to it"_v, false, true},
        R{},
        R{},
        R{},
    },

    // HandoverSlot - a position naming one of the caller's origins, which the caller may keep the
    // value in after the call returns. The Result row's refusals, said of a hand-over; the fix for a
    // view of this frame is the bound a hand-over has by default, which is spelled `yield'`.
    {
        R{"this hands over a closure that captures storage of this frame, which is gone when this function returns, and the origin its type names outlives it - write `yield'` on the position instead"_v, false, false},
        R{"this hands over a value referring to a temporary, which lives in this function's frame and is gone when it returns, and the origin its type names outlives it - write `yield'` on the position instead, or hand over a loan of storage the caller lent"_v, false, false},
        R{"this hands over a value reached through a pointer whose type names no origin, so nothing says it is in the origin the hand-over names - write `%source'a`"_v, false, false},
        R{"this hands over a value referring to %@, a value a call lent for the length of that call, and the origin its type names outlives it - write `yield'` on the position instead"_v, false, true},
        R{"this hands over a value referring to something a call lent for the length of that call, and the origin its type names outlives it - write `yield'` on the position instead"_v, false, false},
        R{"this hands over a value referring to %@, which is this function's own storage, and the origin its type names outlives it - write `yield'` on the position instead"_v, false, true},
        R{},
        R{"this hands over a value referring to %@, which is borrowed for the length of this call, and the origin its type names outlives it - publish that origin on it with `'`"_v, false, true},
        R{"this hands over a value referring to %@, which is in a different origin from the one its type names"_v, false, true},
    },
};

} // namespace

// What a holder naming slot `declared` makes of one lender - the judgement the result, the destination
// and the slot-naming hand-over share word for word: a global is accepted, a lent parameter is held to
// the slot, and every other kind is refused for what it is. `Refusal::None` accepts.
Analysis::Refusal Analysis::slotJudgement(U32 l, Lender kind, U16 declared) {
    if(kind == Lender::Program) return Refusal::None;
    return kind == Lender::Parameter ? slotRefusal(l, declared) : refusalOf(kind);
}

// The refusal a lender that is not followed gets for its kind alone. A lent parameter's is
// `CallBounded`, which is the whole of it for a global; the result and the destination compare its
// slot first.
Analysis::Refusal Analysis::refusalOf(Lender kind) {
    switch(kind) {
        case Lender::Environment:   return Refusal::Environment;
        case Lender::Temporary:     return Refusal::Temporary;
        case Lender::OpaqueAddress: return Refusal::OpaqueAddress;
        case Lender::Borrowed:      return Refusal::Borrowed;
        case Lender::Unnamed:       return Refusal::Unnamed;
        case Lender::CallBound:     return Refusal::Unnamed;
        case Lender::Parameter:     return Refusal::CallBounded;

        // Accepted or followed by every holder, so never refused for what it is.
        case Lender::Program:
        case Lender::Address:
        case Lender::Reborrow:
            assertTrue(false);
            [[fallthrough]];

        case Lender::Frame:
            return Refusal::Frame;
    }

    return Refusal::Frame;
}

void Analysis::reportRefusal(LocationId source, Holder holder, Refusal refusal, U32 l,
                             const String* holderName) {
    auto& sentence = refusalSentences[U32(holder)][U32(refusal)];
    assertTrue(sentence.text.size() != 0);
    assertTrue(!sentence.holder || holderName);

    /*
     * A **record that holds loans**, built here and returned at `o'R` (J164, review 3 §2.2).
     *
     * `o'SView` is an `SView` that lives in `o`'s storage - `source'Maybe(T)` is "itself reached
     * through that external access", ownership.md - so a new one is a temporary and the refusal is
     * right. What the author meant is almost always the record by value with its *fields'* loans in
     * `o`, which is the loan argument, and the owner-or-loan advice in the table names neither.
     */
    if(holder == Holder::Result && refusal == Refusal::Temporary && function.result.type &&
       !function.result.contract.isOwned() && decl[function.result.type]->kind == Type::Record &&
       ((RecordType*)decl[function.result.type])->schema) {
        auto name = typeText(context, decl, function.result.type);
        report(source, "this returns a new %@ at a loan of one, which would be a temporary of this frame - a record that holds loans is returned by value with its loans published as its loan argument: write `-> %@(source')` rather than `-> source'%@`"_v,
               name, name, name);
        return;
    }

    if(!sentence.lender) {
        if(sentence.holder) report(source, sentence.text, *holderName);
        else report(source, sentence.text);
        return;
    }

    auto lender = placeText(context, decl, base, body, *body.place(base, loanAt(U32(l))->place));

    /*
     * A **capture** returned from the lambda's body. Its environment is a parameter like any other
     * and the loan is call-bounded like any other, but "publish an origin on it" names a fix the
     * author cannot write - the environment has no written type to put the `'` on.
     */
    if(holder == Holder::Result && refusal == Refusal::CallBounded) {
        auto root = rootOf(loanAt(U32(l))->place);
        auto environment = function.environment && root->kind == Place::Local &&
                           body.local(base, LocalId(root->payload))->parameter == 0;
        if(environment) {
            report(source, "this closure returns a borrow of %@, which it captured - a closure's environment is lent to it for the length of each call, so a result cannot refer to a capture. Pass the storage as an argument in the result's origin instead"_v,
                   lender);
            return;
        }
    }

    if(sentence.holder) report(source, sentence.text, *holderName, lender);
    else report(source, sentence.text, lender);
}

/*
 * Analysis-Lens.md §12.3(c) at a hand-over - "an unqualified `%T` at a boundary is refused when the
 * value is frame-rooted".
 *
 * The result half of that sentence is `checkReturnConformance` below and needed nothing new. This is
 * the other half, and it is narrower than conformance rather than the same rule again: a hand-over's
 * root always states *something* - `Origin::CallBounded` is the default and is what makes a lens
 * able to yield a view of its own frame at all - so what can be missing is a statement about an
 * **interior** position, which is exactly the position a raw pointer's pointee is.
 *
 *     lens fn withPathCString(path: String) -> %U8      -- refused here
 *     lens fn withPathCString(path: String) -> %yield'U8
 *
 * The two differ in nothing the caller can see except the qualifier, and the qualifier is the whole
 * of it: the pointee of the first says nothing, so a continuation may copy the address into storage
 * that outlives the call and no rule has a term to object with - the pointer is trivially copyable,
 * so the copy is an owned scalar rooted in nothing. The second says the pointee is bounded by the
 * continuation, which every later walk already knows what to do with.
 *
 * Only where the value is rooted in **this frame's own storage**, which is §12.3(c)'s condition
 * verbatim and is what keeps `Native` code untouched: a pointer manufactured from an integer, an
 * allocator or foreign code is rooted in no loan at all, or in an address whose type names nothing,
 * so a bare `%T` stays what it is. A lent parameter and a global are the caller's and the program's,
 * and a reborrow of either is followed to them by the same walk the other three checks take.
 *
 * A position that **names one of the caller's origins** - `-> a'Int`, `-> %a'T`, an `iter fn`'s or a
 * skipping lens's item alike - is the opposite case and gets the result's rule instead (J122). The
 * lens call binds the continuation's parameter to exactly what filled that slot, so every consumer
 * trusts it as a reborrow and may keep it past the call; `lens fn bad(x: a'[Int]) -> a'[Int]` yielding
 * a local of its own compiled, and a caller storing its hand-over read freed memory. An iterator
 * *object* is the same MIR body converted a second time, so this one check covers it, and what its
 * consumer is handed through `next` is rooted in the object rather than in the slot anyway.
 */
void Analysis::checkHandoverConformance(Node& node) {
    auto& call = (NodeCall&)node;

    // The one call in a body that is the hand-over: through the continuation parameter, which the
    // desugaring appended and `yield` is the only thing that reads.
    if(!function.lens || !call.callsValue()) return;
    if(call.target.source() != SourceKind::Result) return;

    auto& target = *body.node(base, call.target.nodeId());
    if(target.kind != Node::Read) return;

    auto& read = (NodeRead&)target;
    if(read.place.source() != SourceKind::Place) return;

    auto& place = *body.place(base, read.place.placeId());
    if(place.kind != Place::Local || place.payload != function.continuationArg) return;

    auto arguments = call.arguments.contents(base);
    if(arguments.size() != 1) return;

    auto handed = *arguments.pointerAt(0);
    if(handed.source() != SourceKind::Result) return;

    auto produced = handed.nodeId();
    auto positions = valuePositions(produced);
    auto& value = valueOccurrence(*body.node(base, produced));

    auto judge = [&](U32 l, Lender kind) {
        if(kind == Lender::Program || kind == Lender::Parameter || kind == Lender::OpaqueAddress) {
            return true;
        }

        reportRefusal(node.source, Holder::Handover, refusalOf(kind), l);
        return false;
    };

    for(U32 p = 0; p < positions; p++) {
        auto declared = originSlotAt(decl, function.handover, p);

        /*
         * A position naming one of the **caller's** origins is a promise the caller acts on: the
         * lens call binds the continuation's parameter to exactly what filled that slot, so a
         * consumer may store the value wherever the slot's storage may be held, and read it after
         * this function has returned. That is a result's promise, and it is judged the way
         * `checkReturnConformance` judges one - the same judge, word for word, told as a hand-over.
         * A copy of a `%yield'T` hand-over's pointer carries its call's bound (`findCallBounds`),
         * which is refused here whatever else the set holds.
         */
        if(declared != kNoOriginSlot) {
            auto judgeSlot = [&](U32 l, Lender kind) {
                auto refusal = slotJudgement(l, kind, declared);
                if(refusal == Refusal::None) return true;

                reportRefusal(node.source, Holder::HandoverSlot, refusal, l);
                return false;
            };

            if(!judgeRow(originsOfValue(produced, p), judgeSlot)) return;
            continue;
        }

        // Otherwise the root is a view the callee may hand out of its own frame - `Origin::CallBounded`
        // is the default - and a position the declaration described states whatever it says,
        // `yield'` included. What is left is a position nothing was written about at all, which
        // `bindsOriginAt` is the one reader of.
        if(bindsOriginAt(decl, function.handover, p)) continue;

        if(!judgeRow(originsOfValue(produced, p), judge)) return;
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
 * *place*, because where a loan is rooted is where its storage lives. `judgeLenders` takes each loan
 * to its lenders; of those, one that lives as long as the program satisfies any slot, a lent
 * parameter satisfies the slot it sits in (`grantedSlot`), and everything else is a reference the
 * caller cannot be told the extent of.
 *
 * Each position of the returned value is judged against the slot the signature named for *it*.
 * Per position rather than over one union, because a result can state a relationship for its
 * interior positions as well as for its root: `-> Reader(a')` hands back an owned record whose
 * `held` field is in the caller's `a`, and asking about the value as a whole cannot tell that from a
 * root that names `a` and a field that names nothing.
 *
 * The first loan that fails ends the whole check, and that is not per position either. A value's
 * origin set holds every loan it may be rooted in, and one mistake usually puts several in it:
 * `return r.held` on a reader this frame built is the loan of the guard *and* the loan of the field
 * that holds it, which are one error about one statement. Reporting each would be the pass counting
 * its own intermediate values at the author.
 */
void Analysis::checkReturnConformance(Node& node) {
    auto& written = (NodeReturn&)node;
    if(!written.hasValue || written.value.source() != SourceKind::Result) return;

    auto produced = written.value.nodeId();
    auto positions = valuePositions(produced);

    for(U32 p = 0; p < positions; p++) {
        auto declared = originSlotAt(decl, function.result, p);

        /*
         * A position that refers to something the signature did not say whose - an owned
         * `-> Reader` handed back with a live guard in it, or a root borrowed with no origin on it -
         * is told so by the lender's name where it has one: the spelling that fixes the first is a
         * loan argument on the result's own type, `-> Reader(a')`, and the second is a `'` on the
         * result, so the message names the type rather than the position. A lender with no name
         * worth printing gets its own sentence instead.
         */
        auto judge = [&](U32 l, Lender kind) {
            auto refusal = slotJudgement(l, kind, declared);
            if(refusal == Refusal::None) return true;

            if(declared == kNoOriginSlot && (kind == Lender::Frame || kind == Lender::Borrowed)) {
                refusal = Refusal::Unpublished;
            }

            reportRefusal(node.source, Holder::Result, refusal, l);
            return false;
        };

        if(!judgeRow(originsOfValue(produced, p), judge)) return;
    }
}

/*
 * The body's half of an installation - what a `&` parameter holds when the call returns.
 *
 * `installedPositions` lets the caller believe the signature: after `remember(box, source)`, `box`
 * holds whatever `remember`'s group was bound to and nothing else. That is only true if the body
 * put nothing else there, and this is where it is held to it - the destination rule
 * `Reject.LoanDestination.yana` states, over the same origin state the caller half reads.
 * Every loan an interior position of a `&` parameter holds at a `return` must be rooted in a
 * parameter publishing the slot that position names: a local of this frame is gone when the caller
 * reads it, a call-bounded parameter's loan ended with the call, and another group is a set the
 * caller did not bind here. The same walk and the same slot question as the result's, and it names
 * the parameter's own type when that is where the missing origin belongs.
 */
void Analysis::checkDestinationConformance(OriginCursor& cursor, Node& node) {
    if(!loanCount) return;

    for(auto id: destinationParameters) {
        auto& local = *body.local(base, id);
        auto& parameter = *decl[function.args.get(decl, local.parameter)];
        auto positions = localPositions(id);

        for(U32 p = 1; p < positions; p++) {
            auto declared = originSlotAt(decl, parameter.occurrence, p);
            auto refusal = Refusal::None;
            auto refused = U32(0);

            auto judge = [&](U32 l, Lender kind) {
                refusal = slotJudgement(l, kind, declared);
                refused = l;
                return refusal == Refusal::None;
            };

            // `judgeRow` by hand, for the loan the row holds as well as the lender refused: the
            // first is what a store put there, and the store is where the report goes.
            expandInto(originsOfLocal(cursor.state(), id, p), expanded.pointer());
            BitMatrix::zero(lendersSeen.pointer(), loanWords);

            auto held = ~U32(0);
            BitMatrix::forEach(expanded.pointer(), loanWords, [&](Size l) {
                if(held == ~U32(0) && !judgeLenders(U32(l), judge)) held = U32(l);
            });

            if(held == ~U32(0)) continue;

            auto at = destinationStore(id, p, held, node.source);
            for(auto reported: destinationReports) {
                if(reported == at) return;
            }
            destinationReports.push(at);

            auto holder = context.findName(parameter.name);
            reportRefusal(at, Holder::Destination, refusal, refused, &holder);
            return;
        }
    }
}

/*
 * Where loan `l` was put into position `p` of the `&` parameter `id` - the store a destination
 * refusal is reported at (J132).
 *
 * The check itself is at a `return`, which is where "what the caller reads" is true, and it used to
 * report there - the implicit `return` of a `:` body being the header at column 0, so the author of
 * a long function was told the parameter and had to find the store. The origin state knows which
 * node moved a row, so this replays it: a forward pass over every block from its entry state,
 * noting each node after which the row holds the loan and before which it did not. The last such
 * node in block order is the answer; any of them put the loan there, so any is a true place to
 * point at, and the last is the one nearest the `return` in the common straight-line case. A loan
 * already in the row on entry to the body - which no store of this body put there - keeps the
 * `return`.
 *
 * Asked only when a refusal is made, so the replay costs a failing body and nothing else.
 */
LocationId Analysis::destinationStore(LocalId id, U32 position, U32 l, LocationId fallback) {
    auto at = stateIndex[l];
    if(at == kNotStored) return fallback;

    auto row = Size(localRow[id.index] + position) * stateWords;
    auto found = fallback;
    OriginCursor replay { *this };

    for(U32 b = 0; b < blockCount; b++) {
        replay.enter(b);

        auto count = nodeCount(b);
        for(U32 n = 0; n < count; n++) {
            auto& node = nodeAt(b, n);
            auto before = BitMatrix::test(replay.state() + row, at);
            stepOriginsForward(replay, node);

            if(!before && BitMatrix::test(replay.state() + row, at) && node.source != kNullLocation) {
                found = node.source;
            }
        }
    }

    return found;
}

/*
 * A loan **stored into a global** - ownership.md, "Globals", and the §7.4 rule
 * `Reject.LoanDestination.yana` states: a global holds a loan only of storage that lives as long as
 * the program does, which is another global.
 *
 * The destination decides the extent, as it does for a result and for a `&` parameter's slot - and
 * a global's extent is the program, so the one lender that conforms is one whose own extent is the
 * program too. A local of this frame is gone when the call returns and a parameter's loan ends with
 * the call, whatever origin its signature publishes: a caller's `'` names *its* storage, which may
 * be a local of its own. Asked at the write rather than at the `return`, unlike the two
 * conformances above, because nothing about a global is the caller's to read at a boundary - the
 * next function to read it is any function at all.
 *
 * Three shapes reach one, and each global is asked about separately - a call handed two globals
 * installs into both:
 *
 *  - a write whose place is rooted at a global - `g = {g | held: x}`, `g = R {held: x}` and the
 *    entry's own `let &g = R {held: x}`;
 *  - a call installing into a global argument, `remember(g, x)`, whose loans are the ones
 *    `applyInstall` would union in;
 *  - either of those **through a loan** of a global - `pair.a = R {held: x}` over a `pair` holding
 *    `&gStore`, or `remember(itself(gStore), x)` - which is a state write whose lender, followed
 *    the way `applyThrough` follows it, is a global. `applyThrough` has no rows for one to write.
 *
 * For a write into owned storage the value's root position is the value itself and not a loan it
 * keeps, so the positions asked are its interior ones; into a loan-typed place the root *is* the
 * loan. An installation never fills a root.
 */
void Analysis::checkGlobalConformance(OriginCursor& cursor, Node& node) {
    if(!loanCount) return;

    // Every case below is a `Write` or a call `callParts` answers; asked first, since this runs at
    // every node of the check walk and the rows below are built per call.
    if(node.kind != Node::Write && node.kind != Node::Call && !isLensCall(node.kind)) return;

    SmallArray<U64, 4> stored;
    for(U32 w = 0; w < loanWords; w++) stored.push(0);

    auto collect = [&](const U64* row) { BitMatrix::unionInto(stored.pointer(), row, loanWords); };

    // Every loan collected against `global`, then emptied for the next one; false once it reported.
    auto settle = [&](Place& global) {
        auto clean = true;
        BitMatrix::forEach(stored.pointer(), loanWords, [&](Size l) {
            clean = clean && storeIntoGlobal(node, global, U32(l));
        });

        BitMatrix::zero(stored.pointer(), loanWords);
        return clean;
    };

    auto collectWrite = [&](PlaceId destination) {
        auto first = body.place(base, destination)->occurrence.contract.isOwned() ? 1u : 0u;
        forEachOperandOrigin(cursor, ((NodeWrite&)node).value, [&](U32 p, const U64* row) {
            if(p >= first) collect(row);
        });
    };

    auto collectInstall = [&](U32 argument, SmallArray<U16, 4>& installed) {
        for(auto at: installed) forEachInstallSource(cursor, node, argument, at, collect);
    };

    DeclPtr<Function> callee;
    OperandList* arguments;

    if(node.kind == Node::Write) {
        auto destination = ((NodeWrite&)node).destination.placeId();
        auto root = rootOf(destination);

        if(root->kind == Place::Global) {
            collectWrite(destination);
            if(!settle(*root)) return;
        }
    } else if(callParts(node, callee, arguments)) {
        auto operands = arguments->contents(base);
        SmallArray<U16, 4> installed;

        for(U32 i = 0; i < operands.size(); i++) {
            auto place = installDestination(*operands.pointerAt(i));
            if(!place.valid()) continue;

            auto root = rootOf(place);
            if(root->kind != Place::Global) continue;

            installedPositions(node, i, installed);
            if(!installed.size()) continue;

            collectInstall(i, installed);
            if(!settle(*root)) return;
        }
    }

    // `stepOriginsForward`'s range - `computeNodeTables`.
    auto clean = true;
    SmallArray<U16, 4> installed;

    auto end = nodeTables[node.id.index + 1].writes;
    for(auto w = nodeTables[node.id.index].writes; w < end && clean; w++) {
        auto& found = writes[w];
        if(!found.goesThrough()) continue;

        // `Lenders` survives the walk, which clobbers only its own rows - see `throughScratch`.
        SmallArray<Place*, 2> globals;
        auto lenders = throughRow(ThroughRow::Lenders);
        gatherLenders(cursor, found, lenders);
        forEachUniqueLender(cursor, lenders, [&](U32, Place& root) {
            if(root.kind != Place::Global) return;

            for(auto known: globals) {
                if(known->payload == root.payload) return;
            }

            globals.push(&root);
        });

        for(auto global: globals) {
            if(found.argument == ~U32(0)) {
                collectWrite(((NodeWrite&)node).destination.placeId());
            } else {
                installedPositions(node, found.argument, installed);
                collectInstall(found.argument, installed);
            }

            clean = clean && settle(*global);
        }
    }
}

/*
 * One loan stored into `global`: accepted where it lives as long as the program, and remembered.
 *
 * What lives as long as the program is a global and a placed literal (`Lender::Program`), and a raw
 * address whose type names no pointee: raw access carries no validity promise (ownership.md, "Raw
 * pointer results"), and there is then nothing this check could be told. Everything else ends with
 * the call or the frame - a parameter's loan whatever origin its signature publishes, since a
 * caller's `'` names *its* storage, which may be a local of its own. `judgeLenders` is the walk from
 * the stored loan to those, the same one the result and the destination take.
 *
 * The global now holds a loan of each lender for as long as the program runs, so a write of one
 * anywhere is a write under a live loan - `checkLentGlobalWrites`. A global lending to itself is left
 * out, since every update of a record global reads the record it replaces.
 *
 * An **exclusive** loan is remembered as one: the lender is then reachable only through its holder,
 * so the borrow that made the loan is the one use of its name the check lets through - which is why
 * each loan's own site is kept. A second global holding a loan of a lender one of them holds
 * exclusively would be two live names for storage one of them was promised alone, so that is refused
 * here, at the second store.
 */
bool Analysis::storeIntoGlobal(Node& node, Place& global, U32 l) {
    SmallArray<U32, 4> roots;
    BitMatrix::zero(lendersSeen.pointer(), loanWords);

    auto clean = judgeLenders(l, [&](U32 m, Lender kind) {
        if(kind == Lender::OpaqueAddress) return true;

        if(kind == Lender::Program) {
            if(rootOf(loanAt(U32(m))->place)->kind == Place::Global) roots.push(m);
            return true;
        }

        auto holder = placeText(context, decl, base, body, global);
        reportRefusal(node.source, Holder::Global, refusalOf(kind), m, &holder);
        return false;
    });

    if(!clean) return false;
    if(!globalLending) return true;

    for(auto r: roots) {
        auto& root = *loanAt(U32(r));
        auto lender = rootOf(root.place)->payload;
        if(lender == global.payload) continue;

        auto exclusive = root.capability == Access::Unique;
        auto known = globalLending->lent.get(lender);

        if(known) {
            auto&& held = known.unwrap();
            if(held.holderGlobal != global.payload && (exclusive || held.exclusive)) {
                auto holder = placeText(context, decl, base, body, global);
                auto named = placeText(context, decl, base, body, *rootOf(root.place));
                if(held.exclusive) {
                    report(node.source,
                           "%@ cannot hold a loan of %@, because %@ already holds an exclusive loan of it for as long as the program runs - a global lent exclusively is reachable through its one holder"_v,
                           holder, named, held.holder);
                } else {
                    report(node.source,
                           "%@ cannot hold an exclusive loan of %@, because %@ already holds a loan of it for as long as the program runs - a global lent exclusively is reachable through its one holder"_v,
                           holder, named, held.holder);
                }
                return false;
            }
        }

        auto fresh = !known;
        auto& entry = globalLending->lent[lender];
        if(fresh) {
            entry.holder = placeText(context, decl, base, body, global);
            entry.holderGlobal = global.payload;
        }

        entry.exclusive |= exclusive;
        entry.stores.push(root.source);
    }

    return true;
}

/*
 * Which uses of a place operand **write** its root: a move out, an exclusive borrow (a borrow handed
 * to `push` is how most writes happen) and an `addressOf`, since nothing after one is visible here.
 * Every other use reads it. The summary, the lent-global rule and `computeStaticLiterals` ask this;
 * the teardown check does not, since what a teardown writes is the summary's answer.
 */
static bool writesThrough(const Node& node, const Operand& operand) {
    auto mode = operand.mode();
    return node.kind == Node::Address || mode == UseMode::Take || mode == UseMode::BorrowUnique;
}

/*
 * Which functions a function-typed operand may be a value of, where this body says - a `fun.value`
 * made here, or a local every write of which is one (`lazyOr(a, b)` parks its thunk in a local and
 * lends that). A parameter is `Parameter`: the caller handed it on. Anything else - a global, a
 * field, a call's result - is `Unknown`. `None` is `valueTargets`'s: an operand that is no value at
 * all.
 */
enum class FunSource { None, Known, Parameter, Unknown };

static FunSource functionValueTargets(MirBase base, Body& body, Operand operand,
                                      SmallArray<DeclPtr<Function>, 4>& into) {
    if(operand.source() != SourceKind::Result) return FunSource::Unknown;

    auto& producer = *body.node(base, operand.nodeId());
    if(producer.kind == Node::FunValue) {
        into.push(((NodeFunValue&)producer).callee);
        return FunSource::Known;
    }

    if(producer.kind != Node::Read) return FunSource::Unknown;

    auto& read = (NodeRead&)producer;
    if(read.place.source() != SourceKind::Place) return FunSource::Unknown;

    auto place = body.place(base, read.place.placeId());
    if(place->kind != Place::Local) return FunSource::Unknown;

    auto& local = *body.local(base, LocalId(place->payload));
    if(local.isParameter()) return FunSource::Parameter;

    auto found = false;
    for(U32 b = 0; b < body.blocks.size(); b++) {
        auto block = body.block(base, BlockId(b));

        for(U32 at = 0; at < block->nodes.size(); at++) {
            auto& node = *base[block->nodes.get(base, at)];
            if(node.kind != Node::Write) continue;

            auto& write = (NodeWrite&)node;
            if(write.destination.source() != SourceKind::Place) continue;

            auto written = body.place(base, write.destination.placeId());
            if(rootPlace(base, *written).kind != Place::Local ||
               rootPlace(base, *written).payload != place->payload) {
                continue;
            }

            if(written != place || write.value.source() != SourceKind::Result) return FunSource::Unknown;

            auto& value = *body.node(base, write.value.nodeId());
            if(value.kind != Node::FunValue) return FunSource::Unknown;

            into.push(((NodeFunValue&)value).callee);
            found = true;
        }
    }

    return found ? FunSource::Known : FunSource::Unknown;
}

static DeclPtr<Type> operandType(MirBase base, Body& body, Operand operand) {
    if(operand.source() != SourceKind::Result) return nullptr;
    return valueOccurrence(*body.node(base, operand.nodeId())).type;
}

// The code a lambda's environment record runs, where the type is one.
static DeclPtr<Function> lambdaOf(DeclBase decl, DeclPtr<Type> type) {
    if(!type || decl[type]->kind != Type::Record) return nullptr;
    return ((RecordType*)decl[type])->lambda;
}

/*
 * What a value may run where it is **called**, or **handed** to a call - the one classification the
 * summary, the confirming walk and the check site share; what `Parameter` and `Unknown` cost is each
 * one's policy, not this. `type` is the value's type in the asker's terms: the confirming walk
 * substitutes it first, which is how a variable becomes the lambda a specialization chose. A lambda's
 * record runs the lambda, a function value runs what `functionValueTargets` says, and anything else
 * handed to a call is no value (`None`). A value that is *called* is asked where it came from
 * whatever its type, since a variable or an erased iterator being called is a value too.
 *
 * It was three copies, and they had drifted: the summary's call through a value did not ask
 * `lambdaOf`, and the check site asked nothing of a called value not typed as a function - so `g()`
 * over a `Call`-constrained parameter, holding a window of a global across it, read the run the call
 * freed (J121).
 */
static FunSource valueTargets(DeclBase decl, MirBase base, Body& body, Operand operand, DeclPtr<Type> type,
                              bool called, SmallArray<DeclPtr<Function>, 4>& into) {
    if(auto lambda = lambdaOf(decl, type)) {
        into.push(lambda);
        return FunSource::Known;
    }

    if(!called && (!type || decl[type]->kind != Type::Fun)) return FunSource::None;
    return functionValueTargets(base, body, operand, into);
}

/*
 * What a call of a member of `Iterate` at a **site type** or an **erased** `iter(T)` runs, where the
 * call names the *class's* member.
 *
 * Since J125 a concrete `it.next()` or `it.resume(v)` names the object's own `f$next` or `f$resume`
 * (`Resolver::generatedMemberAt`), which `runsProducer` answers for. What still names the class's
 * member is a call inside a body generic in `Iterate`: in the summary and at the check site the
 * subject is then a variable and this answers `Other`, and in the confirming walk the call's
 * substituted types can make it a site type or the erased one. There the members are the
 * compiler's, one per producer, so the answer is the producer's body - and at the erased type
 * whatever step is in the pair, which is a call through a value. The member's own row would join
 * every implementation any instance gives it.
 */
enum class IterateCall { Other, Producer, Erased };

template<class Classes>
static IterateCall iterateCallOf(DeclBase decl, const Classes& iterate, DeclPtr<Function> callee,
                                 DeclPtr<Type> subject, DeclPtr<Function>& producer) {
    if(!iterate.plain || !callee || !subject) return IterateCall::Other;
    if(decl[callee]->specializes) callee = decl[callee]->specializes;
    auto klass = decl[callee]->klass;
    if(!klass || (klass != iterate.plain && klass != iterate.failing)) return IterateCall::Other;

    if(decl[subject]->kind == Type::Iter) return IterateCall::Erased;
    if(decl[subject]->kind != Type::Record) return IterateCall::Other;

    auto& record = *(RecordType*)decl[subject];
    if(!record.producer) return IterateCall::Other;

    producer = record.producer;
    return IterateCall::Producer;
}

static DeclPtr<Type> firstTypeArgument(MirBase base, MirList<DeclPtr<Type>>* typeArguments) {
    if(!typeArguments || !typeArguments->size()) return nullptr;
    return typeArguments->get(base, 0);
}

/*
 * What a direct call runs, in the terms of the rows: the teardowns of the types it hands the callee -
 * a generic callee may release a value of any of them (`Teardown`) - and then the callee, or
 * `iterateCallOf`'s answer for a class member of `Iterate`. The summary and the check site both ask
 * this; the confirming walk asks `iterateCallOf` itself, because it selects the instance the call's
 * substituted types choose where these two take the member's row. A concrete step call names the
 * object's own member and has no type arguments, so what that step releases is `objectParts`'s.
 */
template<class Classes, class OnFunction, class OnTeardown, class OnIndirect>
static void forEachCallRun(DeclBase decl, MirBase base, const Classes& iterate, DeclPtr<Function> callee,
                           MirList<DeclPtr<Type>>* typeArguments, OnFunction&& onFunction,
                           OnTeardown&& onTeardown, OnIndirect&& onIndirect) {
    if(typeArguments) {
        for(auto argument: typeArguments->contents(base)) {
            if(argument) onTeardown(argument);
        }
    }

    DeclPtr<Function> producer;
    switch(iterateCallOf(decl, iterate, callee, firstTypeArgument(base, typeArguments), producer)) {
        case IterateCall::Producer: return onFunction(producer);
        case IterateCall::Erased: return onIndirect();
        case IterateCall::Other: return onFunction(callee);
    }
}

/*
 * The type arguments of a **specialization**, whose teardowns its generic's row leaves out: a
 * generic body's row is one answer for every instantiation, so what it releases of its variables is
 * charged to whoever chose the types (`Teardown`). Whatever answers for a specialization *without*
 * such a caller asks them instead - `reachesGlobalState` of a specialization, and the members the
 * compiler writes for an object of a generic producer (`objectParts`), whose steps are called with
 * no type arguments at all. `f` answering true stops it.
 */
template<class F>
static bool forEachLeftToCaller(DeclBase decl, Function& function, F&& f) {
    if(!function.specializes) return false;
    for(auto argument: function.typeArguments.contents(decl)) {
        if(f(argument)) return true;
    }
    return false;
}

/*
 * Whether `function` is a member the compiler writes for an iterator object that runs the
 * producer's own body: its step, its teardown (the step entered with a cancel request), the two
 * consumer-facing steppers over it, and the erased form's shims. `resolve_ssa` makes their bodies
 * out of the producer's MIR, so none of them has one here, and what each runs is `objectParts`'s.
 */
static bool runsProducer(const Function& function) {
    if(!function.generatedFrom) return false;

    switch(function.generated) {
        case Function::Generated::IterStep:
        case Function::Generated::IterDrop:
        case Function::Generated::IterNext:
        case Function::Generated::IterResume:
        case Function::Generated::IterErasedStep:
        case Function::Generated::IterErasedRaw:
            return true;
        default:
            return false;
    }
}

/*
 * **What stepping or releasing an object runs** - J130. The producer's body at its types, and
 * besides it what the producer's lender would have been charged with: a call of the producer lends
 * it its arguments, and a function value or a lambda among them is charged to that caller (the
 * summary's `Parameter`, the check site's `hand`). An object's record *is* that caller - it holds the
 * captured arguments and lends them to every step - so the parts are:
 *
 * - `producer(generatedFrom)`, the producer's specialization;
 * - `teardown(argument)` for each of its type arguments (`forEachLeftToCaller`), since the step is
 *   called with none and a generic producer's row leaves them out - `holdThenDrop(->x: a)` releases
 *   `x` inside its second step;
 * - `lambda(f)` for each lambda record among the captures and the type arguments - a `Call`
 *   promise at a lambda's record runs the lambda, and a `Holder(f)` capture holds one where no
 *   capture's own type is it;
 * - `indirect()` for each capture that is a function value, which is a call through a value this
 *   side cannot say anything about.
 *
 * Before this, a lambda captured by `runs(() -> grow())` was charged nowhere: not at the
 * construction, which runs nothing, and not at `it.next()`, whose operand is the record. An
 * erased object was covered already - its pair is a function value (`IterObject`) - and a `for`
 * over the call hands the lambda to it directly. Each part answering true stops it.
 */
template<class Parts>
static bool objectParts(DeclBase decl, const Function& member, Parts& parts) {
    auto& producer = *decl[member.generatedFrom];
    if(parts.producer(member.generatedFrom)) return true;

    auto callable = [&](DeclPtr<Type> type) {
        if(!type) return false;
        if(decl[type]->kind == Type::Pointer) type = ((PointerType*)decl[type])->to;
        if(auto lambda = lambdaOf(decl, type)) return parts.lambda(lambda);
        return decl[type]->kind == Type::Fun && parts.indirect();
    };

    if(forEachLeftToCaller(decl, producer, [&](DeclPtr<Type> argument) {
        return parts.teardown(argument) || callable(argument);
    })) {
        return true;
    }

    auto record = member.generatedRecord;
    if(!record || decl[record]->kind != Type::Record) return false;

    auto& site = *(RecordType*)decl[record];
    for(auto field: site.fields.contents(decl)) {
        if(field.index < RecordType::kCaptureFirst || field.index >= site.payloadFirst) continue;
        if(callable(field.occurrence.type)) return true;
    }
    return false;
}

/*
 * **What one node runs**, as events - the one definition of a call edge, which the summary, the
 * confirming walk and the check site each read through a policy of their own. Every event is a
 * method the policy must have, so a node kind that runs code, added here, does not compile until
 * each of the three has said what it means to it: the holes J113 closed were each an edge one of
 * three hand-written copies of this switch did not have. A method answering true stops the walk,
 * which is how the confirming walk answers on its first "yes".
 *
 * - `release(type)`: an overwrite releases what the destination held - a global's, a borrow's or an
 *   element's included, which is exactly the storage `forEachReleased` does not name;
 * - `makeValue(node)`, `makeLambda(lambda)`: a function value, or a lambda's record, made here;
 * - `call(call)`: a call naming its callee, `genCallable`'s included;
 * - `yield(call)`: a call of the body's own continuation;
 * - `callValue(source, targets)`: any other call through a value, classified by `valueTargets`;
 * - `lensCall(call)`: a lens call, whose callee is null where it goes through a value;
 * - `hand(type, source, targets)`: each argument of either kind of call, which may be a value the
 *   callee runs, classified the same way.
 *
 * The classification is made here rather than by each policy, so a called value is asked where it
 * came from in one place (J121's reason for `valueTargets`). A policy's `typeOf(type)` is the type
 * it is asked at - the confirming walk substitutes it, which is how a variable becomes the lambda a
 * specialization chose.
 */
template<class Policy>
static bool visitRuns(DeclBase decl, MirBase base, Body& body, const Function& function, Node& node,
                      Policy& policy) {
    OperandList* arguments = nullptr;

    switch(node.kind) {
        case Node::Write: {
            auto& write = (NodeWrite&)node;
            if(write.mode == Destination::Init || write.destination.source() != SourceKind::Place) return false;
            return policy.release(body.place(base, write.destination.placeId())->occurrence.type);
        }

        case Node::FunValue:
            return policy.makeValue((NodeFunValue&)node);

        case Node::Aggregate: {
            auto lambda = lambdaOf(decl, ((NodeResult&)node).occurrence.type);
            return lambda && policy.makeLambda(lambda);
        }

        case Node::Call: {
            auto& call = (NodeCall&)node;
            if(!call.callsValue()) {
                if(policy.call(call)) return true;
            } else if(callsOwnContinuation(decl, base, body, function, call)) {
                if(policy.yield(call)) return true;
            } else {
                SmallArray<DeclPtr<Function>, 4> targets;
                auto type = policy.typeOf(call.targetType);
                if(policy.callValue(valueTargets(decl, base, body, call.target, type, true, targets), targets)) {
                    return true;
                }
            }
            arguments = &call.arguments;
            break;
        }

        default: {
            if(!isLensCall(node.kind)) return false;
            auto& call = (NodeLensCall&)node;
            if(policy.lensCall(call)) return true;
            arguments = &call.arguments;
            break;
        }
    }

    for(auto argument: arguments->contents(base)) {
        SmallArray<DeclPtr<Function>, 4> targets;
        auto type = policy.typeOf(operandType(base, body, argument));
        if(policy.hand(type, valueTargets(decl, base, body, argument, type, false, targets), targets)) return true;
    }
    return false;
}

/*
 * What a body may release besides what it overwrites: each owned local's type - a parameter it owns
 * is its to release too - and each place's under one. The drops are placed after the analysis, so
 * this is the placement's superset (see `Teardown`), shared by the summary and the confirming walk.
 * `release` answering true stops it.
 */
template<class Release>
static bool forEachReleased(MirBase base, Body& body, Release&& release) {
    for(U32 l = 0; l < body.locals.size(); l++) {
        auto& local = *body.local(base, LocalId(l));
        if(local.occurrence.contract.isOwned() && release(local.occurrence.type)) return true;
    }

    for(U32 p = 0; p < body.places.size(); p++) {
        auto& place = *body.place(base, PlaceId(p));
        if(!place.parent) continue;

        auto& root = rootPlace(base, place);
        if(root.kind != Place::Local) continue;
        if(!body.local(base, LocalId(root.payload))->occurrence.contract.isOwned()) continue;
        if(release(place.occurrence.type)) return true;
    }

    return false;
}

/*
 * **What releasing a value of `type` runs**, as parts - the one statement of a teardown's structure,
 * which the summary's teardown rows and the confirming walk read through a policy each: the authored
 * `Drop` and then `Reclaim` (`authored(instance, match)`, the instance null where none is declared),
 * the type's own arguments where one of those instances is parametric (`arguments(record)`, since
 * its generic member releases values of them), and an array's element and each field a derived
 * teardown walks (`part(type)`). A part answering true stops it.
 *
 * Only what lies under the caller's recursion guard is here. What comes before it - no teardown, a
 * function value's environment, an erased iterator, a variable - is three lines each caller reads
 * its own way. `summary.cpp`'s `reachTeardown` is a third reading and stays its own: it resolves
 * the member through `teardownMember`, which mints a specialization, and has none of those cases.
 */
template<class Parts>
static bool forEachTeardownPart(Context& context, Program& program, DeclPtr<Type> type,
                                const Ownership& ownership, Parts& parts) {
    auto decl = *program.decl;
    auto parametric = false;

    auto authored = [&](intrinsic::Known klass, TeardownKind teardown) {
        if(teardown != TeardownKind::Authored) return false;

        InstanceMatch match;
        auto instance = declaredInstanceFor(context, program, klass, type, &match);
        parametric = parametric || (instance && decl[instance]->gen);
        return parts.authored(instance, match);
    };

    if(authored(intrinsic::Known::drop, ownership.drop)) return true;
    if(authored(intrinsic::Known::reclaim, ownership.reclaim)) return true;

    auto kind = decl[type]->kind;
    if(kind == Type::Array) return parts.part(((ArrayType*)decl[type])->element);
    if(kind != Type::Record) return false;

    auto& record = *(RecordType*)decl[type];
    if(parametric && parts.arguments(record)) return true;
    if(ownership.drop == TeardownKind::Authored && ownership.reclaim == TeardownKind::Authored) return false;

    for(auto field: record.fields.contents(decl)) {
        if(tearsDownField(decl, field) && parts.part(field.occurrence.type)) return true;
    }

    return false;
}

Maybe<U32> GlobalEffects::rowOf(DeclPtr<Function> function) const {
    if(!function) return Nothing();
    auto decl = *program.decl;
    if(decl[function]->specializes) function = decl[function]->specializes;
    if(auto found = functionRow.get(function.offset)) return Just(U32(found.unwrap()));
    return Nothing();
}

/*
 * The row of a function a checked body names. The summary gives every function of the program one,
 * and every function a body names is one of them, so a miss is a function made after the summary -
 * which the loan check, running before any is made, never sees. `build-assert` stops on one; a
 * release build answers `anyRow`, which is "anything", rather than the nothing a skipped part said.
 */
static U32 checkedRow(const GlobalEffects& effects, DeclPtr<Function> function) {
    auto row = effects.rowOf(function);
    assertTrue(!function || row);
    return row ? row.unwrap() : effects.anyRow;
}

void GlobalEffects::ensure() {
    if(computed) return;
    computed = true;
    computeGlobalEffects(context, program, *this);
}

/*
 * Which globals a call may touch - the summary a loan of a global is checked against at a call.
 *
 * A loan of a global is an ordinary loan inside the body that takes it, and every access the body
 * makes itself is checked against it the ordinary way. What that never saw is the *callee*: `let w =
 * gArr` followed by a call to a function that pushes onto `gArr`, then `w[0]`, compiled in both
 * stages, and the push may have moved the run `w` points into. A global is the one storage every
 * function can name without being handed it, so a call is an access to every global the callee's
 * body - and every body it runs - reads or writes.
 *
 * **What counts.** A place operand rooted at a writable global (`writesThrough` says which uses
 * write it); an immutable global is never written after its initializer, so it has no bit. Then what
 * a body runs without naming a global, to a fixpoint: its direct calls; every implementation of a
 * class member it names, through the member's own row; the functions it makes values of (below);
 * the teardowns it may run (`Teardown`); the members the compiler writes for an iterator object
 * (their producer); and what the code a lens or an `iter fn` yields to may do (`continuationRow`).
 * Where nothing says what a value is - read out of a global or a field, a call's result - a call
 * through it reaches `indirectRow`, the join of every function the program makes a value of.
 *
 * **A superset, confirmed at the call.** A row is one answer for every instantiation of a generic
 * body, so it joins every implementation of each member the body calls at a variable - one user
 * `instance Eq(Weird)` that writes a global would make every generic `==` in the library a writer
 * of it, and `print("{n}")` of an `Int` a call that may change it. So a row is what makes the
 * common answer cheap - no loan of a global is live, or the row says the call touches nothing - and
 * a "yes" is confirmed by `GlobalEffects::confirmCall`, which reads the callee's body again at the
 * call's own type arguments: a member call selects the instance those types choose, a generic callee
 * is read at its arguments, a teardown is the one the substituted type has, and a function value is
 * charged where it is called or handed to a call rather than where it is made. The rejected
 * alternative was to make the rows themselves instantiation-aware - a row per specialization, or a
 * set of "members called at a variable" resolved at every concrete call inside the fixpoint - which
 * charges the precision to every program rather than to the ones with a loan to check.
 *
 * **Here and not in `computeSummaries`**, which is the pass that already walks the call graph, for
 * the pass-order reason `resolve.cpp` gives: the summaries read what this analysis concluded, so
 * they run after it, and the loan check needs this before it. The other rejected alternative was
 * refusing any loan of a writable global that is live across *any* call. That is one line and it
 * refuses `total(gArr)` for every callee that only reads, which is most of what a program does
 * with a global table.
 *
 * Global-grained rather than place-grained: a callee writing `g.count` conflicts with a loan of
 * `g.items`. A field-level answer needs the callee's places rebased into the caller's, and nothing
 * in `lib/` has asked for it.
 *
 * If holes in this keep turning up, the fallback is a language rule instead of an analysis: a
 * writable global cannot be borrowed beyond the expression that names it.
 */
namespace {

// A row before the fixpoint: the globals its own body names, and the rows it runs. `state` is
// `stateBit`'s - it names some global, or runs code it was lent or cannot see.
struct Direct {
    SmallArray<U32, 4> reads, writes, callees;
    bool indirect = false;
    bool state = false;
};

/*
 * **What a teardown runs**. A body releases what it owns, and an authored `Drop` or
 * `Reclaim` is a call nothing in the body wrote: a summary of the calls a body writes does not
 * see one, and a function whose local's teardown pushes onto `gArr` would read as touching
 * nothing. The drops are placed after this runs, so what is charged is every type the body
 * *may* release (`forEachReleased`, and each overwritten destination's).
 *
 * One row per dropped type, whose callees are what `reachTeardown` in builder.cpp would root:
 * the authored member, or the members of each field a derived teardown walks. An authored member
 * of a **parametric** instance - `Reclaim([a])`, `Reclaim(Run(a))` - is a generic body releasing
 * values of its variables, so the row also runs the teardowns of the type's own arguments:
 * `Array(Noisy)`'s row reaches `Noisy`'s `Drop`, which the generic body's row cannot.
 *
 * A **function value** releases its environment, which is not a call of its code: what runs is
 * the teardown of whatever some `fun.value` captured, so its row is the join of every captured
 * environment's (`closure`). An **erased iterator** releases its object, which is the step
 * entered with a cancel request - the producer's code after its `yield`s - so its row is every
 * producer the program erases (`erased`). Charging either with every function value's *code*
 * (`indirectRow`) refused 21 lines of `lib/Test` whose callees merely drop a closure.
 *
 * A **variable** releases nothing in the summary: the body that chose the type is charged, since
 * every call names its type arguments and is charged with their teardowns - `push(xs, Noisy {n:
 * 1})` may release a `Noisy`, `push(gArr, 4)` nothing. Charging a variable with every teardown in
 * the program instead made every generic body in `lib/` - `push`, `get` - a writer of whatever
 * global any `Drop` touched. A loan held *inside* the generic body is the other half, and the
 * check answers it with `anyTeardownRow`: the caller that chose the type never sees that loan.
 */
struct Teardown {
    SmallArray<U32, 4> callees;
    bool closure = false;
    bool erased = false;
};

// Rows named before they are numbered, in a callee list: an index into `teardowns`, which become
// rows once every function and region has one, and one into `regionDirect`, likewise.
constexpr U32 kTeardown = 0x80000000u;
constexpr U32 kRegion = 0x40000000u;
constexpr U32 kNoTeardown = ~0u;               // in `teardownIndex`: a type whose teardown runs nothing

// `computeGlobalEffects`'s rows as they are built, before any is numbered in the matrices.
struct RowBuilder {
    Context& context;
    Program& program;
    GlobalEffects& effects;
    DeclBase decl;
    MirBase base;

    Array<Direct> direct;
    Array<DeclPtr<Function>> functions;

    Array<U32> declaredTeardowns;              // rows: every authored `Drop` and `Reclaim` member
    Array<U32> capturedTeardowns;              // teardown indices, one per captured environment type
    Array<U32> erasedProducers;                // rows
    Array<Teardown> teardowns;
    HashMap<U32, U32> teardownIndex;           // a type's offset -> its teardown, or `kNoTeardown`
    SmallArray<DeclPtr<Type>, 16> reaching;

    Array<U32> addressTaken;                   // rows of every function the program makes a value of

    // See "The code a continuation runs" below.
    struct LensRegion { U32 lens, region; };
    struct OwnContinuation { U32 region, function; };
    Array<LensRegion> lensRegions;             // (lens row, region) for every lens call naming its callee
    Array<U32> anonymousRegions;               // regions of a lens called through a value
    Array<U32> objectified;                    // rows of producers some object steps
    Array<OwnContinuation> ownContinuations;

    // A region's row, one per (function row, region) that holds code, and one per body with regions
    // for what the body releases - numbered after every function's.
    Array<Direct> regionDirect;
    HashMap<U64, U32> regionIndex;             // (function row, region id) -> index in regionDirect

    // The row of a function, made on first sight - where `GlobalEffects::rowOf` only looks one up.
    U32 rowFor(DeclPtr<Function> function) {
        if(decl[function]->specializes) function = decl[function]->specializes;
        if(auto found = effects.functionRow.get(function.offset)) return found.unwrap();
        auto row = U32(functions.size());
        effects.functionRow[function.offset] = row;
        functions.push(function);
        return row;
    }

    // `forEachTeardownPart`'s reading for a row: each part's members, as callees of `collected`.
    struct Collect {
        RowBuilder& builder;
        Teardown& collected;

        bool authored(DeclPtr<Instance> instance, InstanceMatch&) {
            // The instance's own member - its generic body, where the head is parametric - and not
            // `teardownMember`, which would mint a specialization before any body is analysed.
            if(!instance) return false;
            if(auto member = builder.decl[instance]->members.get(builder.decl, 0)) {
                collected.callees.push(builder.rowFor(member));
            }
            return false;
        }

        bool arguments(RecordType& record) {
            for(auto argument: record.arguments.contents(builder.decl)) part(argument);
            return false;
        }

        bool part(DeclPtr<Type> type) {
            builder.collect(type, collected);
            return false;
        }
    };

    void collect(DeclPtr<Type> type, Teardown& collected) {
        if(!type) return;

        auto kind = decl[type]->kind;
        if(kind == Type::Variable) return;

        auto ownership = ownershipOf(decl, type);
        if(!ownership.needsTeardown()) return;
        if(kind == Type::Fun) { collected.closure = true; return; }
        if(kind == Type::Iter) { collected.erased = true; return; }

        for(auto entry: reaching) {
            if(entry == type) return;
        }

        reaching.push(type);
        PopOnExit<DeclPtr<Type>> leaving { reaching };

        Collect parts { *this, collected };
        forEachTeardownPart(context, program, type, ownership, parts);
    }

    // A type's teardown, or `kNoTeardown` - remembered either way, since a type that needs a
    // teardown reaching nothing is released as often as one that does.
    U32 teardownOf(DeclPtr<Type> type) {
        if(!type) return kNoTeardown;
        if(auto found = teardownIndex.get(type.offset)) return found.unwrap();

        Teardown collected;
        collect(type, collected);

        auto index = kNoTeardown;
        if(collected.closure || collected.erased || collected.callees.size()) {
            index = U32(teardowns.size());
            teardowns.push(static_cast<Teardown&&>(collected));
        }

        teardownIndex[type.offset] = index;
        return index;
    }

    void releases(Direct& from, DeclPtr<Type> type) {
        auto index = teardownOf(type);
        if(index != kNoTeardown) from.callees.push(kTeardown | index);
    }

    void calleeEdges(Direct& from, DeclPtr<Function> callee, MirList<DeclPtr<Type>>* typeArguments) {
        forEachCallRun(decl, base, effects.iterateClass, callee, typeArguments,
                       [&](DeclPtr<Function> function) { from.callees.push(rowFor(function)); },
                       [&](DeclPtr<Type> type) { releases(from, type); },
                       [&]() { from.indirect = true; });
    }

    // A value this body calls or hands on, by `valueTargets`: a value it was lent is charged to
    // whoever lent it, so a `Parameter` runs nothing here.
    void valueEdges(Direct& from, FunSource source, SmallArray<DeclPtr<Function>, 4>& targets) {
        if(source == FunSource::Unknown) from.indirect = true;
        for(auto target: targets) from.callees.push(rowFor(target));
    }

    // The row of a region of `row`'s body, made on first use; the body's own region has none.
    U32 regionOf(U32 row, ContinuationId region, U32 released) {
        auto key = (U64(row) << 32) | region.index;
        if(auto found = regionIndex.get(key)) return found.unwrap();

        auto index = U32(regionDirect.size());
        regionIndex[key] = index;
        regionDirect.push(Direct {});

        // It may run what the body releases, since a local declared inside it dies there.
        if(released != kNoTeardown) regionDirect[index].callees.push(kRegion | released);
        return index;
    }

    // `objectParts`' reading for a row: what stepping or releasing the object runs, as edges.
    struct ObjectEdges {
        RowBuilder& builder;
        Direct& from;

        bool producer(DeclPtr<Function> function) { from.callees.push(builder.rowFor(function)); return false; }
        bool teardown(DeclPtr<Type> type) { builder.releases(from, type); return false; }
        bool lambda(DeclPtr<Function> function) { return producer(function); }
        bool indirect() { from.indirect = true; return false; }
    };

    void walkBody(U32 row, Function& function);
};

// `visitRuns`'s reading for a row: everything a node may run, charged to `mine`.
struct RowRuns {
    RowBuilder& builder;
    Body& body;
    U32 row;
    U32 released;                              // the body's release row, where it has regions
    bool hasRegions;
    Direct& mine;
    SmallArray<U32, 4>& chain;

    bool release(DeclPtr<Type> type) {
        builder.releases(mine, type);
        return false;
    }

    /*
     * A body that makes a function value is charged with running it. The value runs where it is
     * called or handed to a call, and a stored or returned one is `Unknown` wherever it is read
     * back - so this is wider than it needs to be, and `confirmCall` charges the value at the call
     * instead. It stays in the row because the row is also the answer where `confirmCall` cannot
     * read a body at concrete types, and there the maker is the one place left that knows the value.
     */
    bool makeValue(NodeFunValue& value) {
        auto target = builder.rowFor(value.callee);
        builder.addressTaken.push(target);
        mine.callees.push(target);

        // What releasing the pair runs - see `Teardown`. A value of an erased step makes its
        // producer an erased object, which the generated-function switch says of the step itself.
        if(value.capturing && value.environment.source() == SourceKind::Result) {
            auto index = builder.teardownOf(valueOccurrence(*body.node(builder.base, value.environment.nodeId())).type);
            if(index != kNoTeardown) builder.capturedTeardowns.push(index);
        }
        return false;
    }

    // A lambda is the same statement. Its value is its environment record rather than a
    // `fun.value`, and a body compiled once calls it through the `Call` class it was lent it under,
    // so the record's construction is where the maker is.
    bool makeLambda(DeclPtr<Function> lambda) {
        auto target = builder.rowFor(lambda);
        builder.addressTaken.push(target);
        mine.callees.push(target);
        return false;
    }

    /*
     * A call through a value, a `Call` promise or a function witness may run code this body was
     * lent, which is charged to whoever lent it and so is not in the row. `reachesGlobalState` asks
     * about the callee alone and has no lender to charge, so for it such a body may reach anything:
     * `state`.
     */
    bool call(NodeCall& call) {
        auto decl = builder.decl;
        if(call.genCallable != NodeCall::kNoCallable ||
           (decl[call.callee]->klass && decl[decl[call.callee]->klass]->role == intrinsic::Known::call)) {
            mine.state = true;
        }

        builder.calleeEdges(mine, call.callee, &call.typeArguments);
        return false;
    }

    // Nothing in the function's row - see "The code a continuation runs" - but a region holding a
    // `yield` is what a lens called inside it yields to, so its row runs this function's
    // continuation.
    bool yield(NodeCall&) {
        mine.state = true;
        for(auto region: chain) builder.ownContinuations.push(RowBuilder::OwnContinuation { region, row });
        return false;
    }

    // The summary's `lambdaOf` narrowing is J121's: before it, a call through a site type read the
    // value as `Unknown` wherever it did not come from a `fun.value`.
    bool callValue(FunSource source, SmallArray<DeclPtr<Function>, 4>& targets) {
        mine.state = true;
        builder.valueEdges(mine, source, targets);
        return false;
    }

    bool lensCall(NodeLensCall& call) {
        if(!call.callee) mine.indirect = true;
        else builder.calleeEdges(mine, call.callee, &call.typeArguments);

        // The region this call opens is what the callee's `yield` runs.
        if(hasRegions && call.region.valid()) {
            auto opened = builder.regionOf(row, call.region, released);
            if(call.callee) builder.lensRegions.push(RowBuilder::LensRegion { builder.rowFor(call.callee), opened });
            else builder.anonymousRegions.push(opened);
        }
        return false;
    }

    bool hand(DeclPtr<Type>, FunSource source, SmallArray<DeclPtr<Function>, 4>& targets) {
        builder.valueEdges(mine, source, targets);
        return false;
    }

    DeclPtr<Type> typeOf(DeclPtr<Type> type) { return type; }
};

void RowBuilder::walkBody(U32 row, Function& function) {
    auto& body = *base[function.mir];
    auto hasRegions = body.regions.size() > 1;

    // What this body may release - see `Teardown`. With regions it is a row of its own, which the
    // function's row and each region's run, rather than a copy in each.
    Direct releasing;
    auto& into = hasRegions ? releasing : direct[row];
    forEachReleased(base, body, [&](DeclPtr<Type> type) { releases(into, type); return false; });

    auto released = kNoTeardown;
    if(releasing.callees.size()) {
        released = U32(regionDirect.size());
        regionDirect.push(static_cast<Direct&&>(releasing));
        direct[row].callees.push(kRegion | released);
    }

    // Whether any place of the body is a global's: most bodies name none, and then the operand
    // walk below - every operand of every node, each rooted by a parent walk - can find nothing.
    // An operand's place is an entry of this table, so this asks the same question of each one.
    auto namesGlobal = false;
    for(U32 p = 0; p < body.places.size() && !namesGlobal; p++) {
        namesGlobal = rootPlace(base, *body.place(base, PlaceId(p))).kind == Place::Global;
    }

    SmallArray<U32, 4> chain;
    for(U32 b = 0; b < body.blocks.size(); b++) {
        auto block = body.block(base, BlockId(b));

        // A block of a region runs inside every region enclosing it, so what it does goes to
        // each of their rows as well as the function's.
        chain.clear();
        if(hasRegions) {
            for(auto at = block->region; at.valid() && body.region(base, at)->parent.valid();
                at = body.region(base, at)->parent) {
                chain.push(regionOf(row, at, released));
            }
        }

        Direct scratch;
        auto& mine = chain.size() ? scratch : direct[row];
        RowRuns runs { *this, body, row, released, hasRegions, mine, chain };

        for(U32 at = 0; at < block->nodes.size(); at++) {
            auto& node = *base[block->nodes.get(base, at)];

            if(namesGlobal) forEachOperand(base, node, [&](Operand& operand, OperandRole) {
                if(operand.source() != SourceKind::Place) return;

                auto& root = rootPlace(base, *body.place(base, operand.placeId()));
                if(root.kind != Place::Global) return;

                mine.state = true;
                auto bit = effects.globalBit.get(root.payload);
                if(!bit) return;
                (writesThrough(node, operand) ? mine.writes : mine.reads).push(bit.unwrap());
            });

            visitRuns(decl, base, body, function, node, runs);
        }

        if(chain.size()) {
            auto join = [&](Direct& to) {
                for(auto bit: scratch.reads) to.reads.push(bit);
                for(auto bit: scratch.writes) to.writes.push(bit);
                for(auto callee: scratch.callees) to.callees.push(callee);
                to.indirect = to.indirect || scratch.indirect;
                to.state = to.state || scratch.state;
            };

            for(auto target: chain) join(regionDirect[target]);
            join(direct[row]);
        }
    }
}

} // namespace

void computeGlobalEffects(Context& context, Program& program, GlobalEffects& into) {
    auto decl = *program.decl;
    RowBuilder builder { context, program, into, decl, *program.mir };
    auto& direct = builder.direct;
    into.iterateClass.plain = program.roles.klass(intrinsic::Known::iterate);
    into.iterateClass.failing = program.roles.klass(intrinsic::Known::iterateFailing);

    for(auto modulePointer: program.modules) {
        for(auto globalPointer: decl[modulePointer]->globals.contents(decl)) {
            if(decl[globalPointer]->writable) into.globalBit[globalPointer.offset] = U32(into.globalBit.size());
        }
    }
    into.stateBit = U32(into.globalBit.size());

    for(auto modulePointer: program.modules) {
        for(auto functionPointer: decl[modulePointer]->functions.contents(decl)) {
            if(decl[functionPointer]->specializes) continue;
            builder.rowFor(functionPointer);
        }
    }

    /*
     * Every implementation of each class member, as an edge **from the member's own row** - so a
     * call naming the member reaches all of them through the one row, whichever side asks.
     * `confirmCall` is what narrows it to the instance a call's types select.
     */
    struct MemberEdge { U32 member, implementation; };
    Array<MemberEdge> memberEdges;

    auto dropClass = program.roles.klass(intrinsic::Known::drop);
    auto reclaimClass = program.roles.klass(intrinsic::Known::reclaim);

    for(auto modulePointer: program.modules) {
        for(auto instancePointer: decl[modulePointer]->instances.contents(decl)) {
            auto& instance = *decl[instancePointer];
            if(!instance.klass) continue;

            auto members = instance.members.contents(decl);
            for(U32 i = 0; i < members.size(); i++) {
                auto member = members[i];
                if(!member) continue;

                if(instance.klass == dropClass || instance.klass == reclaimClass) {
                    builder.declaredTeardowns.push(builder.rowFor(member));
                }

                auto declared = decl[instance.klass]->members.get(decl, i);
                if(declared && declared != member) {
                    memberEdges.push(MemberEdge { builder.rowFor(declared), builder.rowFor(member) });
                }
            }
        }
    }

    /*
     * **The code a continuation runs.** A lens's or an `iter fn`'s `yield` calls the region its
     * caller wrote, and a loan the callee holds across the `yield` is live while that region runs -
     * which neither side sees alone: the caller checks the region against its own loans, and the
     * callee cannot see the region. So each function called as a lens has a continuation row: the
     * join of every region any call of it opens (a region's row is its blocks' accesses and calls,
     * a nested region's included), asked at the `yield` (`checkGlobalAccessAcrossCall`). An
     * **object** of the producer yields to whoever holds the object, which is any code at all, so
     * a producer the program makes an object of reaches every row (`anyRow`).
     *
     * The function's own row leaves its continuation out. What runs during a call of a lens is its
     * body and the caller's region, and the region is the caller's own code - in the caller's row,
     * and checked against the caller's loans where it is written. Joining every region into the
     * lens's row would charge one caller with what another caller's region does.
     */
    for(U32 row = 0; row < builder.functions.size(); row++) {
        direct.push(Direct {});
        auto& function = *decl[builder.functions[row]];

        /*
         * **A body the compiler writes later**. An iterator object's members are bodies
         * `resolve_ssa` makes out of the producer's own MIR, so there is nothing here to walk and
         * what those that run the producer run is `objectParts`: the producer, its type arguments'
         * teardowns and what the record's captures may call. The erased members call through the
         * pair and the trampoline through a link, which is a call through a value. The
         * construction and the item accessors run none of the producer's code, and the maker of
         * an object for a witness (`IterObject`) makes a function value of the erased step - so
         * its producer joins what a call through a value may run, which is `fun.value`'s rule.
         * Any of them makes the producer an object, whose `yield` runs whatever its holder does;
         * an erased step makes it an erased one, which releasing an `iter(T)` may run.
         *
         * Every **other** function with no body touches nothing: an intrinsic is one operation over
         * its operands, an external function cannot name a global of this program (it can reach one
         * only through an address, and an `addressOf` is already a write where it is taken), and a
         * member of an instance the compiler **provides** (`provided.def`) has a body written by
         * `provideBodies` before this runs, which is walked below like any other - arithmetic,
         * comparison and `Show` over its own parameters, naming no global.
         */
        switch(function.generated) {
            case Function::Generated::IterStep:
            case Function::Generated::IterDrop:
            case Function::Generated::IterNext:
            case Function::Generated::IterResume:
            case Function::Generated::IterErasedStep:
            case Function::Generated::IterErasedRaw:
                if(runsProducer(function)) {
                    RowBuilder::ObjectEdges edges { builder, direct[row] };
                    objectParts(decl, function, edges);
                    builder.objectified.push(builder.rowFor(function.generatedFrom));

                    auto erased = function.generated == Function::Generated::IterErasedStep ||
                                  function.generated == Function::Generated::IterErasedRaw;
                    if(erased) builder.erasedProducers.push(builder.rowFor(function.generatedFrom));
                }
                break;

            case Function::Generated::IterErasedNext:
            case Function::Generated::IterErasedResume:
            case Function::Generated::IterWalk:
                direct[row].indirect = true;
                break;

            case Function::Generated::IterObject:
                if(function.generatedFrom) {
                    builder.addressTaken.push(builder.rowFor(function.generatedFrom));
                    builder.erasedProducers.push(builder.rowFor(function.generatedFrom));
                    builder.objectified.push(builder.rowFor(function.generatedFrom));
                }
                break;

            default:
                // A foreign function - no body, not an intrinsic, not a class member's
                // declaration - reaches whatever its own side keeps, which `reachesGlobalState`
                // has to count. The loan check does not: it cannot name a global of this program.
                if(!function.mir && function.intrinsic == intrinsic::Op::Count && !function.klass) {
                    direct[row].state = true;
                }
                break;
        }

        if(function.mir) builder.walkBody(row, function);
    }

    auto functionCount = U32(direct.size());
    for(auto edge: memberEdges) direct[edge.member].callees.push(edge.implementation);

    /*
     * The rows after the functions'. A call through a function value runs one of the functions some
     * body made a value of - a lambda, a `@lazy` thunk, a named function handed on - so it is one
     * more row, the join of every `fun.value` target in the program. `anyRow` is every function's,
     * which is what the holder of an iterator object may do between two steps.
     */
    auto newRow = [&]() { direct.push(Direct {}); return U32(direct.size() - 1); };

    into.indirectRow = newRow();
    for(auto target: builder.addressTaken) direct[into.indirectRow].callees.push(target);

    into.anyRow = newRow();
    for(U32 row = 0; row < functionCount; row++) direct[into.anyRow].callees.push(row);

    auto firstRegion = U32(direct.size());
    for(auto& region: builder.regionDirect) direct.push(static_cast<Direct&&>(region));

    into.anonymousContinuationRow = newRow();
    for(auto region: builder.anonymousRegions) {
        direct[into.anonymousContinuationRow].callees.push(firstRegion + region);
    }

    auto continuation = [&](U32 function) -> U32 {
        if(auto found = into.continuationRow.get(function)) return found.unwrap();
        auto row = newRow();
        into.continuationRow[function] = row;
        direct[row].callees.push(into.anonymousContinuationRow);
        return row;
    };

    for(auto opened: builder.lensRegions) direct[continuation(opened.lens)].callees.push(firstRegion + opened.region);

    // A lens called as a class member yields to what every call of the member opens: the member's
    // continuation, which is its regions and the anonymous ones. It is asked before any object or
    // own continuation adds to a row, and neither names a class's declaration, so nothing else is
    // in it.
    // The member's row is read out before `continuation` may insert into the map it lives in.
    for(auto edge: memberEdges) {
        auto found = into.continuationRow.get(edge.member);
        if(!found) continue;

        auto member = U32(found.unwrap());
        direct[continuation(edge.implementation)].callees.push(member);
    }

    for(auto producer: builder.objectified) direct[continuation(producer)].callees.push(into.anyRow);

    // The row first: `continuation` may add one, and `direct[...]` taken before it would be a
    // reference into the storage that reallocates - which the pair-list form of this loop was.
    for(auto own: builder.ownContinuations) {
        auto target = continuation(own.function);
        direct[firstRegion + own.region].callees.push(target);
    }

    // Then a row per dropped type, and `anyTeardownRow` - every teardown the program declares.
    auto firstTeardown = U32(direct.size());
    for(auto entry: builder.teardownIndex.entries()) {
        if(entry.value != kNoTeardown) into.teardownRow[entry.key] = firstTeardown + entry.value;
    }

    for(auto& teardown: builder.teardowns) {
        auto row = newRow();
        for(auto callee: teardown.callees) direct[row].callees.push(callee);
        if(teardown.closure) for(auto index: builder.capturedTeardowns) direct[row].callees.push(kTeardown | index);
        if(teardown.erased) for(auto producer: builder.erasedProducers) direct[row].callees.push(producer);
    }

    into.anyTeardownRow = newRow();
    for(auto member: builder.declaredTeardowns) direct[into.anyTeardownRow].callees.push(member);
    for(auto index: builder.capturedTeardowns) direct[into.anyTeardownRow].callees.push(kTeardown | index);
    for(auto producer: builder.erasedProducers) direct[into.anyTeardownRow].callees.push(producer);

    /*
     * Reads and writes are one matrix, a row's writes in the words after its reads, so that a join
     * over an edge is one union - with a handful of writable globals a row is a word or two, and two
     * calls per edge were mostly call.
     */
    auto rows = U32(direct.size());
    auto words = U32((into.globalBit.size() + 1 + 63) / 64);
    into.words = words;
    into.access.resetWords(rows, 2 * words);

    // The edges, as one flat list per row with duplicates removed - the fixpoint re-unions each.
    Array<U32> edgeStart, edges;
    IndexSet seen;
    seen.reset(rows);

    for(U32 row = 0; row < rows; row++) {
        auto& from = direct[row];
        for(auto bit: from.reads) BitMatrix::set(into.access.row(row), bit);
        for(auto bit: from.writes) BitMatrix::set(into.access.row(row) + words, bit);
        if(from.state) BitMatrix::set(into.access.row(row), into.stateBit);
        if(from.indirect && row != into.indirectRow) from.callees.push(into.indirectRow);

        edgeStart.push(U32(edges.size()));
        for(auto callee: from.callees) {
            if(callee & kTeardown) callee = firstTeardown + (callee & ~kTeardown);
            else if(callee & kRegion) callee = firstRegion + (callee & ~kRegion);
            if(callee == row || !seen.add(callee)) continue;
            edges.push(callee);
        }
        for(auto i = edgeStart[row]; i < edges.size(); i++) seen.set(edges[i], false);
    }
    edgeStart.push(U32(edges.size()));

    into.own.resetWords(rows, 2 * words);
    for(U32 row = 0; row < rows; row++) BitMatrix::copy(into.own.row(row), into.access.row(row), 2 * words);

    /*
     * The join over the edges, one strongly connected component at a time in the order Tarjan's
     * walk finishes them - every callee's component before its caller's - so an acyclic call is
     * joined once and only a cycle iterates. Sweeping every row in declaration order until nothing
     * changed took a pass per level of the deepest chain that ran against that order.
     */
    constexpr U32 kUnvisited = ~0u;
    Array<U32> order, low, stack, walk, next;
    IndexSet onStack;
    onStack.reset(rows);
    for(U32 row = 0; row < rows; row++) { order.push(kUnvisited); low.push(0); }
    U32 counter = 0;

    auto join = [&](U32 row) {
        auto changed = false;
        for(auto i = edgeStart[row]; i < edgeStart[row + 1]; i++) {
            changed |= BitMatrix::unionInto(into.access.row(row), into.access.row(edges[i]), 2 * words);
        }
        return changed;
    };

    auto enter = [&](U32 row) {
        order[row] = low[row] = counter++;
        stack.push(row);
        onStack.add(row);
        walk.push(row);
        next.push(edgeStart[row]);
    };

    for(U32 start = 0; start < rows && words; start++) {
        if(order[start] != kUnvisited) continue;
        enter(start);

        while(walk.size()) {
            auto row = walk[walk.size() - 1];
            auto& at = next[next.size() - 1];

            if(at < edgeStart[row + 1]) {
                auto callee = edges[at++];
                if(order[callee] == kUnvisited) enter(callee);
                else if(onStack.get(callee)) low[row] = min(low[row], order[callee]);
                continue;
            }

            walk.pop();
            next.pop();
            if(walk.size()) {
                auto caller = walk[walk.size() - 1];
                low[caller] = min(low[caller], low[row]);
            }

            if(low[row] != order[row]) continue;

            auto first = stack.size();
            do { first--; } while(stack[first] != row);

            for(auto i = first; i < stack.size(); i++) onStack.set(stack[i], false);

            if(first + 1 == stack.size()) {
                join(row);
            } else {
                for(auto changed = true; changed;) {
                    changed = false;
                    for(auto i = first; i < stack.size(); i++) changed |= join(stack[i]);
                }
            }

            while(stack.size() > first) stack.pop();
        }
    }
}

/*
 * A **specialization** answers from its generic's row, and that row leaves out the teardowns of the
 * generic's variables, which are charged to whoever chose the types (`Teardown`) - so a
 * `consume$Noisy` whose `Noisy` has a `Drop` storing through a pointer kept in a global read as
 * reaching nothing. Its own type arguments are asked here instead (`forEachLeftToCaller`, the rule an
 * object's steps read the same way through `objectParts`, whose own rows carry the edges). A type
 * with no teardown row is
 * one no walked body released, or one whose teardown runs nothing; the first is possible - a
 * specialization's arguments are substituted, so `inner(Array(a))` in `outer$Noisy` is
 * `Array(Noisy)`, which nothing named - so such a type is "yes" wherever it needs a teardown at all.
 * Before this the answer was right only because owning a value of a variable type takes an owning
 * or writable parameter (`mutates`, which the SSA asker reads first) or a class-constrained call
 * (`state`) - an argument nobody had written down.
 */
bool reachesGlobalState(Program& program, DeclPtr<Function> callee) {
    if(!program.globalEffects || !callee) return true;

    auto& effects = *program.globalEffects;
    effects.ensure();

    auto row = effects.rowOf(callee);
    if(!row || BitMatrix::test(effects.readsOf(row.unwrap()), effects.stateBit)) return true;

    auto decl = *program.decl;
    return forEachLeftToCaller(decl, *decl[callee], [&](DeclPtr<Type> argument) {
        if(!argument) return true;

        auto teardown = effects.teardownRow.get(argument.offset);
        return teardown ? BitMatrix::test(effects.readsOf(teardown.unwrap()), effects.stateBit)
                        : !isConcreteType(decl, argument) || ownershipOf(decl, argument).needsTeardown();
    });
}

void releaseGlobalEffects(GlobalEffects* effects) {
    delete effects;
}

bool GlobalEffects::touches(U32 row, U32 globalPayload, bool anyAccess) const {
    auto bit = globalBit.get(globalPayload);
    if(!bit) return false;

    if(BitMatrix::test(writesOf(row), bit.unwrap())) return true;
    return anyAccess && BitMatrix::test(readsOf(row), bit.unwrap());
}

/*
 * `confirmCall`'s walk: whether the code a row stands for touches the global at the types it is
 * run at, read off the bodies rather than the rows - see `computeGlobalEffects`.
 *
 * Every step asks the row first, so a body whose row says no is never read, and it charges exactly
 * what the row would have except where the types decide: a member call runs the instance its
 * substituted arguments select (the class's default where the instance leaves the member out), a
 * generic callee is read at its substituted arguments, a teardown is the one the substituted type
 * has (a parametric instance's member read at the match's bindings, which is how `Array(Noisy)`
 * reaches `Noisy`'s `Drop`), and a body releases only what it owns - so `peek(x: a)` at a `Noisy`
 * releases nothing, where the row charged every type argument's teardown. A function value is
 * charged where it is called or handed to a call, never where it is made, since a stored or
 * returned value is `Unknown` wherever it is read back. A `yield` adds nothing: the region it runs
 * is the caller's own code, read where the caller's body is.
 *
 * Where the types are not concrete - a body compiled once, a call from a generic body the check
 * itself is in - there is nothing to select, and the answer is the row's. A cycle answers "no" for
 * the member already being read, and a question that reads more than `kConfirmBudget` bodies
 * answers "yes"; an answer that leaned on either is not remembered, since the next question starts
 * with a fresh budget and must not read a "yes" that was only this one running out.
 */
namespace {

constexpr U32 kConfirmBudget = 4096;

enum ConfirmState: U8 { kAbsent, kConfirming, kYes, kNo };

struct ConfirmWalk {
    ConfirmWalk(GlobalEffects& effects, GlobalEffects::Question question):
        effects(effects), question(question), decl(*effects.program.decl), base(*effects.program.mir) {
        effects.budget = kConfirmBudget;
    }

    GlobalEffects& effects;
    GlobalEffects::Question question;
    DeclBase decl;
    MirBase base;

    bool row(U32 row) { return effects.touches(row, question.payload, question.anyAccess); }
    bool rowOf(DeclPtr<Function> function) { return row(checkedRow(effects, function)); }

    DeclPtr<Type> substituted(DeclPtr<Type> type, const Substitution& with) {
        return with.isEmpty() || !type ? type : substituteType(effects.context, effects.program, type, with);
    }

    static bool concrete(DeclBase decl, const DeclPtr<Type>* arguments, Size count) {
        for(Size i = 0; i < count; i++) {
            if(!arguments[i] || !isConcreteType(decl, arguments[i])) return false;
        }
        return true;
    }

    // The memo entry for a function at arguments, or for a type where `isType`.
    U32 entry(U32 what, bool isType, const DeclPtr<Type>* arguments, Size count) {
        auto key = (U64(what) * 0x9e3779b97f4a7c15ull) ^ (U64(question.payload) << 1) ^
                   U64(question.anyAccess) ^ (U64(isType) << 63);
        for(Size i = 0; i < count; i++) key = (key ^ arguments[i].offset) * 0x100000001b3ull;

        auto flags = U8(U8(question.anyAccess) | U8(isType << 1));
        auto first = effects.confirmedIndex.get(key);
        for(auto at = first ? first.unwrap() : ~0u; at != ~0u; at = effects.confirmed[at].next) {
            auto& found = effects.confirmed[at];
            if(found.what != what || found.payload != question.payload || found.flags != flags ||
               found.argumentCount != count) {
                continue;
            }

            auto same = true;
            for(Size i = 0; i < count && same; i++) {
                same = effects.confirmedArguments[found.argumentsAt + i] == arguments[i];
            }
            if(same) return at;
        }

        auto at = U32(effects.confirmed.size());
        effects.confirmed.push(GlobalEffects::Confirmed {
            what, question.payload, U32(effects.confirmedArguments.size()),
            first ? first.unwrap() : ~0u, U16(count), flags, kAbsent,
        });
        for(Size i = 0; i < count; i++) effects.confirmedArguments.push(arguments[i]);
        effects.confirmedIndex[key] = at;
        return at;
    }

    /*
     * Asks `compute` once per entry, remembering its answer only where it assumed nothing under it:
     * no cycle answered "no", and no step ran out of budget and answered "yes". Both are counted in
     * `cycleHits`, since both make an answer this question's rather than the entry's.
     */
    template<class F> bool memo(U32 at, F&& compute) {
        switch(effects.confirmed[at].state) {
            case kYes: return true;
            case kNo: return false;
            case kConfirming: effects.cycleHits++; return false;
            default: break;
        }

        if(!effects.budget) {
            effects.cycleHits++;
            return true;
        }
        effects.budget--;

        auto hits = effects.cycleHits;
        effects.confirmed[at].state = kConfirming;
        auto answer = compute();
        effects.confirmed[at].state = hits != effects.cycleHits ? kAbsent : answer ? kYes : kNo;
        return answer;
    }

    // `forEachTeardownPart`'s reading at concrete types: the member the match selects, read at its
    // bindings - which is what a parametric instance's arguments are, so they add nothing here.
    struct Parts {
        ConfirmWalk& walk;

        bool authored(DeclPtr<Instance> instance, InstanceMatch& match) {
            if(!instance) return true;

            auto decl = walk.decl;
            auto member = decl[instance]->members.get(decl, 0);
            if(!member) return true;
            if(!decl[instance]->gen) return walk.function(member, nullptr, 0);
            return walk.function(member, match.bindings.pointer(), match.bindings.size());
        }

        bool arguments(RecordType&) { return false; }
        bool part(DeclPtr<Type> type) { return walk.teardown(type); }
    };

    bool teardown(DeclPtr<Type> type) {
        if(!type) return true;
        if(!isConcreteType(decl, type)) return row(effects.anyTeardownRow);

        auto registered = effects.teardownRow.get(type.offset);
        if(registered && !row(registered.unwrap())) return false;

        auto kind = decl[type]->kind;
        auto ownership = ownershipOf(decl, type);
        if(!ownership.needsTeardown()) return false;
        if(kind == Type::Fun || kind == Type::Iter) {
            return registered ? true : row(effects.anyTeardownRow);
        }

        return memo(entry(type.offset, true, nullptr, 0), [&]() {
            Parts parts { *this };
            return forEachTeardownPart(effects.context, effects.program, type, ownership, parts);
        });
    }

    // A call of `callee` at `typeArguments`, in the terms `with` substitutes.
    bool call(DeclPtr<Function> callee, MirList<DeclPtr<Type>>* typeArguments, const Substitution& with) {
        if(!callee) return row(effects.indirectRow);

        SmallArray<DeclPtr<Type>, 4> arguments;
        if(typeArguments) {
            for(auto argument: typeArguments->contents(base)) arguments.push(substituted(argument, with));
        }

        auto settled = concrete(decl, arguments.pointer(), arguments.size());
        auto& target = *decl[decl[callee]->specializes ? decl[callee]->specializes : callee];

        DeclPtr<Function> producer;
        switch(iterateCallOf(decl, effects.iterateClass, callee, arguments.size() ? arguments[0] : nullptr, producer)) {
            case IterateCall::Producer: return function(producer, nullptr, 0);
            case IterateCall::Erased: return row(effects.indirectRow);
            case IterateCall::Other: break;
        }

        if(target.klass) {
            if(!settled || !arguments.size()) return rowOf(callee);

            // A `Call` at a lambda's record runs the lambda - the one instance that is not declared.
            if(decl[target.klass]->role == intrinsic::Known::call) {
                if(auto lambda = lambdaOf(decl, arguments[0])) return function(lambda, nullptr, 0);
            }

            InstanceMatch found;
            if(!selectInstance(effects.context, effects.program, target.klass, arguments.pointer(),
                               arguments.size(), kNullLocation, false, found)) {
                return rowOf(callee);
            }

            auto& instance = *decl[found.instance];
            if(auto written = instance.members.get(decl, target.classMember)) {
                if(!instance.gen) return function(written, nullptr, 0);
                return function(written, found.bindings.pointer(), found.bindings.size());
            }

            auto declared = decl[target.klass]->members.get(decl, target.classMember);
            return function(declared, found.arguments.pointer(), found.arguments.size());
        }

        // A callee with no body of its own may release a value of any type it is handed - the
        // intrinsics that move and destroy are how a generic body releases its variables.
        if(!target.mir) {
            for(auto argument: arguments) {
                if(teardown(argument)) return true;
            }
        }

        if(!settled) return function(callee, nullptr, 0);
        return function(callee, arguments.pointer(), arguments.size());
    }

    // What a value `valueTargets` classified may run: a value this body was lent is its lender's
    // business, and the lender is read where it is, so a `Parameter` runs nothing here.
    bool value(FunSource source, SmallArray<DeclPtr<Function>, 4>& targets) {
        switch(source) {
            case FunSource::None:
            case FunSource::Parameter: return false;
            case FunSource::Unknown: return row(effects.indirectRow);
            case FunSource::Known: break;
        }

        for(auto target: targets) {
            if(function(target, nullptr, 0)) return true;
        }
        return false;
    }

    bool function(DeclPtr<Function> callee, const DeclPtr<Type>* arguments, Size count) {
        if(!callee) return true;

        if(auto generic = decl[callee]->specializes) {
            auto own = decl[callee]->typeArguments.contents(decl);
            count = own.size();
            arguments = count ? own.pointerAt(0) : nullptr;
            callee = generic;
        }

        auto found = effects.rowOf(callee);
        assertTrue(found);
        if(!found) return true;

        /*
         * A generic body's row leaves out the teardowns of its variables, which its callers are
         * charged with - so at arguments, what they release is asked too. An argument with no row
         * asks `anyTeardownRow`, which is also what opens the body for a lambda among the arguments
         * whose code the row leaves out; `teardown(argument)` would be narrower and was not taken
         * for that reason (J121).
         */
        auto at = found.unwrap();
        if(!row(at)) {
            auto released = false;
            for(Size i = 0; i < count && !released; i++) {
                auto registered = arguments[i] ? effects.teardownRow.get(arguments[i].offset) : Nothing();
                released = registered ? row(registered.unwrap()) : row(effects.anyTeardownRow);
            }
            if(!released) return false;
        }

        auto bit = effects.globalBit.get(question.payload).unwrap();
        if(BitMatrix::test(effects.ownWritesOf(at), bit)) return true;
        if(question.anyAccess && BitMatrix::test(effects.ownReadsOf(at), bit)) return true;

        // An object's step or teardown has no body before the conversion, and what it runs is the
        // producer read at its own types - the precision `for` over the call has (J130).
        auto& function = *decl[callee];
        if(runsProducer(function)) {
            Object parts { *this };
            return objectParts(decl, function, parts);
        }

        if(!function.mir) return true;
        if(!concrete(decl, arguments, count)) count = 0;

        return memo(entry(callee.offset, false, arguments, count), [&]() {
            Substitution with { function.gen, (DeclPtr<Type>*)arguments, count };
            return body(function, *base[function.mir], with);
        });
    }

    // `objectParts`' reading here: the producer's specialization is read at its own arguments, which
    // also asks what it releases of them, so the teardowns add nothing.
    struct Object {
        ConfirmWalk& walk;

        bool producer(DeclPtr<Function> function) { return walk.function(function, nullptr, 0); }
        bool teardown(DeclPtr<Type>) { return false; }
        bool lambda(DeclPtr<Function> function) { return walk.function(function, nullptr, 0); }
        bool indirect() { return walk.row(walk.effects.indirectRow); }
    };

    bool body(Function& function, Body& body, const Substitution& with);
};

// `visitRuns`'s reading at the types `with` substitutes: the first thing a node may run that
// touches the global. A value is charged where it is called or handed to a call, not where it is
// made, and a `yield` runs the caller's own code - see `ConfirmWalk` - so those three events are no.
struct ConfirmRuns {
    ConfirmWalk& walk;
    Body& body;
    const Substitution& with;

    bool release(DeclPtr<Type> type) { return walk.teardown(walk.substituted(type, with)); }
    bool makeValue(NodeFunValue&) { return false; }
    bool makeLambda(DeclPtr<Function>) { return false; }
    bool yield(NodeCall&) { return false; }

    bool call(NodeCall& call) {
        if(call.genCallable == NodeCall::kNoCallable) return walk.call(call.callee, &call.typeArguments, with);

        // Typed by a requirement of this body's context, so the callee is re-derived at the
        // specialization; the row is the answer, and a lambda among this body's type arguments is
        // what a `Call` promise may be answered by.
        if(walk.rowOf(call.callee)) return true;
        for(Size i = 0; i < with.count; i++) {
            auto lambda = lambdaOf(walk.decl, with.arguments[i]);
            if(lambda && walk.function(lambda, nullptr, 0)) return true;
        }
        return false;
    }

    bool callValue(FunSource source, SmallArray<DeclPtr<Function>, 4>& targets) { return walk.value(source, targets); }
    bool lensCall(NodeLensCall& call) { return walk.call(call.callee, &call.typeArguments, with); }

    bool hand(DeclPtr<Type>, FunSource source, SmallArray<DeclPtr<Function>, 4>& targets) {
        return walk.value(source, targets);
    }

    DeclPtr<Type> typeOf(DeclPtr<Type> type) { return walk.substituted(type, with); }
};

bool ConfirmWalk::body(Function& function, Body& body, const Substitution& with) {
    if(forEachReleased(base, body, [&](DeclPtr<Type> type) { return teardown(substituted(type, with)); })) {
        return true;
    }

    ConfirmRuns runs { *this, body, with };
    for(U32 b = 0; b < body.blocks.size(); b++) {
        auto block = body.block(base, BlockId(b));

        for(U32 at = 0; at < block->nodes.size(); at++) {
            if(visitRuns(decl, base, body, function, *base[block->nodes.get(base, at)], runs)) return true;
        }
    }

    return false;
}

} // namespace

bool GlobalEffects::confirmCall(Question question, DeclPtr<Function> callee, MirList<DeclPtr<Type>>* typeArguments) {
    return ConfirmWalk { *this, question }.call(callee, typeArguments, Substitution {});
}

bool GlobalEffects::confirmFunction(Question question, DeclPtr<Function> function) {
    return ConfirmWalk { *this, question }.function(function, nullptr, 0);
}

bool GlobalEffects::confirmTeardown(Question question, DeclPtr<Type> type) {
    return ConfirmWalk { *this, question }.teardown(type);
}

const IndexSet& Analysis::globalLoanSet() {
    if(globalLoansKnown) return globalLoans;
    globalLoansKnown = true;
    globalLoans.reset(loanCount);

    for(U32 l = 0; l < bodyLoans; l++) {
        auto root = rootOf(loanAt(U32(l))->place);
        if(root->kind != Place::Global || !decl[DeclPtr<Global>(root->payload)]->writable) continue;
        globalLoans.add(l);
        anyGlobalLoan = true;
    }

    return globalLoans;
}

bool Analysis::globalLoanLive(const U64* live, const U64* refused) {
    auto& loans = globalLoanSet();
    if(!anyGlobalLoan) return false;

    for(U32 l = 0; l < loanCount; l++) {
        if(loans.get(l) && BitMatrix::test(live, l) && !(refused && BitMatrix::test(refused, l))) return true;
    }
    return false;
}

/*
 * The one live loan of a writable global that the code run at this point may conflict with - a
 * write of the global under a shared loan, any use of it under an exclusive one - which both checks
 * report. `touches(question)` asks the rows, and `confirm(question)` the bodies at their types
 * (`computeGlobalEffects`); the second is asked only where the first says yes.
 */
template<class Touches, class Confirm>
Maybe<U32> Analysis::conflictingGlobalLoan(const U64* live, const U64* refused, Touches&& touches,
                                           Confirm&& confirm) {
    auto& loans = globalLoanSet();

    for(U32 l = 0; l < loanCount; l++) {
        if(!loans.get(l) || !BitMatrix::test(live, l) || (refused && BitMatrix::test(refused, l))) continue;

        auto& loan = *loanAt(U32(l));
        GlobalEffects::Question question { rootOf(loan.place)->payload, loan.capability == Access::Unique };
        if(touches(question) && confirm(question)) return Just(l);
    }

    return Nothing();
}

Analysis::GlobalLoanText Analysis::globalLoanText(U32 l) {
    auto& loan = *loanAt(U32(l));
    auto root = rootOf(loan.place);
    auto& place = *body.place(base, loan.place);

    return GlobalLoanText {
        placeText(context, decl, base, body, place),
        &place == root ? ownedString("it", 2) : placeText(context, decl, base, body, *root),
    };
}

namespace {

/*
 * `visitRuns`'s reading at a check site: the parts of what one call may run, each a row and what
 * confirms it (`checkGlobalAccessAcrossCall`).
 *
 * A value this body was *lent* - a `@lazy` parameter handed on, a callback - is `Unknown` here
 * rather than the caller's business, because the loan being asked about is this body's:
 * `outer(@lazy x)` holding a loan and calling `inner(x)` is refused wherever any `@lazy` thunk in
 * the program writes the global, even where every call of `outer` passes a constant. The precise
 * alternative is a row per (function, parameter) joining what each call site passes, solved in the
 * same fixpoint; it waits for a program that needs it. That is the one policy this reading has that
 * the summary and the confirming walk do not; the other is a value handed on under a variable a
 * `Call` requirement of this body names, which `valueTargets` calls no value and which is lent all
 * the same (J121).
 */
struct CallParts {
    struct Part { U32 row; DeclPtr<Function> function; };

    GlobalEffects& effects;
    DeclBase decl;
    MirBase base;
    Body& body;
    Function& function;

    SmallArray<Part, 4> parts;
    DeclPtr<Function> callee = nullptr;
    MirList<DeclPtr<Type>>* typeArguments = nullptr;
    bool everything = false;                   // through a value nothing here says - `indirectRow`
    bool yielding = false;
    bool unconfirmed = false;                  // a function with no row - `checkedRow`

    void addRow(U32 row) { parts.push(Part { row, nullptr }); }
    void addIndirect() { addRow(effects.indirectRow); everything = true; }

    void addFunction(DeclPtr<Function> run, bool confirmed) {
        auto row = checkedRow(effects, run);
        unconfirmed = unconfirmed || row == effects.anyRow;
        parts.push(Part { row, confirmed ? run : nullptr });
    }

    void addValue(FunSource source, SmallArray<DeclPtr<Function>, 4>& targets) {
        if(source == FunSource::None) return;
        if(source != FunSource::Known) return addIndirect();
        for(auto target: targets) addFunction(target, true);
    }

    // What the callee may release of the types this call chooses - see `Teardown` - where a type
    // still mentioning a variable of this body may be any of them.
    void addCallee(DeclPtr<Function> run, MirList<DeclPtr<Type>>* arguments) {
        callee = run;
        typeArguments = arguments;
        forEachCallRun(decl, base, effects.iterateClass, run, arguments,
                       [&](DeclPtr<Function> target) { addFunction(target, false); },
                       [&](DeclPtr<Type> type) {
                           if(auto found = effects.teardownRow.get(type.offset)) addRow(found.unwrap());
                           if(!isConcreteType(decl, type)) addRow(effects.anyTeardownRow);
                       },
                       [&]() { addIndirect(); });
    }

    // Asked at a call node only - `checkGlobalAccessAtTeardowns` is the check for what is released.
    bool release(DeclPtr<Type>) { return false; }
    bool makeValue(NodeFunValue&) { return false; }
    bool makeLambda(DeclPtr<Function>) { return false; }

    bool call(NodeCall& call) {
        if(!constructedStep(call)) addCallee(call.callee, &call.typeArguments);
        return false;
    }

    /*
     * A step of an object this body **built and kept to itself** - `let &it = runs(() -> print(...))`
     * then `it.next()` - charged with what its constructions lent it, rather than with every function
     * value in the program (J132).
     *
     * An object's record holds the producer's captured arguments and lends them to each step
     * (`objectParts`), and the record type is one per producer, so the step's own row can only say
     * "a function value" of a `() -> {}` capture and joins `indirectRow`: any function value in the
     * program writing the global refused the step (J130 §8's open item, `Q1`). A `for` over the same
     * call is precise, because there the value is an argument of the call being judged. This makes
     * the step the same: where every value the local can hold is one this body constructed, the
     * capture is one of those constructions' arguments, and each is classified the way a `for`'s
     * would be (`valueTargets`); a value that says nothing is still `indirectRow`.
     *
     * The local must be one nothing else can refill: not a parameter or a capture, every write a
     * whole store of a construction of the same producer, and every exclusive loan of it the subject
     * of one of the producer's own steps - a step writes the payload and never the captures. A
     * unique loan handed anywhere else (`reset(&it)`), a partial write or a store of a value from
     * elsewhere keeps the step's own row. The confirming walk still reads the member (`callee`), which
     * joins the indirect row; it is asked only once a precise part already touches the global.
     */
    bool constructedStep(NodeCall& call) {
        auto& member = *decl[call.callee];
        auto kind = member.generated;
        if(kind != Function::Generated::IterNext && kind != Function::Generated::IterResume &&
           kind != Function::Generated::IterStep) {
            return false;
        }
        if(!member.generatedFrom || !call.arguments.size()) return false;

        auto subject = call.arguments.get(base, 0);
        if(subject.source() != SourceKind::Result) return false;

        auto read = body.node(base, subject.nodeId());
        if(read->kind != Node::Read || ((NodeRead*)read)->place.source() != SourceKind::Place) return false;

        auto place = body.place(base, ((NodeRead*)read)->place.placeId());
        if(place->parent || place->kind != Place::Local) return false;

        auto local = LocalId(place->payload);
        auto& held = *body.local(base, local);
        if(held.isParameter() || held.environment) return false;

        auto rootedHere = [&](Operand operand) {
            if(operand.source() != SourceKind::Place) return false;
            auto p = body.place(base, operand.placeId());
            while(p->parent) p = base[p->parent];
            return p->kind == Place::Local && LocalId(p->payload) == local;
        };

        auto sameProducer = [&](Node& node, Function::Generated wanted) {
            if(node.kind != Node::Call || ((NodeCall&)node).callsValue()) return false;
            auto callee = ((NodeCall&)node).callee;
            return callee && decl[callee]->generatedFrom == member.generatedFrom &&
                   (wanted == Function::Generated::None ? runsProducer(*decl[callee])
                                                        : decl[callee]->generated == wanted);
        };

        // The exclusive loans of the local, and the constructions stored into it.
        SmallArray<NodeId, 8> unique;
        SmallArray<NodeCall*, 2> built;

        for(U32 i = 0; i < body.nodes.size(); i++) {
            auto& node = *body.node(base, NodeId(i));

            if(node.kind == Node::Read) {
                auto& reading = (NodeRead&)node;
                if(!rootedHere(reading.place)) continue;
                if(reading.loan.valid() && body.loan(base, reading.loan)->capability == Access::Unique) {
                    if(body.place(base, reading.place.placeId()) != place) return false;
                    unique.push(NodeId(i));
                }
                continue;
            }

            if(node.kind == Node::Write) {
                auto& write = (NodeWrite&)node;
                if(rootedHere(write.destination)) {
                    if(body.place(base, write.destination.placeId()) != place) return false;
                    if(write.value.source() != SourceKind::Result) return false;

                    auto& value = *body.node(base, write.value.nodeId());
                    if(!sameProducer(value, Function::Generated::IterNew)) return false;
                    built.push(&(NodeCall&)value);
                }
                if(rootedHere(write.value)) return false;
                continue;
            }

            auto elsewhere = false;
            forEachOperand(base, node, [&](Operand& operand, OperandRole) {
                if(rootedHere(operand)) elsewhere = true;
            });
            if(elsewhere) return false;
        }

        if(!built.size()) return false;

        // Every exclusive loan is the subject of one of the producer's own steps, and nothing else.
        for(U32 i = 0; i < body.nodes.size(); i++) {
            auto& node = *body.node(base, NodeId(i));
            auto first = true;
            auto stray = false;

            forEachOperand(base, node, [&](Operand& operand, OperandRole) {
                auto subjectHere = first;
                first = false;
                if(operand.source() != SourceKind::Result) return;

                for(auto loan: unique) {
                    if(operand.nodeId() != loan) continue;
                    if(!subjectHere || !sameProducer(node, Function::Generated::None)) stray = true;
                }
            });
            if(stray) return false;
        }

        // What stepping runs, less the capture this body can name: the parts of `objectParts`, and
        // for a function-value capture the values the constructions were handed.
        struct Parts {
            CallParts& runs;
            bool captured = false;

            bool producer(DeclPtr<Function> run) { runs.addFunction(run, false); return false; }
            bool teardown(DeclPtr<Type> type) {
                if(auto found = runs.effects.teardownRow.get(type.offset)) runs.addRow(found.unwrap());
                if(!isConcreteType(runs.decl, type)) runs.addRow(runs.effects.anyTeardownRow);
                return false;
            }
            bool lambda(DeclPtr<Function> run) { runs.addFunction(run, true); return false; }
            bool indirect() { captured = true; return false; }
        } parts { *this };

        objectParts(decl, member, parts);

        if(parts.captured) {
            for(auto construction: built) {
                for(auto argument: construction->arguments.contents(base)) {
                    auto type = operandType(base, body, argument);
                    if(!type || decl[type]->kind != Type::Fun) continue;

                    SmallArray<DeclPtr<Function>, 4> targets;
                    addValue(valueTargets(decl, base, body, argument, type, false, targets), targets);
                }
            }
        }

        callee = call.callee;
        typeArguments = &call.typeArguments;
        return true;
    }

    bool yield(NodeCall&) {
        auto row = effects.anonymousContinuationRow;
        if(auto own = effects.rowOf(DeclPtr<Function>(&function - decl))) {
            if(auto found = effects.continuationRow.get(own.unwrap())) row = found.unwrap();
        }
        addRow(row);
        yielding = true;
        return false;
    }

    bool callValue(FunSource source, SmallArray<DeclPtr<Function>, 4>& targets) {
        addValue(source, targets);
        return false;
    }

    bool lensCall(NodeLensCall& call) {
        if(!call.callee) addIndirect();
        else addCallee(call.callee, &call.typeArguments);
        return false;
    }

    bool hand(DeclPtr<Type> type, FunSource source, SmallArray<DeclPtr<Function>, 4>& targets) {
        if(source == FunSource::None && callableVariable(type)) source = FunSource::Parameter;
        addValue(source, targets);
        return false;
    }

    DeclPtr<Type> typeOf(DeclPtr<Type> type) { return type; }

    // Whether `type` is a variable some `Call` requirement of this body's context names.
    bool callableVariable(DeclPtr<Type> type) {
        if(!type || decl[type]->kind != Type::Variable || !function.gen) return false;

        for(auto requirement: decl[function.gen]->classes.contents(decl)) {
            if(!requirement.klass || decl[requirement.klass]->role != intrinsic::Known::call) continue;
            if(requirement.args.size() && requirement.args.get(decl, 0) == type) return true;
        }
        return false;
    }
};

} // namespace

/*
 * A loan of a global, live across a call that may write the global - or, for an exclusive loan, may
 * so much as read it. `GlobalEffects` is the argument for what a call touches, and `CallParts` for
 * what this call runs.
 *
 * The loans asked are the ones live on *entry* to the call, which is the ones held across it and the
 * ones its arguments are - `f(gArr)` over `fn f(xs: '[Int]): grow(); xs[0]` is the same hole one
 * frame down, and the only frame that can see both halves of it is this one. A loan whose place is
 * rooted at the global directly is the one asked: a binding that reborrows it keeps that loan live
 * for as long as it is used, so a window of a window is asked through its root.
 *
 * A **`yield`** is asked against what the code it yields to may do (`continuationRow`): a loan this
 * body holds across it is live while the caller's region runs, and only this frame holds it.
 */
void Analysis::checkGlobalAccessAcrossCall(Node& node, const U64* live, const U64* refused) {
    if(!globalEffects || !loanCount || (node.kind != Node::Call && !isLensCall(node.kind))) return;
    if(node.source == reportedGlobalAccess) return;

    // Nothing is computed until a loan of a writable global is live across a call.
    if(!globalLoanLive(live, refused)) return;

    globalEffects->ensure();
    auto& effects = *globalEffects;

    CallParts runs { effects, decl, base, body, function };
    visitRuns(decl, base, body, function, node, runs);
    if(!runs.parts.size()) return;

    auto found = conflictingGlobalLoan(live, refused,
        [&](GlobalEffects::Question question) {
            for(auto& part: runs.parts) {
                if(effects.touches(part.row, question.payload, question.anyAccess)) return true;
            }
            return false;
        },
        [&](GlobalEffects::Question question) {
            if(runs.everything || runs.yielding || runs.unconfirmed) return true;
            if(runs.callee && effects.confirmCall(question, runs.callee, runs.typeArguments)) return true;

            for(auto& part: runs.parts) {
                if(part.function && effects.touches(part.row, question.payload, question.anyAccess) &&
                   effects.confirmFunction(question, part.function)) {
                    return true;
                }
            }
            return false;
        });
    if(!found) return;

    auto text = globalLoanText(found.unwrap());
    auto exclusive = loanAt(U32(found.unwrap()))->capability == Access::Unique;
    reportedGlobalAccess = node.source;

    if(runs.yielding) {
        report(node.source,
               "%@ is borrowed across this yield, and the code it yields to may change %@ - end the borrow before the yield, or copy what it reads"_v,
               text.named, text.global);
    } else if(runs.everything) {
        report(node.source,
               "%@ is borrowed across this call, which goes through a function value and may change %@ - end the borrow before the call, or copy what it reads"_v,
               text.named, text.global);
    } else if(exclusive) {
        report(node.source,
               "%@ is borrowed exclusively across this call, and the function called may use %@ - end the borrow before the call"_v,
               text.named, text.global);
    } else {
        report(node.source,
               "%@ is borrowed across this call, and the function called may change %@ - end the borrow before the call, or copy what it reads"_v,
               text.named, text.global);
    }
}

/*
 * Whether a local is a **temporary** - an unnamed local this frame owns, which is what a value with no
 * storage of its own is given when something borrows it: `hold` for an owner handed to a borrowing
 * parameter, and `adaptToView` for a fixed array or any other donor a window is derived from.
 *
 * Its extent is an ordinary local's - it lives as long as the loans taken of it, inside the frame -
 * so nothing about it is refused that a named local's loan would not be. What is different is the
 * sentence: `local3` is the compiler's name and not the author's, and the fix is to change what the
 * value *is* rather than which binding lends it.
 */
bool Analysis::temporaryLocal(const Local& local) {
    return !local.name && !local.isParameter() && local.occurrence.contract.isOwned();
}

/*
 * A loan of a **placed string or array literal** - static bytes, which live as long as the program.
 *
 * `"abc"` at a `'String` position is a `const String` whose run is bytes the compiler placed, parked
 * by `holdBorrowed` in a temporary so that the view has something to be a loan of. The view is the
 * run and not the temporary: a `String` or an `Array` is a record whose shared form is a window of
 * its run, so what the window points at is the placed bytes whatever happens to the slot. So a loan
 * of such a temporary satisfies every destination a global's does - a global's field, a result, a
 * `&` parameter's field - rather than being refused as the frame's.
 *
 * Exactly the locals that can say so (`computeStaticLiterals`), and only a shared loan of the whole
 * local, which is the only loan `holdBorrowed` makes of one. `classifyLender` asks it of a temporary
 * and of a named owned local.
 *
 * **A named binding of the literal is the same bytes** (J132). `let s = "hello"` is a `write.init`
 * of the same `const` into a local the author named, and `ownership.md`'s "`let w = gArr` stores as
 * `gArr` does" reads as covering it: `gText = {gText | held: "hello"}` was accepted and
 * `gText = {gText | held: s}` refused as this function's storage, as was `return s` under
 * `-> a'String`. The window is the run whatever the binding does afterwards - a move hands the same
 * run to an owner whose `ownsHeap` is zero, so nothing frees it, and a write *through* the binding,
 * the one thing that could re-point what a later window is of, disqualifies it below.
 *
 * The rejected alternative was placing the literal's *view* as a constant, so that no slot exists
 * and the derivation borrows a `Place::Global`. That changes what every string argument lowers to,
 * for a question only the conformance checks ask.
 */
bool Analysis::staticLiteralLoan(const Loan& loan, Place& root) {
    if(loan.capability != Access::Shared || body.place(base, loan.place) != &root) return false;

    if(!staticLiteralsKnown) computeStaticLiterals();
    return staticLiterals.get(root.payload);
}

/*
 * `staticLiteralLoan`'s set, in one pass over the body: every temporary (`temporaryLocal`) or named
 * owned local written exactly once, whole, by an `Init` of a `const`, and never written through afterwards
 * (`writesThrough`) - a `push` onto it would leave the window where it was, but a window taken after
 * one would be of the new run - and of those, the ones whose type is a record with a shared view: a
 * fixed array's window is its own slot and is not this.
 */
void Analysis::computeStaticLiterals() {
    staticLiteralsKnown = true;

    auto locals = body.locals.size();
    IndexSet filled, spoiled;
    filled.reset(locals);
    spoiled.reset(locals);
    staticLiterals.reset(locals);

    for(U32 b = 0; b < blockCount; b++) {
        auto nodes = this->block(b)->nodes.size();

        for(U32 at = 0; at < nodes; at++) {
            auto& node = nodeAt(b, at);
            Operand* initialized = nullptr;

            if(node.kind == Node::Write) {
                auto& write = (NodeWrite&)node;

                if(write.destination.source() == SourceKind::Place) {
                    auto destination = write.destination.placeId();
                    auto root = rootOf(destination);

                    if(root->kind == Place::Local) {
                        auto whole = body.place(base, destination) == root;
                        auto constant = write.value.source() == SourceKind::Result &&
                                        body.node(base, write.value.nodeId())->kind == Node::Const;
                        auto once = whole && write.mode == Destination::Init && constant;

                        if(!once || !filled.add(root->payload)) spoiled.add(root->payload);
                        initialized = &write.destination;
                    }
                }
            }

            forEachOperand(base, node, [&](Operand& operand, OperandRole) {
                if(&operand == initialized || operand.source() != SourceKind::Place) return;

                auto named = rootOf(operand.placeId());
                if(named->kind == Place::Local && writesThrough(node, operand)) {
                    spoiled.add(named->payload);
                }
            });
        }
    }

    for(U32 i = 0; i < locals; i++) {
        if(!filled.get(i) || spoiled.get(i)) continue;

        auto& local = *body.local(base, LocalId(i));
        auto named = local.name && !local.isParameter() && !local.environment &&
                     local.occurrence.contract.isOwned();
        if(!named && !temporaryLocal(local)) continue;

        auto type = local.occurrence.type;
        if(type && decl[type]->kind == Type::Record && hasViewFor(decl, program, type, Access::Shared)) {
            staticLiterals.add(i);
        }
    }
}

/*
 * The call-bounded borrowing locals a reborrow is followed through - `classifyLender`'s second half
 * of `Reborrow`, in one pass over the body the first time a loan of one is classified.
 *
 * A local qualifies when it is written at least once, whole, and every whole write stores an
 * **exact** value: one whose origins name what it refers to. A write into a place *inside* a
 * borrowing local goes through it and does not re-point it, so only whole writes are asked. Three
 * values are exact:
 *
 *  - a borrow a `Read` minted - the loan of the place and, by the read rule, the place's own origins;
 *  - a value whose root a signature published, `Origin::Slot`, which that signature's own
 *    conformance check held the producer to;
 *  - a block parameter every edge into which supplies an exact value - `let w = if c then gA else gB`.
 *
 * What is left out is the value only a call bounds: a lens's hand-over arrives at the continuation's
 * parameter on an edge that supplies no argument at all (`Builder::addLensCall`), and what the origin
 * walk puts there is every argument of the call - the storage that must outlive the continuation,
 * not the storage the value is in. `classifyLender` says what following one admitted.
 *
 * The block parameters are the greatest fixpoint: each starts exact and is cleared by an edge that
 * supplies something else, until nothing moves. Inexactness enters only at a real source, so a loop
 * carrying an exact borrow round its back edge stays exact.
 */
void Analysis::computeExactBorrows() {
    exactBorrowsKnown = true;

    auto nodes = body.nodes.size();
    exactValues.reset(nodes);

    for(U32 i = 0; i < nodes; i++) {
        auto& node = *body.node(base, NodeId(i));
        if(!hasResult(node.kind)) continue;

        auto published = valueOccurrence(node).contract.origin() == Origin::Slot;
        auto minted = node.kind == Node::Read && ((NodeRead&)node).loan.valid();
        if(published || minted || node.kind == Node::Argument) exactValues.add(i);
    }

    for(auto changed = true; changed;) {
        changed = false;

        for(U32 b = 0; b < blockCount; b++) {
            auto block = this->block(b);
            auto count = block->nodes.size();
            if(!count) continue;

            forEachEdge(base, *base[block->nodes.get(base, count - 1)], [&](Edge& edge) {
                if(!edge.target) return;

                auto target = base[edge.target];
                auto supplied = edge.arguments.contents(base);

                for(U32 i = 0; i < target->arguments.size(); i++) {
                    auto& parameter = *base[target->arguments.get(base, i)];
                    auto at = parameter.id.index;
                    if(!exactValues.get(at)) continue;
                    if(valueOccurrence(parameter).contract.origin() == Origin::Slot) continue;

                    auto exact = i < supplied.size() &&
                                 supplied.pointerAt(i)->source() == SourceKind::Result &&
                                 exactValues.get(supplied.pointerAt(i)->nodeId().index);

                    if(!exact) {
                        exactValues.set(at, false);
                        changed = true;
                    }
                }
            });
        }
    }

    auto locals = body.locals.size();
    IndexSet spoiled;
    spoiled.reset(locals);
    exactBorrows.reset(locals);

    for(U32 i = 0; i < nodes; i++) {
        auto& node = *body.node(base, NodeId(i));
        if(node.kind != Node::Write) continue;

        auto& write = (NodeWrite&)node;
        if(write.destination.source() != SourceKind::Place) continue;

        auto place = body.place(base, write.destination.placeId());
        if(place->parent || place->kind != Place::Local) continue;

        auto exact = write.value.source() == SourceKind::Result &&
                     exactValues.get(write.value.nodeId().index);
        if(exact) exactBorrows.add(place->payload);
        else spoiled.add(place->payload);
    }

    for(U32 i = 0; i < locals; i++) {
        if(spoiled.get(i)) exactBorrows.set(i, false);
    }
}

/*
 * A loan of a global, live where a **teardown** runs that may write the global - or, for an
 * exclusive loan, may so much as read it.
 *
 * The same question `checkGlobalAccessAcrossCall` asks at a call, at the one call no node writes: a
 * value with an authored `Drop` or `Reclaim` dies at its last use, and `drop(a)` pushing onto
 * `gArr` while `let w = gArr` is still read is `readAfter(gArr)` again, with the call in the drop
 * list rather than in the body. Asked after `placeDrops`, which is what decides where the teardown
 * runs, against the loans live on entry to the node it is placed before - the loans that node and
 * everything after it still use. What it runs is `GlobalEffects::teardownRow`, keyed by the type
 * the placement releases. A type that still mentions a **variable** of this body - a generic body
 * is analysed once, before any caller chose the type - may be any teardown in the program
 * (`anyTeardownRow`): the caller that chose it is charged with its teardown, but it cannot see a loan
 * held in here.
 */
void Analysis::checkGlobalAccessAtTeardowns() {
    if(!globalEffects || !loanCount || errors) return;

    auto drops = out.drops.contents(base);
    if(!drops.size()) return;

    globalLoanSet();
    if(!anyGlobalLoan) return;

    // The placed drops by block, once, rather than a scan of all of them per block.
    Array<U32> dropStart, byBlock;
    for(U32 index = 0; index <= blockCount; index++) dropStart.push(0);
    for(auto& step: drops) dropStart[step.block + 1]++;
    for(U32 index = 0; index < blockCount; index++) dropStart[index + 1] += dropStart[index];
    for(Size i = 0; i < drops.size(); i++) byBlock.push(0);
    {
        Array<U32> fill;
        for(U32 index = 0; index < blockCount; index++) fill.push(dropStart[index]);
        for(U32 i = 0; i < drops.size(); i++) byBlock[fill[drops.begin()[i].block]++] = i;
    }

    SmallArray<U64, 4> live;
    for(U32 w = 0; w < loanWords; w++) live.push(0);

    BitMatrix before;
    OriginCursor backward { *this };

    for(U32 index = 0; index < blockCount; index++) {
        if(dropStart[index] == dropStart[index + 1]) continue;

        // The live set before each position, by the same backward transfer `checkLoans` steps.
        auto count = U32(this->block(index)->nodes.size());
        before.resetWords(count + 1, loanWords);

        BitMatrix::copy(live.pointer(), loansAt(loanOut, index), loanWords);
        BitMatrix::copy(before.row(count), live.pointer(), loanWords);
        backward.enterAtExit(index);

        for(U32 n = count; n > 0; n--) {
            auto position = n - 1;
            backward.undo(position);
            stepLoansBackward(live.pointer(), backward, nodeAt(index, position));
            BitMatrix::copy(before.row(position), live.pointer(), loanWords);
        }

        for(auto i = dropStart[index]; i < dropStart[index + 1]; i++) {
            auto& step = drops.begin()[byBlock[i]];
            if(step.position > count) continue;

            auto& released = *body.place(base, step.place);
            auto type = released.occurrence.type;
            if(!type) continue;

            auto concrete = isConcreteType(decl, type);
            auto liveHere = before.row(step.position);
            if(!globalLoanLive(liveHere, nullptr)) continue;

            globalEffects->ensure();
            auto& effects = *globalEffects;

            auto row = effects.teardownRow.get(type.offset);
            if(!row && concrete) continue;

            auto found = conflictingGlobalLoan(liveHere, nullptr,
                [&](GlobalEffects::Question question) {
                    if(row && effects.touches(row.unwrap(), question.payload, question.anyAccess)) return true;
                    return !concrete && effects.touches(effects.anyTeardownRow, question.payload, question.anyAccess);
                },
                [&](GlobalEffects::Question question) {
                    return !concrete || effects.confirmTeardown(question, type);
                });
            if(!found) continue;

            auto text = globalLoanText(found.unwrap());
            auto dying = placeText(context, decl, base, body, released);
            auto at = step.position < count ? nodeAt(index, step.position).source : function.source;

            report(at,
                   "%@ is borrowed where %@ is torn down, and its teardown may change %@ - end the borrow before %@ is released, or copy what it reads"_v,
                   text.named, dying, text.global, dying);
            return;
        }
    }
}

/*
 * A write of a global another global holds a loan of - refused wherever it is written.
 *
 * `let &gLoose = Loose {held: gArr}` is accepted because a global's loan has to be good for as
 * long as the program, and a global's is. The same fact is the rule for the lender: the loan
 * is live at every point of the program, since every function can read `gLoose.held` without being
 * handed it, so a write of `gArr` anywhere is a write under a live shared loan - `push(gArr, 4)` in
 * any body, then `gLoose.held[0]`, read the run `push` freed. So a global another global borrows
 * is read-only for the program, which is what `doc/spec/ownership.md`'s "Globals" now says. A global
 * lent **exclusively** is more than read-only: it is not named at all outside the borrows that made
 * its loans, since a read beside a write through the holder is the aliasing an exclusive loan
 * excludes. Recording it as a shared loan was the rejected alternative: it left the lender readable
 * by name beside the holder, and only an unrelated refusal of the borrow that made the loan kept a
 * program from doing it.
 *
 * The rejected alternative is flow-sensitive: the loan ends when `gLoose` is re-pointed, so a write
 * after every re-pointing would be sound. Nothing orders the functions of a program - any of them
 * may run after any other - so "after" would need a whole-program order of calls this language does
 * not have, and the one it does have (the entry's statements) is the one place such a program
 * would not write the lender anyway. A write in the entry sequence before the loan is stored is
 * refused too, as the price of not ordering it.
 *
 * After every body, since the body storing the loan may be analysed after one writing the lender.
 * A write is what `writesThrough` says - a move out, an exclusive borrow, an `addressOf` - and an
 * assignment that is not the global's own initialization.
 */
bool checkLentGlobalWrites(Context& context, Program& program, const GlobalLending& lending) {
    if(!lending.lent.size()) return true;

    auto decl = *program.decl;
    auto base = *program.mir;
    auto clean = true;

    for(auto modulePointer: program.modules) {
        for(auto functionPointer: decl[modulePointer]->functions.contents(decl)) {
            auto& function = *decl[functionPointer];
            if(function.specializes || !function.mir) continue;

            auto& body = *base[function.mir];

            // One expression may be several nodes reading the global - a format hole is - and it
            // is one use to the author.
            LocationId last = kNullLocation;

            for(U32 b = 0; b < body.blocks.size(); b++) {
                auto block = body.block(base, BlockId(b));

                for(U32 at = 0; at < block->nodes.size(); at++) {
                    auto& node = *base[block->nodes.get(base, at)];
                    if(node.source == last) continue;
                    Operand* initialized = nullptr;
                    if(node.kind == Node::Write && ((NodeWrite&)node).mode == Destination::Init) {
                        initialized = &((NodeWrite&)node).destination;
                    }

                    auto reported = false;
                    forEachOperand(base, node, [&](Operand& operand, OperandRole) {
                        if(reported || &operand == initialized || operand.source() != SourceKind::Place) return;

                        auto& place = *body.place(base, operand.placeId());
                        auto& root = rootPlace(base, place);
                        if(root.kind != Place::Global) return;

                        auto lent = lending.lent.get(root.payload);
                        if(!lent) return;

                        /*
                         * Lent exclusively, the lender is reachable only through its holder:
                         * a read by its own name anywhere is a second path to storage the holder
                         * was promised alone - `gTight.held.value += 1` in one function and
                         * `gCell.value` in another. The borrows that made the loans are the uses
                         * left legal.
                         */
                        // The texts only where a report is made: most uses of a lent global are
                        // reads, and each is asked.
                        if(lent.unwrap().exclusive) {
                            for(auto site: lent.unwrap().stores) {
                                if(site == node.source) return;
                            }

                            context.diagnostics.error(
                                "%@ cannot be used by its own name here, because %@ holds an exclusive loan of it, and a loan a global holds lasts as long as the program - reach it through %@, or give %@ a shared loan"_v,
                                node.source, placeText(context, decl, base, body, root), lent.unwrap().holder, lent.unwrap().holder, lent.unwrap().holder);
                            last = node.source;
                            reported = true;
                            clean = false;
                            return;
                        }

                        if(!writesThrough(node, operand)) return;

                        auto named = placeText(context, decl, base, body, root);
                        context.diagnostics.error(
                            "%@ cannot be written here, because %@ holds a loan of it, and a loan a global holds lasts as long as the program - give %@ a copy rather than a loan, or leave %@ unwritten"_v,
                            node.source, named, lent.unwrap().holder, lent.unwrap().holder, named);
                        last = node.source;
                        reported = true;
                        clean = false;
                    });
                }
            }
        }
    }

    return clean;
}

} // namespace mir
