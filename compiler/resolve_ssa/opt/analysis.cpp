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

/*
 * Whether an address this body cannot root in an allocation was **read out of storage**.
 *
 * The other half of `allocationRoot`'s null: a chain that ends at a `Load` is a pointer that was
 * sitting in memory, where one ending at a global's address, at a block argument or at an unsafe
 * cast is not. The distinction is worth making because a contained allocation's address never
 * reaches memory - see `loadedAddressMisses`.
 *
 * A `Guaranteed` load is not an end of the chain at all - `addressStep` walks through it, because it
 * names the same storage its operand does - so what stops here is a load of a pointer *out of* a
 * slot, which is exactly the value the claim is about.
 *
 * A **`Binary` add** is walked through where one side is a pointer, and that is the one step this
 * makes that `allocationRoot` does not. Pointer arithmetic stays inside the object it started in -
 * the same claim `ElementAddress` states as an instruction, restated for the spelling `lib/`'s own
 * subscript uses, where the element address is `elementBase(xs) + i * stride` written in source.
 */
bool addressIsLoaded(OptContext& opt, Value* address) {
    while(address) {
        auto definer = address->definer;
        if(!definer) return false;

        auto inst = opt.base[definer];

        if(inst->kind == Inst::Load) {
            if(!inst->results.size()) return false;
            if(opt.base[inst->results.get(opt.base, 0)]->convention != Convention::Guaranteed) {
                return true;
            }
        }

        if(inst->kind == Inst::Binary) {
            auto& binary = (InstBinary&)*inst;
            if(binary.op != mir::BinaryOp::Add) return false;

            auto lhs = opt.base[binary.lhs.value()];
            auto rhs = opt.base[binary.rhs.value()];
            auto isPointer = [&](Value* value) {
                return value->occurrence.type &&
                       opt.decl[value->occurrence.type]->kind == mir::Type::Pointer;
            };

            if(isPointer(lhs) && !isPointer(rhs)) address = lhs;
            else if(isPointer(rhs) && !isPointer(lhs)) address = rhs;
            else return false;

            continue;
        }

        auto next = addressStep(opt, address);
        if(!next) return false;

        address = next;
    }

    return false;
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

    // Recursive rather than a worklist with a visited set, and the shape of the thing walked is
    // why: an address chain is a tree of pure projections rooted at one allocation, so a cycle in
    // it would be a value defined below its own operand - which is what `checkUse` reports.
    bool escapes(Value& address) {
        for(auto userPointer: address.uses(opt.base)) {
            auto& user = *opt.base[userPointer];
            if(!admits(user, address)) return true;
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
                if(!call.callee || call.through != mir::CallThrough::Declaration) return false;
                if(named(call.target)) return false;

                auto kept = opt.decl[call.callee]->summary.escapes().get(mir::ArgumentSet::all());

                for(Size i = 0; i < call.arguments.size(); i++) {
                    if(!named(call.arguments.get(opt.base, i))) continue;
                    if(kept.has(U32(i))) return false;
                }

                return true;
            }

            // And no default that admits. An aggregate component, a native operand, a returned
            // value, an edge operand and a captured environment are each a way for the address to
            // outlive the instruction naming it, and a kind nobody has looked at is one whose effect
            // on the address has not been checked.
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

Contained& containedOf(OptContext& opt) {
    if(!opt.containedStamp.holds(opt.function, opt.body->version, true)) {
        opt.contained.build(opt);
        opt.containedStamp.take(opt.function, opt.body->version);
    }

    return opt.contained;
}

namespace {

/*
 * A **contained** allocation's storage cannot be named by a pointer that came out of memory.
 *
 * Worth stating as a claim rather than as a case, because it is the whole of the fact. `Contained`
 * says every use of the allocation's address keeps that address in this frame: it is refused as a
 * `Store` *value*, as an aggregate component, as a native operand, as a returned value, as an edge
 * operand and as a captured environment, and admitted at a call argument only where the callee's
 * `escapes` row says no address of that position's storage was written anywhere. So the address is
 * never **written anywhere** - and a `Load` therefore cannot produce it, nor a projection of it, since a projection
 * is refused wherever the address itself is.
 *
 * What it buys is the shape every container has. `countPrimes` holds its `Array(Bool)` header in a
 * frame slot; the marking loop loads the run pointer out of that header and stores through it every
 * step. The run and the header are different storage and the run pointer was *loaded from* the
 * header, so no store through it can write the header back - which is what lets `hoist` move the
 * base and the length into the preheader instead of re-reading both every iteration.
 *
 * Deliberately a statement about the *allocation* rather than about the two addresses. Two loaded
 * pointers say nothing about each other, and neither does a loaded pointer against an allocation
 * that escapes - so containment is the premise, and the address chain only says where it came from.
 */
bool loadedAddressMisses(OptContext& opt, const Contained& contained, Value* address,
                         Value& allocation) {
    return contained[allocation] && addressIsLoaded(opt, address);
}

// Whether an address may name bytes inside this allocation. Two addresses rooted in two different
// allocations cannot; neither can a pointer read out of memory and an allocation whose address was
// never written anywhere. An address that is neither may name anything, which is the permissive
// direction and the only sound one for a pointer.
bool addressMayReach(OptContext& opt, const Contained& contained, Use address, Value& allocation) {
    if(!address.isSet()) return true;

    auto value = opt.base[address.value()];
    auto root = allocationRoot(opt, value);
    if(root) return root == &allocation;

    return !loadedAddressMisses(opt, contained, value, allocation);
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
            if(isPure(inst.kind)) return false;
            return !contained[allocation];
    }
}

} // namespace ssa
