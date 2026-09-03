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
        auto into = state() + Size(edit.row) * analysis.loanWords;
        BitMatrix::copy(into, analysis.logPrevious.row(entry), analysis.loanWords);
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

void Analysis::unionPlaceOrigins(OriginCursor& cursor, PlaceId place, U64* into) {
    unionOperandOrigins(cursor, Operand(SourceKind::Place, place.index, UseMode::Inspect), into);
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

        if(write.strong) BitMatrix::copy(into, from, loanWords);
        else BitMatrix::unionInto(into, from, loanWords);
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
                 * A **window** is rooted wherever the borrow it was made from was.
                 *
                 * That is the whole of what makes it safe, and it is one line because the design put
                 * it there: a window is a *description* of a loan rather than a value with a
                 * lifetime of its own, so the loan the `read` underneath minted travels straight
                 * through and every question about how long the window may live is a question about
                 * that loan. `at` and `count` are numbers and contribute nothing.
                 *
                 * The shipping stage's equivalent is `convertSlice`, which builds a two-word record
                 * - so what the analysis saw was a *fresh owned value* whose relationship to the
                 * container had to be recovered afterwards, through `Local::viewOf` and
                 * `resolveViewRoots`. Both of those exist because of the shape chosen here, and
                 * `sum(slice(elements(xs), 1, 4))` releasing `xs` before reading through the window
                 * is what happened while one of them was missing.
                 */
                case Node::Window: {
                    auto& window = (NodeWindow&)node;
                    if(!valuePositions(node.id)) break;

                    auto into = originsOfValue(node.id, 0);
                    forEachOperandOrigin(cursor, window.source, [&](U32, const U64* from) {
                        BitMatrix::unionInto(into, from, loanWords);
                    });

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
            forEachEdge(*last, [&](Edge& edge) {
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

    logPrevious.reset(edits, loanCount);

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
                    BitMatrix::copy(logPrevious.row(entry), cursor.state() + Size(row) * loanWords,
                                    loanWords);
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
            forEachEdge(*last, [&](Edge& edge) {
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
     * Per *node* and not per body, because a place's origins are a question about a program point -
     * which is the whole reason the origin state is flow-sensitive.
     */
    BitMatrix closure;
    closure.reset(loanCount, loanCount);

    SmallArray<U64, 4> scratch, reachedSet, overlapping, needed, skipSet;
    for(U32 w = 0; w < loanWords; w++) {
        scratch.push(0); reachedSet.push(0); overlapping.push(0); needed.push(0); skipSet.push(0);
    }

    /*
     * Cached against the cursor's **revision**, not against the node.
     *
     * The closure is taken from the origin state and from nothing else, so it is the same closure
     * for as long as the state has not moved - and `OriginCursor::revision` is exactly that, counted
     * by the one place that moves it. Keying the cache on the node instead rebuilt an `L x L`
     * closure at every node of the body, and `L` grows with the program - every argument passed to a
     * borrowing parameter mints a loan - so the walk was `nodes x L^2 / 64` on a body where both
     * terms scale together. A chain of sixteen `if`s over eighty borrowed guards took 0.91 s and
     * 6.65 s at a hundred and sixty; keyed on the state it is 0.17 s and 0.71 s, and 0.09 s and
     * 0.39 s once the overlap loop below stops asking about loans nothing can be holding.
     *
     * A single-entry cache is enough because the revision only ever goes up: the walk never returns
     * to a state it has already left.
     */
    auto closureRevision = ~U32(0);

    auto buildClosure = [&](OriginCursor& cursor) {
        if(closureRevision == cursor.revision) return;
        closureRevision = cursor.revision;

        for(U32 l = 0; l < loanCount; l++) {
            auto into = closure.row(l);

            BitMatrix::zero(into, loanWords);
            unionPlaceOrigins(cursor, body.loan(base, LoanId(l))->place, into);
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
    auto reachFrom = [&](OriginCursor& cursor, PlaceId place, SmallArray<PlaceId, 8>& into) {
        into.clear();
        into.push(place);

        BitMatrix::zero(reachedSet.pointer(), loanWords);
        unionPlaceOrigins(cursor, place, reachedSet.pointer());

        // Over a snapshot, because the closure rows are unioned back into the set they are being
        // read from. `scratch` is free here: `buildClosure` has finished with it by the time the
        // one caller of this reaches it.
        BitMatrix::copy(scratch.pointer(), reachedSet.pointer(), loanWords);
        BitMatrix::forEach(scratch.pointer(), loanWords, [&](Size m) {
            BitMatrix::unionInto(reachedSet.pointer(), closure.row(m), loanWords);
        });

        BitMatrix::forEach(reachedSet.pointer(), loanWords, [&](Size m) {
            into.push(body.loan(base, LoanId(m))->place);
        });
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
     * `overlapping` is "whose own place does this access touch", asked once of every loan against
     * the places the access reaches; and a loan conflicts when it is in that set or when anything it
     * reaches is - which is a word-at-a-time test against the closure rather than a second walk over
     * two place lists.
     */
    auto check = [&](OriginCursor& cursor, Node& node, PlaceId place, AccessKind kind,
                     const U64* live, const U64* skip) {
        if(kind == AccessKind::Copy) return;
        if(!BitMatrix::any(live, loanWords)) return;

        buildClosure(cursor);
        reachFrom(cursor, place, reached);

        /*
         * Which loans the conflict loop can actually ask about: the live ones, and everything they
         * reach through the closure.
         *
         * `overlapping` was computed for **every loan in the body**, and `placesOverlap` is a walk
         * over two projection chains - so the cost was the whole loan count times the reach of the
         * access, at every access, for a row the loop below reads only these bits of. The loan count
         * grows with the program rather than with the number of borrows a point holds: every
         * argument handed to a borrowing parameter mints one, and they are dead a node later.
         */
        BitMatrix::copy(needed.pointer(), live, loanWords);
        BitMatrix::forEach(live, loanWords, [&](Size l) {
            BitMatrix::unionInto(needed.pointer(), closure.row(l), loanWords);
        });

        BitMatrix::zero(overlapping.pointer(), loanWords);

        BitMatrix::forEach(needed.pointer(), loanWords, [&](Size l) {
            auto other = body.loan(base, LoanId(l))->place;

            for(auto x: reached) {
                if(!placesOverlap(base, decl, body, x, other)) continue;

                BitMatrix::set(overlapping.pointer(), l);
                break;
            }
        });

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

    // Two cursors over one block and not one, because the two passes below run in opposite
    // directions. Declared out here rather than per block so that the state buffer is allocated
    // once for the body.
    OriginCursor backward { *this }, forward { *this };

    for(U32 index = 0; index < blockCount; index++) {
        auto block = this->block(index);
        auto count = block->nodes.size();
        if(!count) continue;

        after.reset(count, loanCount);
        BitMatrix::copy(live.pointer(), loansAt(loanOut, index), loanWords);
        backward.enterAtExit(index);

        for(U32 n = count; n > 0; n--) {
            auto position = n - 1;
            BitMatrix::copy(after.row(position), live.pointer(), loanWords);
            backward.undo(position);
            stepLoansBackward(live.pointer(), backward, nodeAt(index, position));
        }

        forward.enter(index);

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
                            check(forward, node, place, AccessKind::TakeLoan, at,
                                  skipOfValue(node.id));
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
 * The fix it names is the true one. A closure that outlives its frame has to own what it captured
 * (`doc/spec/functions.md`'s consumed capture) and its environment has to be somewhere that outlives
 * the frame, which is `StorageClass::Heap` - a rung `verifyBody` refuses for want of an allocator.
 * The two arrive together, and until they do this is where a program asks for them.
 */
bool Analysis::reportEnvironmentEscape(LocationId source, Place& root) {
    if(root.kind != Place::Local) return false;

    auto local = body.local(base, LocalId(root.payload));
    if(!local->environment) return false;

    report(source,
           "this closure captures storage of this frame, so it cannot leave it - that needs a heap-placed environment, which the ownership resolver does not implement yet"_v);
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
    auto produced = written.value.nodeId();
    auto positions = valuePositions(produced);

    for(U32 p = 0; p < positions; p++) {
        auto declared = originSlotAt(decl, function.result, p);
        auto origins = originsOfValue(produced, p);

        for(U32 l = 0; l < loanCount; l++) {
            if(!BitMatrix::test(origins, l)) continue;

            auto& loan = *body.loan(base, LoanId(l));
            auto root = rootOf(loan.place);

            // A global outlives the program, so a reference into one satisfies any slot and needs no
            // relationship published for it.
            if(root->kind == Place::Global) continue;

            if(reportEnvironmentEscape(node.source, *root)) return;

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
                return;
            }

            if(root->kind != Place::Local) {
                report(node.source, "%@ is not storage this function can publish the extent of"_v,
                       named);
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
            if(granted.origin() == Origin::Slot && granted.slot() == declared) continue;

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
}

} // namespace mir
