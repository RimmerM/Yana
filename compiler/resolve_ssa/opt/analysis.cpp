#include "pass.h"

/*
 * The whole-body answers, computed and cached - see analysis.h, which is where the argument for
 * caching them lives.
 *
 * The dominator tree is not computed here: it is `resolve_ssa/dominance.cpp`, shared with the
 * verifier and the lowering. What is here is everything derived from it that only a pass wants.
 */

namespace ssa {

/*
 * The natural loops of one body, from the back edges over the dominator tree.
 *
 * A back edge is an edge whose target dominates its source, and the loop it names is every block
 * that reaches that source without leaving through the header - which is the backwards walk below,
 * stopping at the header because the header is marked before it starts.
 *
 * Unreachable blocks are left out entirely. A back edge inside a region nothing enters is not a
 * loop the program can run, and the dominance relation says nothing about a block with no position
 * in the order - so admitting one would be building a loop out of an answer that is not an answer.
 */
void Loops::build(SsaBase base, Body& body, const Dominators& dominators) {
    // Emptied rather than assumed empty: this is `opt.loops`, which outlives every body it is
    // computed for. `clear` destroys each entry - freeing its block set - and keeps the array's own
    // storage, which is the containers guidance's answer for a list rebuilt per function.
    all.clear();

    auto count = U32(body.blocks.size());
    auto entry = base[body.entry]->index.index;

    // Emptied per back edge rather than built per back edge. Inline, because the walk backwards from
    // one back edge reaches the blocks of one loop.
    SmallArray<U32, 32> pending;

    for(U32 i = 0; i < count; i++) {
        auto block = body.block(base, BlockId(i));
        if(block->deleted || !dominators.isReachable(i)) continue;

        forEachSuccessor(base, *block, [&](U32 header) {
            // The entry block heads no loop: nothing branches into a body, so an edge back to it
            // would be a second entry and the "loop" would be the whole function.
            if(header == entry || !dominators.dominates(header, i)) return;

            // One loop per header, not per back edge: two edges back to the same block are one loop
            // with two ways round, and the second is a second backwards walk into the same set.
            auto existing = all.size();
            for(Size l = 0; l < all.size(); l++) {
                if(all[l].header == header) existing = l;
            }

            if(existing == all.size()) {
                Loop fresh;
                fresh.header = header;
                fresh.contains.reset(count);
                fresh.contains.set(header, true);
                all.push(::move(fresh));
            }

            auto& loop = all[existing];
            pending.clear();
            if(loop.contains.add(i)) pending.push(i);

            while(pending.size()) {
                auto index = pending.pop().unwrap();

                for(auto predecessor: body.block(base, BlockId(index))->incoming(base)) {
                    auto from = base[predecessor]->index.index;
                    if(!loop.contains.add(from)) continue;

                    pending.push(from);
                }
            }
        });
    }

    for(auto& loop: all) {
        /*
         * The one block outside the loop that jumps straight to the header.
         *
         * Both halves matter. It has to be the *only* outside predecessor, or an instruction put
         * there would run on one way in and not the other; and the header has to be its only
         * successor, or the instruction would also run on a path that never enters the loop - which
         * for a pure value is waste and for a read is a load the program did not ask for.
         *
         * Counted over `incoming`, which holds one entry per edge: a predecessor whose two arms both
         * land on the header supplies two of them and is therefore declined, which is the safe
         * answer for a block that has already decided something on the way in.
         *
         * Where there is no such block the loop is left alone. Making one means splitting an edge,
         * which is CFG surgery with block arguments attached, and the loops this front end emits
         * already have one - a `for` builds its test as a block of its own with a single jump in.
         */
        auto header = body.block(base, BlockId(loop.header));
        auto outside = Loop::kNone;
        Size outsideCount = 0;

        for(auto predecessor: header->incoming(base)) {
            auto from = base[predecessor]->index.index;
            if(loop.contains[from]) continue;

            outside = from;
            outsideCount++;
        }

        if(outsideCount != 1) continue;

        Size successors = 0;
        forEachSuccessor(base, *body.block(base, BlockId(outside)), [&](U32) { successors++; });
        if(successors == 1) loop.preheader = outside;
    }

    /*
     * Innermost first, by the size of the block set.
     *
     * An inner loop's blocks are a subset of the loop containing it, so "fewer blocks" is "further
     * in" for any two that nest and says nothing about two that do not - which is all the order has
     * to mean. An insertion sort because the count is a handful; the entries move by assignment,
     * which is why `Loop` holds nothing that cannot be moved.
     */
    for(Size i = 1; i < all.size(); i++) {
        for(Size j = i; j > 0 && all[j].contains.popCount() < all[j - 1].contains.popCount(); j--) {
            swap(all[j], all[j - 1]);
        }
    }
}

Dominators& dominatorsOf(OptContext& opt) {
    if(!opt.dominanceStamp.holds(opt.function, opt.body->version, false)) {
        opt.dominators.build(opt.base, *opt.body);
        opt.dominanceStamp.take(opt.function, opt.body->version);
    }

    return opt.dominators;
}

Loops& loopsOf(OptContext& opt) {
    if(!opt.loopStamp.holds(opt.function, opt.body->version, false)) {
        // Through the cached tree rather than a fresh one: the loops are derived from it, so two
        // readings that disagreed would be a loop whose header does not dominate its blocks.
        opt.loops.build(opt.base, *opt.body, dominatorsOf(opt));
        opt.loopStamp.take(opt.function, opt.body->version);
    }

    return opt.loops;
}

Reachable reachableOf(OptContext& opt) {
    return Reachable { &dominatorsOf(opt) };
}

/*
 * One step up an address chain, or null where the chain ends.
 *
 * The three ways an address in this IR names storage some *other* value already names, and they are
 * the same three `Contained` admits as projections rather than escapes:
 *
 *  - a **field** or an **element** of a record or a container, which is the ordinary walk;
 *  - a `load` whose result is **`Guaranteed`**, which at this tier is how a borrow is spelled: there
 *    is no `Borrow` instruction, so `&x` is a `load` of the slot whose result is a second name for
 *    that storage and is no instruction at all in the lowering. Following it is what lets a body
 *    that reaches its container through a borrow be rooted in the container's own slot;
 *  - `represent`, which restates a view record as the occurrence it manifests without moving a byte
 *    (`mir::UnaryOp::Represent`, whose lowering answers the operand's address).
 *
 * Everything else ends the chain, and the two callers below read the end differently.
 */
Value* addressStep(OptContext& opt, Value* address) {
    if(!address) return nullptr;

    auto definer = address->definer;
    if(!definer) return nullptr;

    auto inst = opt.base[definer];
    switch(inst->kind) {
        case Inst::FieldAddress:
            return opt.base[((InstFieldAddress*)inst)->base.value()];
        case Inst::ElementAddress:
            return opt.base[((InstElementAddress*)inst)->base.value()];
        case Inst::Load: {
            if(!inst->results.size()) return nullptr;
            if(opt.base[inst->results.get(opt.base, 0)]->convention != Convention::Guaranteed) {
                return nullptr;
            }

            return opt.base[((InstLoad*)inst)->address.value()];
        }
        case Inst::Unary:
            if(((InstUnary*)inst)->op != mir::UnaryOp::Represent) return nullptr;
            return opt.base[((InstUnary*)inst)->value.value()];
        default:
            return nullptr;
    }
}

Value* allocationRoot(OptContext& opt, Value* address) {
    while(address) {
        auto definer = address->definer;
        if(!definer) return nullptr;
        if(opt.base[definer]->kind == Inst::Alloc) return address;

        address = addressStep(opt, address);
    }

    // A global's address, a pointer loaded out of storage, an address the unsafe boundary produced.
    // None of them is an allocation of this body, and the permissive answer is the only sound one
    // for all three.
    return nullptr;
}

bool addressMayName(OptContext& opt, Value* address, Value& allocation) {
    SmallArray<Value*, 8> pending;
    SmallArray<Value*, 8> seen;
    pending.push(address);

    while(pending.size()) {
        auto at = pending.pop().unwrap();

        // The end of the chain rather than its root: a root is the answer, and anything that is not a
        // block argument is a root this body cannot name, which `allocationRoot`'s null already says.
        while(at && at->definer) {
            if(opt.base[at->definer]->kind == Inst::Alloc) break;
            at = addressStep(opt, at);
        }

        if(!at) continue;
        if(at == &allocation) return true;
        if(at->definer || !at->block) continue;

        auto known = false;
        for(auto visited: seen) known = known || visited == at;
        if(known) continue;
        seen.push(at);

        auto block = opt.base[at->block];
        Size position = 0;
        while(position < block->argumentCount()) {
            if(opt.base[block->argumentAt(opt.base, position)] == at) break;
            position++;
        }

        // One entry per edge, so a predecessor with two edges here is walked twice; both of its
        // edges into this block are asked each time, which repeats an answer rather than losing one.
        for(auto predecessor: block->incoming(opt.base)) {
            auto terminator = opt.base[predecessor]->terminator(opt.base);
            if(!terminator) continue;

            forEachEdge(opt.base, *terminator, [&](Edge& edge) {
                if(opt.base[edge.target] != block || position >= edge.arguments.size()) return;

                auto used = edge.arguments.get(opt.base, position);
                if(used.isSet()) pending.push(opt.base[used.value()]);
            });
        }
    }

    return false;
}

// The step an address is, and the address it is taken of. `base` is null where the address is a
// root - an allocation, a global, a pointer this pass cannot see behind.
Maybe<AddressStep> addressStepOf(OptContext& opt, Value& address, Value*& base) {
    base = nullptr;
    if(!address.definer) return Nothing();

    auto inst = opt.base[address.definer];
    if(inst->kind == Inst::FieldAddress) {
        auto& projection = (InstFieldAddress&)*inst;
        base = opt.base[projection.base.value()];

        return Just(AddressStep { Inst::FieldAddress, projection.record, projection.field, nullptr,
                                  projection.shape, projection.property });
    }

    if(inst->kind == Inst::ElementAddress) {
        auto& element = (InstElementAddress&)*inst;
        base = opt.base[element.base.value()];

        return Just(AddressStep { Inst::ElementAddress, element.container, 0,
                                  opt.base[element.index.value()] });
    }

    return Nothing();
}

/*
 * Whether two element steps name one element - the half all three comparisons below share, and the
 * one part of a step that is a *value* rather than an immediate.
 *
 * Value identity first, because that is what an unfolded index answers with: two `ElementAddress`
 * instructions handed the same loop counter are the same element without either being a number.
 */
static bool sameIndex(OptContext& opt, const AddressStep& left, const AddressStep& right) {
    if(left.index == right.index) return true;

    auto a = constantValueOf(opt, left.index);
    auto b = constantValueOf(opt, right.index);
    return a && b && a.unwrap() == b.unwrap();
}

// Whether two steps of one shape name the same component - `simplify`'s `sameImmediates` asked
// of an address rather than of a computation, plus the index, which is a value there and a
// number here.
bool sameAddressStep(OptContext& opt, const AddressStep& left, const AddressStep& right) {
    if(left.kind != right.kind || left.of != right.of) return false;

    if(left.kind == Inst::FieldAddress) {
        return left.field == right.field && left.shape == right.shape &&
               left.property == right.property;
    }

    return sameIndex(opt, left, right);
}

// The same with the shape left out - see the declaration, which is where the difference is argued.
bool sameAddressComponent(OptContext& opt, const AddressStep& left, const AddressStep& right) {
    if(left.kind != right.kind || left.of != right.of) return false;

    if(left.kind == Inst::FieldAddress) {
        if(left.property != InstFieldAddress::kNoProperty) return false;
        if(right.property != InstFieldAddress::kNoProperty) return false;

        return left.field == right.field;
    }

    return sameIndex(opt, left, right);
}

/*
 * Whether two field projections of one record name storage that cannot overlap.
 *
 * **Only a `RecordLayout::Single` record**, and that is the whole of the rule. A record with one
 * constructor has one field per piece of storage and no discriminant, so two different indices
 * are two different places. A **sum** has neither property: `resolve_mir` keeps one flat field
 * list with a range per constructor, so two constructors' fields are different indices over the
 * *same* payload storage, and a discriminant may be folded into a niche of that payload, which
 * makes writing the payload the way the tag comes to say so. Neither is a question this tier may
 * answer - which storage a constructor occupies and whether a tag is folded are `lower/repr.cpp`'s
 * answers, a **target's**, and `optimizeProgram` runs once per program rather than once per
 * target. `fold.cpp` makes the same refusal about a target-answered width and gives the same
 * reason.
 *
 * What is *not* excluded is co-packing. Two narrow fields of a single-constructor record share a
 * machine word on some targets and are still two fields here, because what a packed access
 * becomes is `lower/pack.cpp`'s expansion one tier down - and that expansion masks, so the
 * separation this states stays true underneath it. It is the shipping stage's ruling and its
 * reason, arriving where the packing has not happened yet rather than in front of a pass that
 * does it.
 *
 * A **property** address is declined on both sides: it names a structural requirement rather than
 * a field, `record` and `field` are meaningless on one, and two requirements this stage cannot
 * see the substitution for may well be one field.
 */
static bool separateFields(OptContext& opt, const AddressStep& left, const AddressStep& right) {
    if(left.property != InstFieldAddress::kNoProperty) return false;
    if(right.property != InstFieldAddress::kNoProperty) return false;
    if(left.field == InstFieldAddress::kDiscriminant) return false;
    if(right.field == InstFieldAddress::kDiscriminant) return false;
    if(left.field == right.field) return false;

    if(!left.of) return false;
    auto declared = opt.decl[left.of];
    if(declared->kind != mir::Type::Record) return false;

    return ((mir::RecordType*)declared)->layout == mir::RecordLayout::Single;
}

// Whether two steps of one shape name components that cannot overlap - the other half of
// `sameAddressStep`, and deliberately not its negation: two indices neither of which is a constant
// are neither the same element nor two different ones.
bool separateAddressStep(OptContext& opt, const AddressStep& left, const AddressStep& right) {
    if(left.kind != right.kind || left.of != right.of) return false;
    if(left.kind == Inst::FieldAddress) return separateFields(opt, left, right);

    auto a = constantValueOf(opt, left.index);
    auto b = constantValueOf(opt, right.index);
    return a && b && a.unwrap() != b.unwrap();
}

namespace {

/*
 * Whether one use of an address keeps it in this frame - `Contained`'s rule, one operand at a time.
 *
 * The operand is compared by identity rather than by position, because a kind that reads a value
 * twice reads it in two roles: `store %a, %a` is a store *through* the allocation and a store *of*
 * its address, and only the first is contained. So every naming slot has to be the admitted one.
 */
struct Containment {
    OptContext& opt;

    /*
     * The names whose use lists are open on the stack - see `escapes`.
     *
     * An address chain used to be a tree of pure projections rooted at one allocation, so a cycle in
     * it would have been a value defined below its own operand, which is what `checkUse` reports.
     * The edge arm below made it a graph: a block argument is a name whose operand can be that same
     * name round a back edge, and a `while` that keeps a borrow across its latch is exactly one.
     */
    IndexSet visiting;

    /*
     * Whether anything done with this name lets the address out of the frame.
     *
     * A name already on the stack answers **no**, and that is sound rather than optimistic: escape
     * is a union over every use reachable from the allocation, the frame that marked the name is
     * still walking that name's own use list, and a refusal it finds there returns through this
     * frame to the root. So every use is examined exactly once and no refusal is lost - what the set
     * removes is the second examination, not the answer.
     */
    bool escapes(Value& address) {
        auto index = address.id.index;
        if(index < visiting.size() && visiting[index]) return false;

        if(index < visiting.size()) visiting.set(index, true);

        auto out = false;
        for(auto userPointer: address.uses(opt.base)) {
            auto& user = *opt.base[userPointer];
            if(!admits(user, address)) { out = true; break; }
        }

        if(index < visiting.size()) visiting.set(index, false);
        return out;
    }

    bool admits(Inst& user, Value& address) {
        auto named = [&](Use used) { return used.isSet() && opt.base[used.value()] == &address; };

        switch(user.kind) {
            /*
             * Reads and writes of what is at the address. A `Store` is admitted through its address
             * and refused as its value, which is the whole-address-as-a-value case above.
             *
             * A `Load` needs the second half, and it is the shape a **borrow** has at this tier.
             * There is no `Borrow` instruction here - `addressOf` hands back the allocation and the
             * *use* says how it is taken - so `&x` handed to a callee arrives as a `load` of the slot
             * whose result is `Guaranteed`, and the loan on it names the allocation. That result is a
             * second name for the storage, so what keeps the allocation contained is that it does not
             * escape either, which is the same walk one level down. A `Trivial` result is the
             * contents, an `Owned` one is the contents taken out, and an `Address` one is a pointer
             * that was *in* the slot rather than a way back to it; none of the three is a reference
             * to this storage.
             */
            case Inst::Load: {
                if(!named(((InstLoad&)user).address)) return false;
                if(!user.results.size()) return true;

                auto result = opt.base[user.results.get(opt.base, 0)];
                if(result->convention != Convention::Guaranteed) return true;

                return !escapes(*result);
            }
            case Inst::Copy:
                return named(((InstCopy&)user).address);
            case Inst::Drop:
                return named(((InstDrop&)user).address);

            /*
             * The release of a heap or region placement, which **keeps the address in the frame**
             * rather than letting it out - and that is what this row corrects.
             *
             * `allocateHeap`/`freeHeap` are library calls by the time the machine sees them, and
             * with no row here the `free` fell to the default and every heap-placed object read as
             * escaping. It is the opposite of escaping: a `free` is the last event of the storage's
             * life, inside this body, so an allocation whose only other uses are accesses is one
             * nothing outside the frame can name. A heap slot is a frame slot with an allocator in
             * front of it as far as every reader of `Contained` is concerned, and what makes that
             * true is that both ends are here.
             *
             * `HeapPlace.yana` is the shape it was costing: `@heap &p` written and read in one
             * block kept every store and every load, because `forwardMemory` reads containment and
             * an escaping allocation is one it may not carry a fact across.
             */
            case Inst::Free:
                return named(((InstFree&)user).address);
            case Inst::Store:
                return named(((InstStore&)user).address) && !named(((InstStore&)user).value);

            /*
             * An atomic reaches its location as an address and its operands as values - `mir::
             * AtomicForm`, whose shape puts the location first. So the allocation is admitted at
             * that position and refused at every other, which is `intoValue` handing the storage
             * itself over.
             */
            case Inst::Atomic: {
                auto& atomic = (InstAtomic&)user;
                if(!atomic.arguments.size()) return false;
                if(!named(atomic.arguments.get(opt.base, 0))) return false;

                for(Size i = 1; i < atomic.arguments.size(); i++) {
                    if(named(atomic.arguments.get(opt.base, i))) return false;
                }

                return true;
            }

            /*
             * A projection, which keeps the address in the frame exactly as far as its own result
             * does - the recursion, and the reason this is a walk rather than a use-list scan.
             *
             * An `ElementAddress` is refused where the allocation is its *index*, which is an
             * address used as a number: nothing here computes one, and admitting it would be
             * admitting an arbitrary arithmetic use.
             */
            /*
             * `represent` - a view record restated as the occurrence it manifests, which moves no
             * byte: `mir::UnaryOp::Represent`'s own row says "storage is shared: the lowering
             * answers the operand's address". So the result is a second name for this storage on
             * exactly the terms a `Guaranteed` load result is, and it keeps the address in the frame
             * exactly as far as its own result does.
             *
             * Missing this row is what kept every container's slot out of `Contained`: a body
             * reaches its window by building the view record and representing it, so the one
             * instruction between the slot and every read of it read as an escape.
             */
            case Inst::Unary: {
                auto& unary = (InstUnary&)user;
                if(unary.op != mir::UnaryOp::Represent || !named(unary.value)) return false;
                return !user.results.size() || !escapes(*opt.base[user.results.get(opt.base, 0)]);
            }

            case Inst::FieldAddress:
                if(!named(((InstFieldAddress&)user).base)) return false;
                return !user.results.size() || !escapes(*opt.base[user.results.get(opt.base, 0)]);
            case Inst::ElementAddress: {
                auto& element = (InstElementAddress&)user;
                if(!named(element.base) || named(element.index)) return false;
                return !user.results.size() || !escapes(*opt.base[user.results.get(opt.base, 0)]);
            }

            /*
             * A **call argument at a position whose address the callee does not keep** - the one
             * escape with an end to it, and what `mir::Summary::escapes` is for.
             *
             * The address is handed over and the callee may do anything with it *for the duration of
             * the call*; what `escapes` says is that no address of the storage at that position was
             * written anywhere, so nothing outside can name it afterwards - which is the whole of
             * what containment claims. The half this leaves behind is what the callee wrote
             * *through* it while it had it, which is `mayWriteStorage`'s `mutates` row and is asked
             * of the call itself.
             *
             * **`retains` is the wrong row here, and asking it kept every container's slot out of
             * containment.** That row is a reach set over values: `growArray(&self, n)` reads a run
             * pointer out of `self` and stores it back into `self`, so parameter 0 is in the written
             * value's reach and reads as retained - while no address of `self`'s storage went
             * anywhere at all. Separating the two is sound in exactly this predicate's terms: an
             * allocation admitted here is one whose address is never stored, so no pointer the
             * callee loads out of memory can be it.
             *
             * `Unknown` reads back as `ArgumentSet::all()`, so a callee whose row nothing filled is
             * the outright escape this used to be for every call. The two rows are read in two
             * places on purpose: escape is about the *address*, mutation is about the *storage*, and
             * one predicate answering both would be a containment that quietly means "and never
             * written", which is not what any reader of `Contained` asks it.
             */
            case Inst::Call: {
                auto& call = (InstCall&)user;
                // The summary is the *callee's*, so it may be read only where `callee` is who runs.
                // Through a slot or a value it is the signature the call was checked against, which
                // has no body and publishes nothing - see `InstCall::callee`.
                if(!call.callee || call.through != mir::CallThrough::Declaration) return false;
                if(named(call.target)) return false;

                auto kept = opt.decl[call.callee]->summary.escapes().get(mir::ArgumentSet::all());

                for(Size i = 0; i < call.arguments.size(); i++) {
                    if(!named(call.arguments.get(opt.base, i))) continue;
                    if(kept.has(U32(i))) return false;
                }

                /*
                 * And what the call handed back - J48, which is the obligation that moved here when
                 * `escapes` stopped counting a return.
                 *
                 * A callee may answer with the address it was given: `valuesMut(&self)` hands back a
                 * pointer into the storage, and a body that then stores that pointer has let this
                 * allocation out without ever naming it again. The summary used to say so, at the
                 * cost of every accessor in the library reporting its subject kept - which is a
                 * price the *other* reader of the row could not pay. So the row says stores only and
                 * the two readers follow the result themselves, which is what `opt/place.cpp` has
                 * always done with its taint.
                 *
                 * `escapes` here is this same walk one level down, exactly as the `Load` arm above
                 * uses it: a result nothing does anything with clears, and one that reaches a store
                 * or a return refuses.
                 */
                for(Size i = 0; i < call.results.size(); i++) {
                    if(escapes(*opt.base[call.results.get(opt.base, i)])) return false;
                }

                return true;
            }

            /*
             * A name **handed over an edge**, which is a second name for the same storage at the
             * other end of it and not a way out of the frame - J50.
             *
             * A block argument is what a borrow that crosses a join or a back edge is spelled as at
             * this tier, and there is nothing else it can be: the operand is a value of this body
             * and the argument is filled from it. So the address is contained exactly as far as that
             * argument is, which is this same walk one level down - the recursion the `Load` and
             * `Unary` arms above already make, with the block argument standing where a result
             * would.
             *
             * Refusing it read as an escape and cost the **allocation**, not the edge: `pick` in
             * `test/resolve/LiveAfterLoop.yana` is inlined into a join, so `main`'s `a` was borrowed
             * into a block argument and therefore uncontained, and every reader of `Contained` -
             * `forwardMemory`, `promote`, `place` - then had to leave it in memory for the whole
             * body. The counterpart in `opt/promote.cpp` is what turns the answer into instructions.
             *
             * Every position the address fills is asked, on every edge, because a terminator can
             * hand the same value to two arms and a `branch %c, X, X` is two edges into one block.
             */
            case Inst::Jump:
            case Inst::Branch:
            case Inst::Switch: {
                auto held = true;

                forEachEdge(opt.base, user, [&](Edge& edge) {
                    if(!held) return;
                    if(!edge.target) { held = false; return; }

                    auto target = opt.base[edge.target];
                    Size position = 0;

                    for(auto used: edge.arguments.contents(opt.base)) {
                        auto at = position++;
                        if(!used.isSet() || opt.base[used.value()] != &address) continue;

                        // An edge longer than its target declares is what `checkEdge` reports; there
                        // is no argument to ask about, so there is nothing this may admit.
                        if(at >= target->argumentCount()) { held = false; return; }
                        if(escapes(*opt.base[target->argumentAt(opt.base, at)])) {
                            held = false;
                            return;
                        }
                    }
                });

                return held;
            }

            // And no default that admits. An aggregate component, a native operand, a returned
            // value and a captured environment are each a way for the address to outlive the
            // instruction naming it, and a kind nobody has looked at is one whose effect on the
            // address has not been checked.
            default:
                return false;
        }
    }
};

} // namespace

void Contained::build(OptContext& opt) {
    auto count = opt.body->values.size();
    inside.reset(count);

    Containment walk { opt };
    walk.visiting.reset(count);

    for(auto valuePointer: opt.body->values.contents(opt.base)) {
        auto value = opt.base[valuePointer];
        if(!value->definer || opt.base[value->definer]->kind != Inst::Alloc) continue;

        // An allocation whose defining instruction was erased is not one: `eraseInst` leaves the
        // table entry standing, and a value defined nowhere and read nowhere is what the verifier
        // itself skips.
        if(!opt.base[value->definer]->block) continue;

        inside.set(value->id.index, !walk.escapes(*value));
    }
}

namespace {

/*
 * Whether one instruction is the `Drop` of this allocation - the one writer `ConstantContents`
 * admits, for the reason `analysis.h` gives.
 */
bool isDropOf(OptContext& opt, Inst& inst, Value& allocation) {
    if(inst.kind != Inst::Drop) return false;

    auto address = opt.base[((InstDrop&)inst).address.value()];
    return allocationRoot(opt, address) == &allocation;
}

/*
 * The one whole-slot `Store` of a constant into this allocation, and null where there is no such
 * store or where the allocation is written more than once.
 *
 * The **direct** use list is enough for the store to be found, because a whole-slot write names the
 * allocation itself; a write through a projection is a different instruction and is what the scan
 * over the body catches.
 */
Inst* constantFill(OptContext& opt, Value& allocation) {
    Inst* fill = nullptr;

    for(auto userPointer: allocation.uses(opt.base)) {
        auto& user = *opt.base[userPointer];
        if(user.kind != Inst::Store) continue;
        if(opt.base[((InstStore&)user).address.value()] != &allocation) continue;

        if(fill) return nullptr;

        auto stored = opt.base[((InstStore&)user).value.value()];
        if(!stored->definer || opt.base[stored->definer]->kind != Inst::Const) return nullptr;

        fill = &user;
    }

    return fill;
}

} // namespace

void ConstantContents::build(OptContext& opt, const Contained& contained) {
    auto count = opt.body->values.size();

    held.clear();
    held.reserve(count);
    for(Size i = 0; i < count; i++) held.push(mir::Constant());

    for(auto valuePointer: opt.body->values.contents(opt.base)) {
        auto value = opt.base[valuePointer];
        if(!value->definer || opt.base[value->definer]->kind != Inst::Alloc) continue;
        if(!opt.base[value->definer]->block) continue;
        if(!contained[value]) continue;

        auto fill = constantFill(opt, *value);
        if(!fill) continue;

        /*
         * Every other instruction of the body asked the one question that answers it - "may this
         * write that storage" - rather than an allowlist of the kinds that read. A kind nobody has
         * looked at yet is then a barrier, which is the direction this has to fail in.
         *
         * It is a scan of the body per candidate, and a candidate is an allocation something stored
         * a constant into whole: a handful per function, against a body that is being walked by
         * every pass in this directory already.
         */
        auto written = false;
        for(auto blockPointer: opt.body->blocks.contents(opt.base)) {
            auto block = opt.base[blockPointer];

            for(auto instPointer: block->instructions(opt.base)) {
                auto& inst = *opt.base[instPointer];
                if(&inst == fill || isDropOf(opt, inst, *value)) continue;
                if(!mayWriteStorage(opt, contained, inst, *value)) continue;

                written = true;
                break;
            }

            if(written) break;
        }

        if(written) continue;

        auto stored = opt.base[((InstStore&)*fill).value.value()];
        held[value->id.index] = ((InstConst&)*opt.base[stored->definer]).constant;
    }
}

ConstantContents& constantContentsOf(OptContext& opt) {
    if(!opt.constantStamp.holds(opt.function, opt.body->version, true)) {
        opt.constantContents.build(opt, containedOf(opt));
        opt.constantStamp.take(opt.function, opt.body->version);
    }

    return opt.constantContents;
}

/*
 * One step of a **constant** address walk - see `constantAt`, where the two roots are argued.
 *
 * Deliberately not `addressStep`: that walk is about which storage an address names and steps
 * through a `represent`, which hands back the same bytes at another type. A constant is a tree of
 * values rather than bytes, so a step that changes the type without changing the storage has no
 * index to follow and stops the walk.
 */
struct ConstantStep {
    Value* base = nullptr;
    U32 index = 0;
    bool indexed = false;
};

static Maybe<ConstantStep> constantStepOf(OptContext& opt, Value& address) {
    if(!address.definer) return Nothing();
    auto inst = opt.base[address.definer];

    if(inst->kind == Inst::FieldAddress) {
        auto& projection = (InstFieldAddress&)*inst;
        if(projection.property != InstFieldAddress::kNoProperty) return Nothing();
        if(projection.field == InstFieldAddress::kDiscriminant) return Nothing();

        return Just(ConstantStep { opt.base[projection.base.value()], projection.field, true });
    }

    if(inst->kind == Inst::ElementAddress) {
        auto& element = (InstElementAddress&)*inst;
        auto index = constantValueOf(opt, opt.base[element.index.value()]);
        if(!index) return Nothing();

        return Just(ConstantStep { opt.base[element.base.value()], U32(index.unwrap()), true });
    }

    /*
     * A **copy** of what is at an address - the same contents read out rather than a second name for
     * the storage, so a projection of the copy is a projection of the same constant. No index, for
     * the reason the borrow below carries none: the step changes what the value *is* held in and not
     * which part of the constant is being named.
     *
     * This is how a `let` of a record type is read. `alias qualified Flags = I64` in
     * `test/resolve/TruthOperators.yana` gives `copy (global.addr empty)` followed by
     * `field.addr .Flags` and a `load` of that, and without this arm the walk stopped at the copy -
     * so a comparison against a global constant stayed a comparison, `main` kept every arm of a
     * fixture the shipping stage folds to one constant, and the difference was a thousand bytes in
     * one body.
     *
     * Sound on exactly the terms the two roots are: `constantAt` concludes nothing unless it reaches
     * a global `isWritten` says nothing writes, or an allocation `ConstantContents` says holds one
     * value throughout. Against either of those a snapshot and a name are the same answer.
     */
    if(inst->kind == Inst::Copy) {
        return Just(ConstantStep { opt.base[((InstCopy*)inst)->address.value()], 0, false });
    }

    // A borrow of the slot, which is a `Guaranteed` load here and names the same storage. Not a
    // step into the value, so it carries no index.
    if(inst->kind == Inst::Load) {
        if(!inst->results.size()) return Nothing();
        if(opt.base[inst->results.get(opt.base, 0)]->convention != Convention::Guaranteed) {
            return Nothing();
        }

        return Just(ConstantStep { opt.base[((InstLoad*)inst)->address.value()], 0, false });
    }

    return Nothing();
}

Maybe<mir::Constant> constantAt(OptContext& opt, const ConstantContents& contents, Value* address) {
    SmallArray<U32, 8> path;

    while(address) {
        if(auto definer = address->definer) {
            auto inst = opt.base[definer];

            if(inst->kind == Inst::Alloc) {
                auto held = contents[*address];
                if(!held.isSet()) return Nothing();

                break;
            }

            /*
             * A global's own initializer, where nothing writes the global.
             *
             * `isWritten` and not the language's `let &`: a `dynamic` global is assigned to by no
             * expression and is still filled by the entry sequence, so its declared contents are
             * what it holds only until the program starts.
             */
            if(inst->kind == Inst::GlobalAddress) {
                if(opt.decl[((InstGlobalAddress*)inst)->global]->isWritten()) return Nothing();
                break;
            }
        }

        auto step = constantStepOf(opt, *address);
        if(!step) return Nothing();

        if(step.unwrap().indexed) path.push(step.unwrap().index);
        address = step.unwrap().base;
    }

    if(!address || !address->definer) return Nothing();

    auto root = opt.base[address->definer];
    auto value = root->kind == Inst::Alloc
        ? contents[*address]
        : opt.decl[((InstGlobalAddress*)root)->global]->initial;

    if(!value.isSet()) return Nothing();

    // From the root down, which is the reverse of the order the walk pushed them in.
    for(Size i = path.size(); i-- > 0;) {
        value = mir::constantField(opt.decl, value, path[i]);
        if(!value.isSet()) return Nothing();
    }

    return Just(value);
}

Contained& containedOf(OptContext& opt) {
    if(!opt.containedStamp.holds(opt.function, opt.body->version, true)) {
        opt.contained.build(opt);
        opt.containedStamp.take(opt.function, opt.body->version);
    }

    return opt.contained;
}

namespace {

/*
 * Whether an operand may name bytes inside this allocation.
 *
 * **A contained allocation is named only by projections of its own address**, and that is the whole
 * of the rule. `Contained` says every use of the address keeps it in this frame: it is refused as a
 * `Store` value, as an aggregate component, as a native operand, as a returned value, as an edge
 * operand and as a captured environment, and admitted at a call argument only where the callee
 * keeps no address of that position's storage. So no value in the body holds a pointer into it that
 * did not come from the allocation itself - and every route that does is one `allocationRoot` walks
 * - a field or element projection, a `Guaranteed` load (which is how a borrow is spelled here) and a
 * `represent` - or a block argument an edge filled with one of those, which the containment walk
 * also admits and `addressMayName` follows back. Pointer arithmetic is not among them, so there is
 * no other way in.
 *
 * That covers three cases one predicate each used to answer separately, and the third is what a
 * string literal needed: a pointer **loaded out of storage** roots nowhere and misses (the shape
 * every container has - the run pointer is loaded from the header, so no store through it writes the
 * header back); a pointer **parameter** roots nowhere and misses, for the same reason; and a call
 * argument that is not an address at all - a length, a copied record, a second object entirely -
 * roots nowhere and misses. Before that last one, a call with one non-address argument at a
 * mutating position was a barrier for every slot in the frame, which is what kept a literal's own
 * fields from being read back out of the constant that filled them.
 *
 * With no containment there is nothing to reason from: two addresses one of which has no root of
 * this body's may name the same bytes, and the permissive answer is the only sound one.
 */
bool addressMayReach(OptContext& opt, const Contained& contained, Use address, Value& allocation) {
    if(!address.isSet()) return true;

    auto value = opt.base[address.value()];
    auto root = allocationRoot(opt, value);
    if(root) return root == &allocation;

    // A borrow handed over an edge roots nowhere and still names the allocation - `addressMayName`.
    return !contained[allocation] || addressMayName(opt, value, allocation);
}

} // namespace

bool mayWriteStorage(OptContext& opt, const Contained& contained, Inst& inst,
                     Value& allocation) {
    switch(inst.kind) {
        case Inst::Store:
            return addressMayReach(opt, contained, ((InstStore&)inst).address, allocation);

        /*
         * The **handover**: a destructive read takes the object out and leaves the slot empty, which
         * is a write of the storage however it is spelled. It is also one of the four events that
         * end a loan - `resolve_ssa/README.md` - and the two facts are the same fact: what a borrow
         * of the slot would be reading afterwards is storage the program has emptied.
         */
        case Inst::Load: {
            if(!inst.results.size()) return false;
            if(!isTracked(opt.base[inst.results.get(opt.base, 0)]->convention)) return false;

            return addressMayReach(opt, contained, ((InstLoad&)inst).address, allocation);
        }

        // A teardown receives an address and may write through it, so a drop is a read *and* a write
        // of its own storage. What it may not reach is an allocation whose address it was never
        // given, which is the opaque case below.
        case Inst::Drop:
            if(addressMayReach(opt, contained, ((InstDrop&)inst).address, allocation)) return true;
            return !contained[allocation];

        case Inst::Atomic: {
            auto& atomic = (InstAtomic&)inst;
            if(!atomic.arguments.size()) return true;

            return addressMayReach(opt, contained, atomic.arguments.get(opt.base, 0), allocation);
        }

        // Storage that has just come into existence with nothing in it.
        case Inst::Alloc:
            return false;

        // A duplicate: it reads its address and writes storage of its own that nothing else names
        // yet - `lower/inst.cpp`'s `Copy` arm is the allocation that makes the second owner a second
        // object.
        case Inst::Copy:
            return false;

        /*
         * A call, and the two facts that answer it.
         *
         * A callee cannot write storage whose address it was never given, so an allocation that is
         * **not** contained is one it may have reached by some other route - and then the only
         * question left is whether the callee writes through anything at all.
         *
         * A **contained** allocation is the case `escapes` opened. `Contained` now admits a call
         * argument at a position whose address the callee does not keep, so "contained" no longer
         * means "no call ever saw it" - it means no call kept it. This call may still be the one holding it, so the
         * positions it was handed at are asked directly: a position the callee does not write
         * through is not a barrier, and neither is a position this call did not point here.
         *
         * `Unknown` reads back as `ArgumentSet::all()` at both rows, which is the answer this had
         * before either was filled: every argument written, every call a barrier.
         */
        case Inst::Call: {
            auto& call = (InstCall&)inst;
            if(!call.callee || call.through != mir::CallThrough::Declaration) return true;

            auto mutated = opt.decl[call.callee]->summary.mutates().get(mir::ArgumentSet::all());
            if(!contained[allocation]) return mutated.any();

            for(Size i = 0; i < call.arguments.size(); i++) {
                if(!mutated.has(U32(i))) continue;
                if(addressMayReach(opt, contained, call.arguments.get(opt.base, i), allocation)) return true;
            }

            return false;
        }

        default:
            /*
             * Everything else, and the default is yes for anything that is not pure.
             *
             * `kSsaPure` is the column that says an instruction may be recomputed or not computed at
             * all, and nothing that writes storage can be either - so it is the one blanket answer
             * available here. What is left over it is `Native`, which is a system call or a block
             * move whose safety comes from an argument this tier cannot read, and every kind nobody
             * has looked at yet. Forgetting too much costs an optimization; forgetting too little is
             * a miscompile.
             */
            if(isPure(inst)) return false;
            return !contained[allocation];
    }
}

} // namespace ssa
