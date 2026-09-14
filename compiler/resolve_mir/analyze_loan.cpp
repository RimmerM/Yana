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
    revision++;

    auto width = analysis.localWidth();
    if(width) BitMatrix::copy(state(), analysis.localsAt(analysis.localIn, block), width);
}

void OriginCursor::enterAtExit(U32 block) {
    if(!analysis.loanCount) return;

    blockIndex = block;
    entry = analysis.logStart[block + 1];
    revision++;

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
        revision++;
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
                               const Occurrence& at, SmallArray<PositionMap, 8>& into) {
    if(!originPositions(decl, local)) return;

    // The local's own root: reaching anything through the local reaches it through whatever the
    // local itself refers to, and what comes out is a value inside that storage.
    into.push(PositionMap { 0, 0 });

    auto schema = shapeOf(decl, local);
    if(!schema) return;

    auto& slots = *decl[schema];
    auto target = shapeOf(decl, at);

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

        // The place is at or inside this slot, so what it names is somebody else's object and the
        // value read out of it is rooted where the slot was pointing - at its own root.
        if(depth <= walked) {
            into.push(PositionMap { U16(1 + i), 0 });
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
 * to; it changes what is at `c`. There is nowhere here to record that, so the written value's
 * origins are unioned into the binding, which over-approximates in the direction that is safe: the
 * value that comes back out of `d` is credited with them.
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
    if(!place->occurrence.contract.isOwned()) return nullptr;

    return place;
}

/*
 * Which rows of the local origin state one node writes, asked without the state in hand.
 *
 * Structural on purpose: it is what the undo log has an entry for, and the entries are counted
 * before the fixpoint climbs. A condition that depended on the state would give a log whose shape
 * moved as the analysis settles, and a walk that undid it would be undoing a different body.
 *
 * The last condition is the one that keeps the log from growing with the slot axis. A **weak**
 * update from a value that carries no origins at all adds nothing to any row, and whether it does is
 * a property of the value's occurrence rather than of the state - so a partial write like
 * `m.write.v = 3`, which reaches two positions of `m` and stores an `I32` into neither, is not a
 * write to the state and costs no entry. A **strong** update writes even from an empty value,
 * because emptying the row is the update.
 */
bool Analysis::localWriteOf(Node& node, StateWrite& into) {
    if(!loanCount) return false;
    if(node.kind != Node::Write) return false;

    auto& write = (NodeWrite&)node;
    if(write.destination.source() != SourceKind::Place) return false;

    auto id = write.destination.placeId();
    auto root = rootOf(id);
    if(root->kind != Place::Local) return false;

    into.local = LocalId(root->payload);
    if(!localPositions(into.local)) return false;

    into.localOccurrence = body.local(base, into.local)->occurrence;
    into.placeOccurrence = body.place(base, id)->occurrence;
    into.placeCount = originPositions(decl, into.placeOccurrence);
    into.strong = replacedLocal(node) != nullptr;

    if(!into.strong && !operandCarriesOrigins(write.value)) return false;

    projectionOf(id, into.projection);

    into.positions.clear();
    selectPositions(into.projection, into.localOccurrence, into.placeOccurrence, into.positions);

    return into.positions.size() != 0;
}

void Analysis::stepOriginsForward(OriginCursor& cursor, Node& node) {
    StateWrite write;
    if(!localWriteOf(node, write)) return;

    /*
     * The value's positions, gathered before any row is touched, so that what is applied below is
     * the state on *entry* to the write.
     *
     * A local assigned from itself therefore reads the rows it is about to replace, and the copies
     * below are rows onto themselves. The cursor carries one buffer, so a transfer that wrote before
     * it read would be reading what it had already replaced - and with a position per slot that is
     * no longer one row against itself but a record's second field against its first.
     */
    auto rows = write.placeCount ? write.placeCount : 1;
    auto width = Size(rows) * loanWords;
    while(writeScratch.size() < width) writeScratch.push(0);
    BitMatrix::zero(writeScratch.pointer(), width);

    forEachOperandOrigin(cursor, ((NodeWrite&)node).value, [&](U32 position, const U64* row) {
        auto to = position < write.placeCount ? position : 0;
        BitMatrix::unionInto(writeScratch.pointer() + Size(to) * loanWords, row, loanWords);
    });

    cursor.revision++;

    for(auto& entry: write.positions) {
        auto into = originsOfLocal(cursor.state(), write.local, entry.from);
        auto to = entry.to < write.placeCount ? entry.to : 0;
        auto from = writeScratch.pointer() + Size(to) * loanWords;

        compactInto(into, from, write.strong);
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

    for(U32 i = 0; i < nodes; i++) {
        auto node = body.node(base, NodeId(i));
        if(node->kind != Node::Read) continue;

        auto& read = *(NodeRead*)node;
        if(read.loan.valid()) mintedBy[read.loan.index] = i;
    }

    for(U32 i = 0; i < nodes; i++) {
        auto node = body.node(base, NodeId(i));

        forEachOperand(base, *node, [&](Operand& operand, OperandRole role) {
            if(operand.source() != SourceKind::Result) return;

            auto harmless = node->kind == Node::Call && role == OperandRole::Argument &&
                            valuePositions(NodeId(i)) == 0;
            if(!harmless) escapes[operand.nodeId().index] = 1;
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
void Analysis::computeOrigins() {
    loanCount = body.loans.size();
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

    // After the prefix sums, which it reads, and before the tables, which it sizes.
    classifyStored();

    valueOrigins.reset(valueRowCount, loanCount);
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

        BitMatrix::unionInto(localsAt(localIn, index), cursor.state(), width);
        cursor.blockIndex = index;
        cursor.revision++;

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
                    });

                    break;
                }

                /*
                 * A record is rooted in whatever its components were - §5.11's "a constructor maps
                 * component slots into result slots", now one set per result slot.
                 *
                 * Component `i` fills the schema position whose path is `[i]`, which is what makes
                 * `Two {left: g, right: h}` two relationships rather than one union of two guards.
                 * A component the record *owns* has no position to fill and contributes nothing:
                 * `classifyTypes` refuses an owned field whose type refers to storage the record
                 * does not name, so there is no third case here for it to fall into.
                 */
                case Node::Aggregate: {
                    auto& built = (NodeAggregate&)node;
                    auto positions = valuePositions(node.id);
                    if(!positions) break;

                    auto schema = shapeOf(decl, valueOccurrence(node));
                    auto components = built.components.contents(base);

                    for(U32 i = 0; i < components.size(); i++) {
                        U32 into = 0;

                        if(schema) {
                            auto& slots = *decl[schema];
                            for(U32 j = 0; j < slots.slots.size(); j++) {
                                auto& slot = *decl[slots.slots.get(decl, j)];
                                if(slot.path.size() != 1 || slot.path.get(decl, 0) != i) continue;

                                into = 1 + j;
                                break;
                            }
                        }

                        auto row = originsOfValue(node.id, into < positions ? into : 0);
                        forEachOperandOrigin(cursor, *components.pointerAt(i),
                                             [&](U32, const U64* from) {
                            BitMatrix::unionInto(row, from, loanWords);
                        });
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
                     * A call through a function **value** publishes no origin relationships.
                     *
                     * There is no declaration to read them off, and a function type does not carry
                     * them: what a value of `(a'T) -> 'T` would mean is a slot scope of its own, and
                     * `resolveType` refuses one for exactly that reason. So a result comes back
                     * rooted in nothing, which is the conservative answer and the true one until a
                     * function type can publish a slot.
                     */
                    if(call.callsValue()) break;

                    auto& callee = *decl[call.callee];

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
                     */
                    auto arguments = call.arguments.contents(base);

                    for(U32 p = 0; p < positions; p++) {
                        auto slot = originSlotAt(decl, callee.result, p);
                        if(slot == kNoOriginSlot) continue;

                        auto into = originsOfValue(node.id, p);

                        for(U32 i = 0; i < arguments.size(); i++) {
                            auto& parameter = *decl[callee.args.get(decl, i)];

                            forEachOperandOrigin(cursor, *arguments.pointerAt(i),
                                                 [&](U32 at, const U64* row) {
                                if(originSlotAt(decl, parameter.occurrence, at) != slot) return;
                                BitMatrix::unionInto(into, row, loanWords);
                            });
                        }
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
         * Read with the cursor standing at the block's exit, which is the state on entry to the
         * terminator: a terminator is the only node that names edges and `localWriteOf` wants a
         * `Write`, so nothing between the two points moves the state.
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
 * The undo log, from the settled fixpoint - one pass, and the only thing that writes `log`.
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

    // Counted first, because `logPrevious` is a row per edit and a `BitMatrix` is sized once. The
    // count is structural - `localWriteOf` reads the nodes and not the state - so this is the same
    // walk the fill below makes, without the state.
    U32 edits = 0;
    StateWrite write;

    for(U32 index = 0; index < blockCount; index++) {
        for(auto nodePointer: block(index)->nodes.contents(base)) {
            if(localWriteOf(*base[nodePointer], write)) edits += U32(write.positions.size());
        }
    }

    logPrevious.resetWords(edits, stateWords);

    OriginCursor cursor { *this };
    auto width = localWidth();

    for(U32 index = 0; index < blockCount; index++) {
        logStart.push(log.size());

        // Seeded from `localIn` directly rather than through `cursor.enter`, which reads the very
        // `logStart` this loop is still filling in.
        cursor.blockIndex = index;
        cursor.revision++;
        if(width) BitMatrix::copy(cursor.state(), localsAt(localIn, index), width);

        auto count = nodeCount(index);

        for(U32 position = 0; position < count; position++) {
            auto& node = nodeAt(index, position);

            // One entry per *position* the node writes, and `undo` is a loop over the ones that
            // share a position for exactly that reason: a write to a whole `Two` replaces both of
            // its fields' origin sets, which is two rows at one program point.
            if(localWriteOf(node, write)) {
                for(auto& touched: write.positions) {
                    auto row = localRow[write.local.index] + touched.from;
                    auto entry = U32(log.size());

                    log.push(OriginEdit { position, row });
                    BitMatrix::copy(logPrevious.row(entry), cursor.state() + Size(row) * stateWords,
                                    stateWords);
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
    if(!loanCount) return;

    loanIn.reset(blockCount, loanCount);
    loanOut.reset(blockCount, loanCount);

    SmallArray<U64, 4> live;
    for(U32 w = 0; w < loanWords; w++) live.push(0);

    OriginCursor cursor { *this };

    sweepBackward([&](U32 index) {
        auto block = this->block(index);
        BitMatrix::zero(live.pointer(), loanWords);

        auto count = block->nodes.size();
        if(count) {
            auto last = base[block->nodes.get(base, count - 1)];
            forEachEdge(base, *last, [&](Edge& edge) {
                if(!edge.target) return;

                auto target = loansAt(loanIn, base[edge.target]->index.index);
                BitMatrix::unionInto(live.pointer(), target, loanWords);
            });
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
 * element restore what they saved - and an exchange has one place to begin with. The shipping stage
 * grants the same pair through `exchangedOnly` in `resolve/analyze_borrow.cpp`.
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
    auto& loan = *body.loan(base, id);
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
    for(U32 w = 0; w < loanWords; w++) {
        scratch.push(0); reachedSet.push(0); overlapping.push(0); conflict.push(0);
        frontier.push(0); probe.push(0); skipSet.push(0);
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

    for(U32 l = 0; l < loanCount; l++) {
        auto root = rootOf(body.loan(base, LoanId(l))->place);
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
     * every operation a fiber context performs is a borrow through the address it was handed. The
     * shipping stage accepts all of it.
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
        auto root = rootOf(body.loan(base, LoanId(m))->place);
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
    auto reachFrom = [&](OriginCursor& cursor, PlaceId place, SmallArray<PlaceId, 8>& into) {
        into.clear();

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

        // Then what those loans' places are rooted in, until nothing new arrives - one round per
        // level of reborrowing, and a body has one or two. Over a frontier rather than over the
        // whole set, so that a round costs the loans it added and not the loans it has.
        BitMatrix::copy(frontier.pointer(), reachedSet.pointer(), loanWords);

        while(BitMatrix::any(frontier.pointer(), loanWords)) {
            BitMatrix::zero(scratch.pointer(), loanWords);
            BitMatrix::forEach(frontier.pointer(), loanWords, [&](Size m) {
                unionPlaceOrigins(cursor, body.loan(base, LoanId(m))->place, scratch.pointer());
            });

            for(U32 w = 0; w < loanWords; w++) scratch[w] &= ~reachedSet[w];
            BitMatrix::unionInto(reachedSet.pointer(), scratch.pointer(), loanWords);
            BitMatrix::copy(frontier.pointer(), scratch.pointer(), loanWords);
        }

        BitMatrix::forEach(reachedSet.pointer(), loanWords, [&](Size m) {
            into.push(body.loan(base, LoanId(m))->place);
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

                        auto& loan = *body.loan(base, LoanId(l));
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
     * computed: what the author wrote is safe, and the shipping stage accepts all three.
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
     * Per block and cleared at every entry, which is the conservative direction: a reservation that
     * crosses a branch is treated as already active, and the argument lists this exists for are
     * straight-line by construction.
     */
    Array<U32> loanOfNode;
    for(Size i = 0; i < body.nodes.size(); i++) loanOfNode.push(kNoReservation);

    for(U32 index = 0; index < blockCount; index++) {
        auto block = this->block(index);
        for(U32 position = 0; position < block->nodes.size(); position++) {
            auto& node = nodeAt(index, position);
            if(node.kind != Node::Read) continue;

            auto& read = (NodeRead&)node;
            if(read.loan.valid()) loanOfNode[node.id.index] = read.loan.index;
        }
    }

    SmallArray<U64, 4> reserved;
    for(U32 w = 0; w < loanWords; w++) reserved.push(0);

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
    // take-side question above reads the second of them at another node's position.
    BitMatrix after, before;

    auto check = [&](OriginCursor& cursor, Node& node, PlaceId place, AccessKind kind,
                     const U64* live, const U64* skip, LoanId taken = LoanId()) {
        if(kind == AccessKind::Copy) return;
        if(!BitMatrix::any(live, loanWords)) return;

        reachFrom(cursor, place, reached);

        // Through a loan already refused - see `refused`. `reachedSet` is what `reachFrom` left:
        // the loans this place's origins reach, closure included.
        if(BitMatrix::intersects(refused.pointer(), reachedSet.pointer(), loanWords)) return;

        /*
         * The loans whose place overlaps one this access reaches - asked of the loans on the roots
         * it reaches and of no others. A root that may be anywhere overlaps every loan, so every
         * live loan conflicts and there is nothing to compute.
         */
        BitMatrix::zero(overlapping.pointer(), loanWords);
        auto anywhere = false;

        for(auto x: reached) {
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
                    if(placesOverlap(base, decl, body, x, body.loan(base, LoanId(l))->place)) {
                        BitMatrix::set(overlapping.pointer(), l);
                    }
                }
            }
        }

        // And the loans whose own root may be anywhere overlap whatever the access reaches - as
        // far as an address reaches it at all, which is `rootIsAddressable`.
        for(auto x: reached) {
            if(!rootIsAddressable(rootOf(x))) continue;

            for(auto l: anyRootLoans) BitMatrix::set(overlapping.pointer(), l);
            break;
        }

        // A live loan conflicts where it overlaps directly or reaches a loan that does - the
        // backward walk, restricted to what is live after the node.
        if(anywhere) {
            BitMatrix::copy(conflict.pointer(), live, loanWords);
        } else {
            BitMatrix::copy(conflict.pointer(), overlapping.pointer(), loanWords);
            holdersInto(cursor, overlapping.pointer(), conflict.pointer());
            for(U32 w = 0; w < loanWords; w++) conflict[w] &= live[w];
        }

        SmallArray<U32, 16> candidates;
        BitMatrix::forEach(conflict.pointer(), loanWords, [&](Size l) { candidates.push(U32(l)); });

        for(auto l: candidates) {
            if(skip && BitMatrix::test(skip, l)) continue;

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
            auto& other = *body.loan(base, LoanId(l));

            if(taken.valid()) {
                if(BitMatrix::test(refused.pointer(), l)) continue;

                BitMatrix::zero(scratch.pointer(), loanWords);
                unionPlaceOrigins(cursor, other.place, scratch.pointer());
                if(BitMatrix::intersects(refused.pointer(), scratch.pointer(), loanWords)) continue;
            }

            // Any number of shared loans may coexist. Everything else - a unique loan meeting any
            // overlapping loan, and a write or a move meeting either - conflicts.
            if(kind == AccessKind::TakeLoan) {
                auto taking = taken.valid() ? taken : ((NodeRead&)node).loan;
                if(taking.index == l) continue;

                auto capability = body.loan(base, taking)->capability;
                if(capability == Access::Shared && other.capability == Access::Shared) continue;

                /*
                 * The reservation above. A shared read never excludes one; another loan does only
                 * where it is still live at the activation, which is the point the exclusion
                 * begins - see `reserved`, where both halves are argued.
                 */
                if(BitMatrix::test(reserved.pointer(), l)) {
                    if(capability == Access::Shared) continue;

                    auto activation = activationOf[l];
                    if(activation != kNoReservation &&
                       !BitMatrix::test(before.row(activation), taking.index)) {
                        continue;
                    }
                }
            }

            /*
             * The element exemption - `exchangeOnlyLoan`, above. The pair is excused when this
             * access is one half and the loan it conflicts with is the other: a second element
             * loan being taken, or a relocation writing through the first. Both halves are asked,
             * so a swap's loan meeting an ordinary borrow of the container is refused as any
             * other pair is.
             */
            auto half = kind == AccessKind::TakeLoan
                ? exchangeOnlyLoan(taken.valid() ? taken : ((NodeRead&)node).loan)
                : exchangeAccess(node);
            if(half && exchangeOnlyLoan(LoanId(l))) continue;

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

                if(kind == AccessKind::TakeLoan) {
                    auto taking = taken.valid() ? taken : ((NodeRead&)node).loan;
                    BitMatrix::set(refused.pointer(), taking.index);
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

    for(U32 index = 0; index < blockCount; index++) {
        auto block = this->block(index);
        auto count = block->nodes.size();
        if(!count) continue;

        after.reset(count, loanCount);
        before.reset(count, loanCount);
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
        BitMatrix::zero(reserved.pointer(), loanWords);

        // And where each of this block's reservations activates - the first operand naming the
        // read that minted it, which is the same walk the forward pass makes below.
        for(U32 l = 0; l < loanCount; l++) activationOf[l] = kNoReservation;

        for(U32 position = 0; position < count; position++) {
            forEachOperand(base, nodeAt(index, position), [&](Operand& operand, OperandRole) {
                if(operand.source() != SourceKind::Result) return;

                auto minted = loanOfNode[operand.nodeId().index];
                if(minted == kNoReservation) return;
                if(activationOf[minted] == kNoReservation) activationOf[minted] = position;
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
            forEachOperand(base, node, [&](Operand& operand, OperandRole) {
                if(operand.source() != SourceKind::Result) return;

                auto minted = loanOfNode[operand.nodeId().index];
                if(minted == kNoReservation) return;
                if(!BitMatrix::test(reserved.pointer(), minted)) return;

                BitMatrix::clear(reserved.pointer(), minted);

                // A loan whose taking was already reported is a loan the program does not have -
                // `refused`, and checking its activation would be the same mistake read twice.
                if(BitMatrix::test(refused.pointer(), minted)) return;

                auto& loan = *body.loan(base, LoanId(minted));
                check(forward, node, loan.place, AccessKind::TakeLoan, before.row(position),
                      skipOfValue(operand.nodeId()), LoanId(minted));
            });

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
                               body.loan(base, read.loan)->capability == Access::Unique) {
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

                    // An `Init` establishes a slot rather than writing through one, so there is
                    // nothing in it for a loan to be pointing at.
                    if(write.mode == Destination::Init) break;

                    auto place = write.destination.placeId();
                    check(forward, node, place, AccessKind::Write, at, skipOfPlace(forward, place));
                    break;
                }

                case Node::Return:
                    checkReturnConformance(node);
                    break;

                case Node::Call:
                    checkHandoverConformance(node);
                    break;

                default:
                    break;
            }

            // After the checks and outside the switch, because every case above asks what storage
            // held on *entry* to the node and two of them leave the switch early.
            stepOriginsForward(forward, node);
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

    // The prefix sum goes with the rows, because without it a row number means nothing: a value's
    // positions are a range and the range is what says which of them is the root.
    out.originRows.reserve(program.mir, nodes + 1);
    for(U32 i = 0; i <= nodes; i++) out.originRows.push(program.mir, valueRow[i]);

    auto rows = valueRow[nodes];
    out.origins.reserve(program.mir, Size(rows) * loanWords);

    for(U32 i = 0; i < rows; i++) {
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
/*
 * A **closure** leaving the frame its captures live in - `resolve_lambda.cpp`, and the one place the
 * heap this stage has no allocator for is asked for.
 *
 * The check itself is the ordinary one: the value is rooted in a loan of this frame's storage, and a
 * result may not be. What this adds is the *message*, and it is worth adding because the ordinary
 * one would name a local the author never wrote - the environment is synthesized, so "this result
 * refers to `main.lambda.env`" describes the compiler rather than the program.
 *
 * The fix it names is the true one, and since D17 it is a fix the author can make. A closure that
 * outlives its frame owns what it captured and its environment is a heap-placed block - and which of
 * the two placements a lambda gets is decided by the **position it is written at**
 * (`resolve_lambda.cpp`). So what reaches here is a closure written at a *borrowing* position and
 * then sent to an owning one, which is a lambda one binding too far from the place it belongs.
 */
bool Analysis::reportEnvironmentEscape(LocationId source, Place& root) {
    if(root.kind != Place::Local) return false;

    auto local = body.local(base, LocalId(root.payload));
    if(!local->environment) return false;

    report(source,
           "this closure captures storage of this frame, so it cannot leave it - write the lambda at the position that takes it, so its environment is placed where the value goes"_v);
    return true;
}

/*
 * Whether a parameter's storage is **this frame's** rather than the caller's.
 *
 * An owned parameter was moved in, so the slot holding it belongs to the callee exactly as a `let`
 * does, and a hand-over that refers to it is the same mistake as one that refers to a local. A
 * parameter the caller lent - a slot origin or a call-bounded loan - is storage the caller keeps,
 * which is `checkReturnConformance`'s distinction below and is read the same way here.
 *
 * The wider question this deliberately does not ask is whether a hand-over should also have to name
 * the *caller's* origin the way a result does. §12.3(c) is about frame-rooted values and this is
 * exactly that sentence; an extent-conformance rule for hand-overs is a separate statement and there
 * is no spelling in the language for one yet.
 */
bool Analysis::frameOwnedParameter(const Local& local) {
    return decl[function.args.get(decl, local.parameter)]->occurrence.contract.isOwned();
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
 * allocator or foreign code is rooted in no loan at all, so a bare `%T` stays what it is.
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

    // Position zero is the root and always states something; a position the declaration described
    // states whatever it says, `Origin::CallBounded` included. What is left is a position nothing
    // was written about at all, which `bindsOriginAt` is the one reader of.
    for(U32 p = 1; p < positions; p++) {
        if(bindsOriginAt(decl, function.handover, p)) continue;

        auto origins = originsOfValue(produced, p);

        for(U32 l = 0; l < loanCount; l++) {
            if(!BitMatrix::test(origins, l)) continue;

            auto& loan = *body.loan(base, LoanId(l));
            auto root = rootOf(loan.place);
            if(root->kind != Place::Local) continue;

            auto local = body.local(base, LocalId(root->payload));
            if(local->isParameter() && !frameOwnedParameter(*local)) continue;

            report(node.source,
                   "this hands over a value referring to %@, which is this frame's own storage, and its type has no origin to say so - write `yield'` on the position that refers to it"_v,
                   placeText(context, decl, base, body, *body.place(base, loan.place)));
            return;
        }
    }
}

void Analysis::checkReturnConformance(Node& node) {
    auto& written = (NodeReturn&)node;
    if(!written.hasValue) return;

    if(written.value.source() != SourceKind::Result) return;

    auto produced = written.value.nodeId();
    auto positions = valuePositions(produced);

    /*
     * One loan against the declared slot, following a **raw address** to what it points at.
     *
     * A borrow taken through a pointer - `borrow(advance(values(xs), i))`, which is what every
     * element of a window is - is rooted at an address place, and an address is not storage
     * this function owns or was lent. What it *is* is a value with a pointee position, and the
     * loans in that position are where the borrow is really rooted: `values(xs)` published `xs`'s
     * loan on its result, the arithmetic carried it, and the borrow inherits it here. So an
     * address root is judged by its pointee's loans, each by the same rule, and an address whose
     * type published nothing is what the old refusal was about.
     */
    auto checkLoan = [&](auto& self, U32 l, U16 declared) -> bool {
        auto& loan = *body.loan(base, LoanId(l));
        auto root = rootOf(loan.place);

        // A global outlives the program, so a reference into one satisfies any slot and needs no
        // relationship published for it.
        if(root->kind == Place::Global) return true;

        if(reportEnvironmentEscape(node.source, *root)) return false;

        auto named = placeText(context, decl, base, body, *body.place(base, loan.place));

        /*
         * A position of the result that refers to something the signature did not say whose.
         *
         * An owned `-> Reader` handed back with a live guard in it, or a root borrowed with no
         * origin on it. The spelling that fixes the first is a loan argument on the result's own
         * type, `-> Reader(a')`, and the second is a `'` on the result - so the message names
         * the type rather than the position, because both fixes are written there.
         */
        if(declared == kNoOriginSlot) {
            report(node.source,
                   "this result refers to %@, and its type has no origin to say so"_v, named);
            return false;
        }

        if(root->kind == Place::Address) {
            auto pointer = NodeId(root->payload);

            if(valuePositions(pointer) < 2) {
                report(node.source,
                       "%@ is reached through a pointer whose type names no origin, so nothing says whose storage it is - write `%source'a`"_v,
                       named);
                return false;
            }

            auto pointee = originsOfValue(pointer, 1);
            for(U32 inner = 0; inner < loanCount; inner++) {
                if(!BitMatrix::test(pointee, inner) || inner == l) continue;
                if(!self(self, inner, declared)) return false;
            }

            return true;
        }

        if(root->kind != Place::Local) {
            report(node.source, "%@ is not storage this function can publish the extent of"_v,
                   named);
            return false;
        }

        auto local = body.local(base, LocalId(root->payload));

        /*
         * A local that holds a borrow **of a published origin** is a reborrow, not this frame's
         * storage.
         *
         * `hold` gives a borrowing value a frame slot so that a projection has a place to name -
         * `xs[i]` is a call whose result is parked in one, and so is `borrow(p).key`. The slot is
         * this frame's; what is *in* it was named by the producing signature as somebody else's, so
         * a loan minted over it says nothing about who owns the object and everything about which
         * name reaches it.
         *
         * What it is really rooted in is in the same origin set: `computeOrigins`' read rule unions
         * the storage's origins into the value beside the loan it mints, so the lender's loan is
         * checked by the loop below this one and a genuinely frame-owned lender is still refused
         * there. That union is why this can skip rather than recurse the way the address case does -
         * a local's origins are flow-sensitive and there is no point in hand here to read them at,
         * while a pointer's pointee is a value's and there is no union to rely on.
         *
         * **`Origin::Slot` and not merely "borrowed"**, which is the whole of the rule's edge. A
         * hand-over is `Origin::CallBounded` - the granting frame said nothing about whose storage
         * it is - so a binding that holds one is exactly the escape `lens/Reject.Escape.yana`
         * refuses, and its origin set carries no loan of the callee's frame for the loop to object
         * to. `let x = y` over a local is call-bounded for the same reason and stays refused here.
         */
        if(!local->isParameter() && local->occurrence.contract.origin() == Origin::Slot) return true;

        if(!local->isParameter()) {
            report(node.source,
                   "%@ is this function's own storage, so a borrow of it cannot outlive the call"_v,
                   named);
            return false;
        }

        auto granted = decl[function.args.get(decl, local->parameter)]->occurrence.contract;
        if(granted.origin() == Origin::Slot && granted.slot() == declared) return true;

        if(granted.origin() != Origin::Slot) {
            report(node.source,
                   "%@ is borrowed for the length of this call, so a borrow of it cannot be returned - publish an origin on it with `'`"_v,
                   named);
            return false;
        }

        report(node.source, "%@ is in a different origin from the one the result names"_v, named);
        return false;
    };

    /*
     * Each position of the returned value against the slot the signature named for *it*.
     *
     * Per position rather than over one union, because a result can now state a relationship for its
     * interior positions as well as for its root: `-> Reader(a')` hands back an owned record whose
     * `held` field is in the caller's `a`, and asking about the value as a whole cannot tell that
     * from a root that names `a` and a field that names nothing.
     *
     * The first loan that fails ends the whole check, and that is not per position either. A value's
     * origin set holds every loan it may be rooted in, and one mistake usually puts several in it:
     * `return r.held` on a reader this frame built is the loan of the guard *and* the loan of the
     * field that holds it, which are one error about one statement. Reporting each would be the pass
     * counting its own intermediate values at the author.
     */
    for(U32 p = 0; p < positions; p++) {
        auto declared = originSlotAt(decl, function.result, p);
        auto origins = originsOfValue(produced, p);

        for(U32 l = 0; l < loanCount; l++) {
            if(!BitMatrix::test(origins, l)) continue;
            if(!checkLoan(checkLoan, l, declared)) return;
        }
    }
}

} // namespace mir
