#include "pass.h"
#include "../../resolve_mir/analyze.h"

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
            auto kind = opt.base[at->definer]->kind;
            if(kind == Inst::Alloc || kind == Inst::Call) break;
            at = addressStep(opt, at);
        }

        if(!at) continue;
        if(at == &allocation) return true;

        /*
         * A **call's result** is a second name for whatever its arguments named - J92. `Contained`
         * admits an argument whose callee hands its address back, as long as the result does not
         * escape in turn (J48), so `pick(&p) -> &'Int` leaves `p` contained while `q = q - 7` writes
         * through the answer. Ending the walk here said that store reached nothing of `p`, and
         * `ConstantContents` then folded a later read of `p.a` to the literal the record was built
         * with. The callee's summary does not say which argument the result came from, so every
         * argument is a candidate; a call whose result is not an address is never asked this.
         */
        if(at->definer && opt.base[at->definer]->kind == Inst::Call) {
            auto& call = (InstCall&)*opt.base[at->definer];
            for(Size i = 0; i < call.arguments.size(); i++) {
                auto used = call.arguments.get(opt.base, i);
                if(used.isSet()) pending.push(opt.base[used.value()]);
            }
            continue;
        }

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
 * Every block argument an edge terminator fills with `address`, on every edge - the walk
 * `Containment::admits` and `ReadWalk::only` share, which each had a copy of until J163.
 *
 * Every position is asked, on every edge, because a terminator can hand the same value to two arms
 * and a `branch %c, X, X` is two edges into one block. `visit` answers whether to go on. An edge
 * with no target, or one longer than its target declares - what `checkEdge` reports - has no
 * argument to ask about, and stops the walk as a refusal would. Answers whether it went through.
 */
template<class Visit>
bool forEachEdgeSlotOf(OptContext& opt, Inst& user, Value& address, Visit&& visit) {
    auto held = true;

    forEachEdge(opt.base, user, [&](Edge& edge) {
        if(!held) return;
        if(!edge.target) { held = false; return; }

        auto target = opt.base[edge.target];
        Size position = 0;

        for(auto used: edge.arguments.contents(opt.base)) {
            auto at = position++;
            if(!used.isSet() || opt.base[used.value()] != &address) continue;
            if(at >= target->argumentCount()) { held = false; return; }

            if(!visit(*opt.base[target->argumentAt(opt.base, at)])) {
                held = false;
                return;
            }
        }
    });

    return held;
}

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

    // Whether a window is bytes read out of a slot - a `Guaranteed` load, through `represent`s -
    // rather than a value a call answered, which may point into the slot's own storage. See the
    // `Aggregate` arm.
    bool windowReadOutOfLine(Value& window) {
        auto value = &window;

        for(U32 depth = 0; depth < 16; depth++) {
            if(!value->definer) return false;

            auto& definer = *opt.base[value->definer];
            if(definer.kind == Inst::Load) return value->convention == Convention::Guaranteed;
            if(definer.kind != Inst::Unary || ((InstUnary&)definer).op != mir::UnaryOp::Represent) return false;

            auto operand = ((InstUnary&)definer).value;
            if(!operand.isSet()) return false;
            value = opt.base[operand.value()];
        }

        return false;
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
                //
                // One such call is admitted, and only as its **target**: a call through a function
                // value read out of this slot - J148. What it reads there is the pair, and the callee
                // is handed the environment the pair holds and the arguments, never the address of
                // the slot; a capture of the slot would have been an escape at the `fun.value`
                // already. Refused where the slot is also an argument, since nothing says what an
                // unknown callee keeps; and its result is not followed, because a callee never
                // handed the address cannot answer with it. It is what lets `soleContents` answer a
                // function slot at all.
                if(!call.callee || call.through != mir::CallThrough::Declaration) {
                    if(call.through != mir::CallThrough::Value || !named(call.target)) return false;

                    for(auto used: call.arguments.contents(opt.base)) {
                        if(named(used)) return false;
                    }

                    return true;
                }
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
             * Every position the address fills is asked, on every edge - `forEachEdgeSlotOf`.
             */
            case Inst::Jump:
            case Inst::Branch:
            case Inst::Switch:
                return forEachEdgeSlotOf(opt, user, address, [&](Value& argument) { return !escapes(argument); });

            /*
             * A **window** installed as a field - J153. A view-form occurrence manifests as its
             * view record, before anything asks whether it is a reference (`manifestNative`), so the
             * aggregate copies the record's bytes - the items pointer and the length - and holds no
             * name for the storage the window was read out of. What the items pointer names is the
             * owner's run, which is `opt/place.cpp`'s question and not this one's.
             *
             * Only into a field that holds the window *as* a window: its own contract a view too, and
             * not `holdsAddress`, which is a capture of the lender's storage whatever its occurrence
             * would lay out. One record constructor with a single layout, where the components are
             * the fields in order; anything else still refuses.
             *
             * It is what `data Cursor {buf: &'[Int]}` read back into `View {buf: c.buf}` was costing:
             * the cursor's slot read as escaping, so the run stored in it could not be placed in the
             * frame (`test/resolve/SliceCapability.yana`, 211 lowered instructions against 38).
             *
             * **Only a window read out of a slot** - J161, `windowReadOutOfLine`. "What the items
             * pointer names is the owner's run" is true of an `Array`/`String` owner and of a
             * window stored in a field, and false of a window a *call* answered: `elementsMut` of a
             * `[T *n]`, an `InlineArray`'s accessor and any `fn f(&self: s'R) -> &s'[T] =
             * self.fixed` hand back a window whose items pointer is the slot's own storage, and
             * admitting it put `win2.yana`'s `Quad` in `Contained` while `pokeMove` wrote through
             * the record it was handed (4 where 77 is right; `-inline none` lost the same writes to
             * a plain `[Int *4]`). So the component must be a `Guaranteed` load - through any
             * number of `represent`s - and the two types with a view row, `Array` and `String`, are
             * both owners whose elements are a separate run. `addressMayName` does not look through
             * an aggregate for the same reason: nothing this arm admits holds a name for the slot.
             */
            case Inst::Aggregate: {
                if(address.convention != Convention::Guaranteed) return false;
                if(!mir::viewRecordOf(opt.context, opt.program, address.occurrence)) return false;
                if(!windowReadOutOfLine(address)) return false;

                auto& aggregate = (InstAggregate&)user;
                if(aggregate.constructor != 0 || !user.results.size()) return false;

                auto& made = *opt.base[user.results.get(opt.base, 0)];
                auto declared = made.occurrence.type ? opt.decl[made.occurrence.type] : nullptr;
                if(!declared || declared->kind != mir::Type::Record) return false;

                auto& record = *(mir::RecordType*)declared;
                if(record.layout != mir::RecordLayout::Single) return false;
                if(aggregate.components.size() != record.fields.size()) return false;

                for(Size i = 0; i < aggregate.components.size(); i++) {
                    if(!named(aggregate.components.get(opt.base, i))) continue;

                    auto field = record.field(opt.decl, U16(i));
                    if(field.holdsAddress || mir::indirectField(opt.decl, field)) return false;

                    auto contract = mir::fieldContract(opt.decl, made.occurrence.shape, field);
                    if(contract.form() != mir::AccessForm::View) return false;
                }

                return true;
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
 * Whether one instruction is the `Drop` of this allocation - the one writer `soleContents` admits,
 * for the reason `analysis.h` gives.
 */
bool isDropOf(OptContext& opt, Inst& inst, Value& allocation) {
    if(inst.kind != Inst::Drop) return false;

    auto address = opt.base[((InstDrop&)inst).address.value()];
    return allocationRoot(opt, address) == &allocation;
}

/*
 * Whether one instruction is a **hand-over out of** this allocation - a `load` whose result is
 * tracked, which takes the object out and leaves the storage empty (`verify.cpp`'s loan rule reads
 * it the same way).
 *
 * Admitted beside the drop and on the drop's own argument: it ends the life of what the storage
 * held, so nothing may read the storage after it without something re-filling it first, and a
 * re-fill is the second write the rule has already excluded. It is also what a drop *becomes* since
 * J137 - `opt/teardown.cpp` writes an authored teardown as `load` and `call` - so a rule that
 * admitted the one and not the other would have stopped answering a string literal's reads at the
 * very rewrite that was meant to let its reclaim fold.
 */
bool isHandoverOf(OptContext& opt, Inst& inst, Value& allocation) {
    if(inst.kind != Inst::Load || !inst.results.size()) return false;
    if(!isTracked(opt.base[inst.results.get(opt.base, 0)]->convention)) return false;

    auto address = opt.base[((InstLoad&)inst).address.value()];
    return allocationRoot(opt, address) == &allocation;
}

// Whether a stored value is a `fun.value`, through the `represent`s a function value is restated
// across - the pair is one word whichever reading the store was written at.
bool isFunValue(OptContext& opt, Value* value) {
    for(U32 step = 0; value && value->definer && step < 4; step++) {
        auto inst = opt.base[value->definer];
        if(inst->kind == Inst::FunValue) return true;
        if(inst->kind != Inst::Unary || ((InstUnary*)inst)->op != mir::UnaryOp::Represent) return false;

        value = opt.base[((InstUnary*)inst)->value.value()];
    }

    return false;
}

/*
 * The one whole-slot `Store` into this allocation of a constant, an `Aggregate`, a `fun.value` or a
 * value moved out of other storage, and null where there is no such store or where the allocation
 * is written whole more than once.
 *
 * The **direct** use list is enough for the store to be found, because a whole-slot write names the
 * allocation itself; a write through a projection is a different instruction and is what
 * `ReadWalk` catches.
 */
Inst* soleFill(OptContext& opt, Value& allocation) {
    Inst* fill = nullptr;

    for(auto userPointer: allocation.uses(opt.base)) {
        auto& user = *opt.base[userPointer];
        if(user.kind != Inst::Store) continue;
        if(opt.base[((InstStore&)user).address.value()] != &allocation) continue;

        if(fill) return nullptr;

        auto stored = opt.base[((InstStore&)user).value.value()];
        if(!stored->definer) return nullptr;

        // A tracked load is the move chain the conversion writes - `store %xs (load %literal)` -
        // and is followed by the readers that walk values (the site estimate, `funValueBehind`);
        // `forwardMemory` reads an `Aggregate` or a constant out of a fill and nothing else.
        auto definer = opt.base[stored->definer];
        auto moved = definer->kind == Inst::Load && isTracked(stored->convention);
        auto kind = definer->kind;
        if(kind != Inst::Const && kind != Inst::Aggregate && !moved && !isFunValue(opt, stored)) {
            return nullptr;
        }

        fill = &user;
    }

    return fill;
}

/*
 * Whether every use reached from `address` - a projection chain of `allocation` - leaves the storage
 * as it was, apart from `fill`, the drops and the hand-overs `soleContents` admits. The escape rule
 * of the whole query; see the header.
 *
 * It follows every name `Contained` follows - a projection, a `Guaranteed` load, a `represent` and
 * a block argument an edge fills with one - so that what the containment walk admitted as a second
 * name is a name this walk asks about rather than a refusal. The edge is the one that mattered for
 * a string literal: a borrow of the slot that crosses a join (`ArmBinding.yana`'s arms) was a
 * refusal here where the body scan this replaced had answered it.
 */
struct ReadWalk {
    OptContext& opt;
    const Contained& contained;
    Value& allocation;
    Inst& fill;

    // The block arguments already entered - an edge round a back edge names the argument it came
    // from, and a name already being walked is being answered by the frame that entered it.
    SmallArray<Value*, 8> entered;

    bool only(Value& address, U32 depth) {
        if(depth > 6) return false;

        for(auto userPointer: address.uses(opt.base)) {
            auto& user = *opt.base[userPointer];
            if(&user == &fill || isDropOf(opt, user, allocation) || isHandoverOf(opt, user, allocation)) {
                continue;
            }

            switch(user.kind) {
                case Inst::FieldAddress:
                case Inst::ElementAddress:
                case Inst::Load: {
                    // A load hands on a *borrow* of the storage where its result is `Guaranteed`,
                    // and that is followed like a projection - into the call it is lent to, which
                    // is where a `&f` parameter writes; a trivial load is a read and ends here.
                    if(mayWriteStorage(opt, contained, user, allocation)) return false;
                    if(!user.results.size()) continue;

                    auto result = opt.base[user.results.get(opt.base, 0)];
                    if(user.kind == Inst::Load && result->convention != Convention::Guaranteed) continue;
                    if(!only(*result, depth + 1)) return false;
                    continue;
                }

                // A `represent` restates the same bytes at another type, and a function value is
                // carried across one between its three readings (the pair, a borrow of it, a
                // pointer to it). A second name, followed like a projection; any other unary of an
                // address is one `Contained` has already refused.
                case Inst::Unary: {
                    if(((InstUnary&)user).op != mir::UnaryOp::Represent) continue;
                    if(!user.results.size()) continue;

                    if(!only(*opt.base[user.results.get(opt.base, 0)], depth + 1)) return false;
                    continue;
                }

                // A name handed over an edge is the block argument at the other end of it. A
                // write through that argument roots nowhere, which `mayWriteStorage` answers
                // through `addressMayName`, so every user of the argument is asked here.
                case Inst::Jump:
                case Inst::Branch:
                case Inst::Switch: {
                    auto held = forEachEdgeSlotOf(opt, user, address, [&](Value& argument) {
                        for(auto known: entered) if(known == &argument) return true;

                        entered.push(&argument);
                        return only(argument, depth + 1);
                    });

                    if(!held) return false;
                    continue;
                }

                default:
                    if(mayWriteStorage(opt, contained, user, allocation)) return false;

                    // A value that carries the address on - a call's result - is a second name
                    // this walk does not follow.
                    for(auto resultPointer: user.results.contents(opt.base)) {
                        auto result = opt.base[resultPointer];

                        auto read = false;
                        for(auto use: result->uses(opt.base)) { (void)use; read = true; break; }
                        if(!read) continue;

                        // A comparison or an arithmetic result is a number. A call's is a number
                        // too unless it is a loan or a raw pointer, which is J92's "a call's result
                        // is a second name for whatever its arguments named".
                        if(user.kind == Inst::Binary) continue;
                        if(user.kind != Inst::Call) return false;
                        if(result->convention == Convention::Guaranteed) return false;
                        if(opt.decl[result->occurrence.type]->kind == mir::Type::Pointer) return false;
                    }

                    if(isTerminator(user.kind)) return false;
                    continue;
            }
        }

        return true;
    }
};

} // namespace

Inst* soleContents(OptContext& opt, const Contained& contained, Value& allocation) {
    if(!allocation.definer || opt.base[allocation.definer]->kind != Inst::Alloc) return nullptr;
    if(!opt.base[allocation.definer]->block) return nullptr;
    if(!contained[&allocation]) return nullptr;

    auto fill = soleFill(opt, allocation);
    if(!fill) return nullptr;

    ReadWalk walk { opt, contained, allocation, *fill };
    return walk.only(allocation, 0) ? fill : nullptr;
}

namespace {

/*
 * Whether a `store` through this name of an allocation says what it wrote - `writtenHere`'s one
 * admitted writer. A store *into* a projection writes a field or an element, and its value is the
 * thing a later read is answered from; a store of the **whole** is the same only where the value is
 * built here, an `Aggregate` or a constant. A whole record moved in from other storage is a value
 * nothing at this tier can take apart, so reading it field by field is reading memory again.
 */
bool statedStore(OptContext& opt, InstStore& store, Value& address) {
    if(!store.address.isSet() || opt.base[store.address.value()] != &address) return false;

    auto definer = address.definer ? opt.base[address.definer] : nullptr;
    if(definer && (definer->kind == Inst::FieldAddress || definer->kind == Inst::ElementAddress)) {
        return true;
    }

    auto value = opt.base[store.value.value()];
    if(!value->definer) return false;

    auto kind = opt.base[value->definer]->kind;
    return kind == Inst::Aggregate || kind == Inst::Const;
}

/*
 * `writtenHere`'s walk over the names of one allocation - the address, its projections, and the
 * borrows (`Guaranteed` loads) and restatements taken of it. It fails at a store that does not say
 * what it wrote, at a callee handed a name it writes through, and at an atomic, and passes everything
 * else: the answer is a heuristic, so a name carried off into a record or across an edge is not
 * followed.
 */
bool namesWrittenHere(OptContext& opt, Value& address, U32 depth) {
    if(depth > 6) return false;

    for(auto userPointer: address.uses(opt.base)) {
        auto& user = *opt.base[userPointer];

        switch(user.kind) {
            case Inst::Store: {
                auto& store = (InstStore&)user;
                if(store.value.isSet() && opt.base[store.value.value()] == &address) return false;
                if(!statedStore(opt, store, address)) return false;
                continue;
            }

            // A callee handed a name at a position it writes through is where the contents came
            // from, as far as this walk can tell: the storage is somebody else's to fill.
            case Inst::Call: {
                auto& call = (InstCall&)user;
                if(!call.callee || call.through != mir::CallThrough::Declaration) return false;

                auto mutated = opt.decl[call.callee]->summary.mutates().get(mir::ArgumentSet::all());
                for(Size i = 0; i < call.arguments.size(); i++) {
                    auto used = call.arguments.get(opt.base, i);
                    if(used.isSet() && opt.base[used.value()] == &address && mutated.has(U32(i))) return false;
                }
                continue;
            }

            case Inst::Atomic:
                return false;

            case Inst::Load: {
                if(!user.results.size()) continue;

                auto result = opt.base[user.results.get(opt.base, 0)];
                if(result->convention != Convention::Guaranteed) continue;
                if(!namesWrittenHere(opt, *result, depth + 1)) return false;
                continue;
            }

            case Inst::FieldAddress:
            case Inst::ElementAddress:
            case Inst::Unary:
                if(!user.results.size()) continue;
                if(!namesWrittenHere(opt, *opt.base[user.results.get(opt.base, 0)], depth + 1)) return false;
                continue;

            default:
                continue;
        }
    }

    return true;
}

} // namespace

bool writtenHere(OptContext& opt, Value& allocation) {
    if(!allocation.definer || opt.base[allocation.definer]->kind != Inst::Alloc) return false;
    if(!opt.base[allocation.definer]->block) return false;

    return namesWrittenHere(opt, allocation, 0);
}

Inst* SoleContents::of(OptContext& opt, const Contained& contained, Value& allocation) {
    if(auto found = answers.getValue(allocation.id.index)) return found.unwrap();

    auto fill = soleContents(opt, contained, allocation);
    *answers.add(allocation.id.index).value = fill;
    return fill;
}

SoleContents& soleContentsOf(OptContext& opt) {
    if(!opt.soleStamp.holds(opt.function, opt.body->version, true)) {
        opt.soleContents.answers.clear();
        opt.soleStamp.take(opt.function, opt.body->version);
    }

    return opt.soleContents;
}

// The stored value's definer, for the filters below.
static Inst* filledWith(OptContext& opt, Inst* fill) {
    if(!fill) return nullptr;
    return opt.base[opt.base[((InstStore&)*fill).value.value()]->definer];
}

// A constant fill is `constantHeldBy`'s, and `forwardMemory` reads it through `constantAt`.
Inst* aggregateFillOf(OptContext& opt, const Contained& contained, Value& value) {
    auto fill = soleContents(opt, contained, value);
    auto definer = filledWith(opt, fill);
    return definer && definer->kind != Inst::Const ? fill : nullptr;
}

// Only a constant whose teardown is nothing, because the one reader erases the slot's drop.
mir::Constant constantHeldBy(OptContext& opt, const Contained& contained, Value& value) {
    auto definer = filledWith(opt, soleContents(opt, contained, value));
    if(!definer || definer->kind != Inst::Const) return {};

    auto constant = ((InstConst*)definer)->constant;
    return opt.program.teardownIsNothing(constant) ? constant : mir::Constant();
}

InstFunValue* funValueBehind(OptContext& opt, SoleContents& slots, const Contained& contained,
                             Value& target, bool& dropped) {
    static constexpr U32 kMaxSteps = 12;

    auto value = &target;

    for(U32 step = 0; step < kMaxSteps; step++) {
        if(!value->definer) return nullptr;

        auto inst = opt.base[value->definer];

        switch(inst->kind) {
            case Inst::FunValue:
                return (InstFunValue*)inst;

            case Inst::Unary:
                if(((InstUnary&)*inst).op != mir::UnaryOp::Represent) return nullptr;
                value = opt.base[((InstUnary&)*inst).value.value()];
                continue;

            case Inst::Load:
                value = opt.base[((InstLoad&)*inst).address.value()];
                continue;

            case Inst::Alloc: {
                auto fill = slots.of(opt, contained, *value);
                if(!fill) return nullptr;

                for(auto userPointer: value->uses(opt.base)) {
                    if(opt.base[userPointer]->kind == Inst::Drop) dropped = true;
                }

                value = opt.base[((InstStore*)fill)->value.value()];
                continue;
            }

            default:
                return nullptr;
        }
    }

    return nullptr;
}

/*
 * Whether the end of this value runs nothing - J138. A constant `mir::Program::teardownIsNothing`
 * says so of, a value that owes no teardown at all, or an `Aggregate` of such values whose type
 * leaves both halves to the derived walk (an authored `Drop` is an effect whatever the fields hold).
 *
 * It is `@caller`'s `Site` above all: every `check` builds one out of two string literals and two
 * numbers, and its drop was a `String.reclaim` per literal at every assertion in the program.
 */
bool owesNothing(OptContext& opt, Value& value, U32 depth) {
    if(!isTracked(value.convention)) return true;
    if(!value.definer || depth > 4) return false;

    auto definer = opt.base[value.definer];
    if(definer->kind == Inst::Const) return opt.program.teardownIsNothing(((InstConst*)definer)->constant);

    // A function value with no environment is `{thunk, null}`, and `teardown(fun)` of a null
    // environment does nothing - J145, J94 §6.2 E. A record literal holding a callback is the shape.
    if(definer->kind == Inst::FunValue) return !((InstFunValue*)definer)->capturing;

    if(definer->kind != Inst::Aggregate) return false;

    if(isLinearAt(opt, value.occurrence)) return false;
    auto ownership = mir::ownershipOf(opt.decl, value.occurrence.type);
    if(ownership.drop == mir::TeardownKind::Authored) return false;
    if(ownership.reclaim == mir::TeardownKind::Authored) return false;

    for(auto used: ((InstAggregate*)definer)->components.contents(opt.base)) {
        if(!used.isSet()) continue;
        if(!owesNothing(opt, *opt.base[used.value()], depth + 1)) return false;
    }

    return true;
}

/*
 * What is at an address or in a value where this body **built** it: the `Aggregate` or `Const` it
 * was built from, read back through the storage it was put in - J137, and the inliner's
 * `siteContents` until J172 moved it here for `opt/teardown.cpp` to ask of a drop's storage.
 *
 * The estimate the inliner seeds with it was seeded only with numbers, and the call that most needed
 * more was a teardown: `releaseRun(value.run)` handed a run the caller built on its own frame reads a
 * flag that is the constant zero in the caller's aggregate, and the whole of the callee is the arm
 * that flag decides against. Weighed as written it is the test and a copy of `freeHeap`, above the
 * budget; weighed at the site it is nothing.
 *
 * A slot is read through `aggregateFillOf`, which is what makes the answer one: its fill is the only
 * write for the whole body. The first form of this took a slot's one whole-slot store as its
 * contents without asking what else wrote it, and `MapLiteral.yana` grew by 7,733 lowered
 * instructions - each `insertAt` pushes through a borrow of the slot the literal filled, and a
 * capacity read as the literal's pruned every growth arm out of the estimate.
 */
Value* builtContents(OptContext& opt, Value* value, U32 depth) {
    if(!value || !value->definer || depth > 6) return nullptr;
    auto inst = opt.base[value->definer];

    switch(inst->kind) {
        case Inst::Aggregate:
        case Inst::Const:
            return value;

        case Inst::Load:
            return builtContents(opt, opt.base[((InstLoad*)inst)->address.value()], depth + 1);

        // Storage whose one write is the fill, and nothing else writes it for the whole body -
        // `aggregateFillOf`, and the reason this is an answer at all: a slot filled once and then
        // pushed to through a borrow holds nothing the fill says.
        case Inst::Alloc: {
            // One slot's answer rather than a table's: a splice moves the version, and the table
            // rebuilt per site was J137's compile time (`aggregateFillOf`).
            auto fill = aggregateFillOf(opt, containedOf(opt), *value);
            if(!fill) return nullptr;

            return builtContents(opt, opt.base[((InstStore*)fill)->value.value()], depth + 1);
        }

        case Inst::FieldAddress: {
            auto& projection = *(InstFieldAddress*)inst;
            auto record = builtContents(opt, opt.base[projection.base.value()], depth + 1);
            return builtContents(opt, builtComponent(opt, record, projection), depth + 1);
        }

        default:
            return nullptr;
    }
}

// The component of a built record a projection names, on `forwardMemory`'s terms for the same
// question: a single-constructor record, the type the projection is of, a plain field.
Value* builtComponent(OptContext& opt, Value* record, InstFieldAddress& projection) {
    if(!record || !record->definer) return nullptr;

    auto definer = opt.base[record->definer];
    if(definer->kind != Inst::Aggregate) return nullptr;
    if(projection.property != InstFieldAddress::kNoProperty) return nullptr;
    if(projection.field == InstFieldAddress::kDiscriminant) return nullptr;
    if(!projection.record || projection.record != record->occurrence.type) return nullptr;

    auto declared = opt.decl[projection.record];
    if(declared->kind != mir::Type::Record) return nullptr;
    if(((mir::RecordType*)declared)->layout != mir::RecordLayout::Single) return nullptr;

    auto& built = (InstAggregate&)*definer;
    if(built.constructor != 0 || projection.field >= built.components.size()) return nullptr;

    auto used = built.components.get(opt.base, projection.field);
    return used.isSet() ? opt.base[used.value()] : nullptr;
}

/*
 * Whether this allocation holds, for the whole body, a value whose end runs nothing - so a `drop`
 * of it, or of any part of it, may be erased: `soleContents` names a constant whose teardown is
 * nothing (`constantHeldBy`'s filter) or an aggregate that owes nothing (`owesNothing`).
 */
bool holdsNothingOwed(OptContext& opt, const Contained& contained, Value& value) {
    auto fill = soleContents(opt, contained, value);
    auto definer = filledWith(opt, fill);
    if(!definer) return false;

    if(definer->kind == Inst::Const) return opt.program.teardownIsNothing(((InstConst*)definer)->constant);

    auto stored = opt.base[((InstStore*)fill)->value.value()];
    return definer->kind == Inst::Aggregate && owesNothing(opt, *stored, 0);
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
     * fixture the shipping stage folded to one constant, and the difference was a thousand bytes in
     * one body.
     *
     * Sound on exactly the terms the two roots are: `constantAt` concludes nothing unless it reaches
     * a global `isWritten` says nothing writes, or an allocation `soleContents` says holds one
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

Maybe<mir::Constant> constantAt(OptContext& opt, SoleContents& slots, const Contained& contained,
                                Value* address) {
    SmallArray<U32, 8> path;

    while(address) {
        if(auto definer = address->definer) {
            auto inst = opt.base[definer];

            if(inst->kind == Inst::Alloc) {
                auto definer = filledWith(opt, slots.of(opt, contained, *address));
                if(!definer || definer->kind != Inst::Const) return Nothing();

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
        ? ((InstConst*)filledWith(opt, slots.of(opt, contained, *address)))->constant
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
bool namesOnlyParameters(OptContext& opt, Value* address);

bool addressMayReach(OptContext& opt, const Contained& contained, Use address, Value& allocation) {
    if(!address.isSet()) return true;

    auto value = opt.base[address.value()];
    auto root = allocationRoot(opt, value);
    if(root) return root == &allocation;

    // A borrow handed over an edge roots nowhere and still names the allocation - `addressMayName`.
    if(contained[allocation]) return addressMayName(opt, value, allocation);

    /*
     * **A parameter's storage is older than every allocation of this body**, contained or not - K4.
     * An address that is nothing but names of an entry argument (fields, `Guaranteed` loads,
     * `represent`, edges back to one) is inside storage the caller made before this activation
     * began, so it is not an allocation the body makes. Containment is not the question: a slot whose
     * borrow was copied into a second carrier (`LimitedInput.take`'s `Ok(run)` of the first
     * `take`'s payload, J203's G1) escapes, and without this `self.left -= count` between the two
     * was a write to it. A pointer *loaded* out of the parameter's storage ends the chain at a load
     * that is not a name, and stays permissive.
     */
    return !namesOnlyParameters(opt, value);
}

// Whether every chain of names behind `address` ends at an argument of the body's entry block.
bool namesOnlyParameters(OptContext& opt, Value* address) {
    SmallArray<Value*, 8> pending;
    SmallArray<Value*, 8> seen;
    pending.push(address);

    while(pending.size()) {
        auto at = pending.pop().unwrap();
        while(at && at->definer) {
            auto next = addressStep(opt, at);
            if(!next) return false;
            at = next;
        }

        if(!at || !at->block) return false;

        auto block = opt.base[at->block];
        if(block == opt.base[opt.body->entry]) continue;

        auto known = false;
        for(auto visited: seen) known = known || visited == at;
        if(known) continue;
        if(seen.size() >= 32) return false;
        seen.push(at);

        Size position = 0;
        while(position < block->argumentCount()) {
            if(opt.base[block->argumentAt(opt.base, position)] == at) break;
            position++;
        }

        auto found = false;
        for(auto predecessor: block->incoming(opt.base)) {
            auto terminator = opt.base[predecessor]->terminator(opt.base);
            if(!terminator) return false;

            forEachEdge(opt.base, *terminator, [&](Edge& edge) {
                if(opt.base[edge.target] != block || position >= edge.arguments.size()) return;

                auto used = edge.arguments.get(opt.base, position);
                if(used.isSet()) {
                    pending.push(opt.base[used.value()]);
                    found = true;
                }
            });
        }

        if(!found) return false;
    }

    return true;
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

        /*
         * A value built out of values - it consumes its components and writes no storage at all.
         * Not `kSsaPure` only because of the consume, so the default below made it a barrier for
         * every allocation that is not contained: `length(stack)` builds an `ArrayView` between two
         * reads of the length, and the second was never answered by the first (J153, `Sort`).
         */
        case Inst::Aggregate:
            return false;

        // An edge hands values to a block and writes nothing - which only mattered once a block's
        // facts could outlive it (`forwardMemory`'s `ends`, J153).
        case Inst::Jump:
        case Inst::Branch:
        case Inst::Switch:
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
         * **not** contained is one it may have reached by some other route. It is a barrier where
         * the callee writes through an argument, or where it can reach global state at all
         * (`mir::reachesGlobalState`, J113). The second is the route that is not an argument: an
         * address kept in a global is written by a callee that takes nothing (`store(g.p, 7)`), and
         * `mutates` is a row over argument positions, so it reads `none` there
         * (`test/resolve/GlobalPointerWrite.yana`). Only a raw pointer puts a local's address in a
         * global - the loan checker refuses it for a reference - so a callee reaching no global
         * keeps the forwarding across it. A specialization is asked with its type arguments'
         * teardowns, which its generic's row leaves to the caller: `consume$Noisy` over a `Drop`
         * that stores through a pointer kept in a global reaches it (J121). What the fact cannot tell apart is a callee reading a
         * writable heap global from one reading a local's address out of one, which is why 17 of
         * the goldens J110 moved stayed moved: their calls reach `String.reclaim`, the allocator or
         * `print`.
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
            auto direct = call.callee && call.through == mir::CallThrough::Declaration;
            if(!direct && !contained[allocation]) return true;

            // A call that runs something this body cannot name writes through every argument it
            // was handed - and through nothing else of a contained allocation: its target is the
            // pair it reads, which `Contained` admits for that reason (J148), not an address the
            // callee is given.
            auto mutated = direct
                ? opt.decl[call.callee]->summary.mutates().get(mir::ArgumentSet::all())
                : mir::ArgumentSet::all();
            if(!contained[allocation]) return mutated.any() || mir::reachesGlobalState(opt.program, call.callee);

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
