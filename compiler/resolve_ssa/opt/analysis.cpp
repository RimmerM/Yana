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

Value* allocationRoot(OptContext& opt, Value* address) {
    while(address) {
        auto definer = address->definer;
        if(!definer) return nullptr;

        auto inst = opt.base[definer];
        switch(inst->kind) {
            case Inst::Alloc:
                return address;
            case Inst::FieldAddress:
                address = opt.base[((InstFieldAddress*)inst)->base.value()];
                break;
            case Inst::ElementAddress:
                address = opt.base[((InstElementAddress*)inst)->base.value()];
                break;
            default:
                // A global's address, a pointer loaded out of storage, an address the unsafe
                // boundary produced. None of them is an allocation of this body, and the permissive
                // answer is the only sound one for all three.
                return nullptr;
        }
    }

    return nullptr;
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
            case Inst::FieldAddress:
                if(!named(((InstFieldAddress&)user).base)) return false;
                return !user.results.size() || !escapes(*opt.base[user.results.get(opt.base, 0)]);
            case Inst::ElementAddress: {
                auto& element = (InstElementAddress&)user;
                if(!named(element.base) || named(element.index)) return false;
                return !user.results.size() || !escapes(*opt.base[user.results.get(opt.base, 0)]);
            }

            // And no default that admits. A call argument, an aggregate component, a native operand,
            // a returned value, an edge operand and a captured environment are each a way for the
            // address to outlive the instruction naming it, and a kind nobody has looked at is one
            // whose effect on the address has not been checked.
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

// Whether an address may name bytes inside this allocation. Two addresses rooted in two different
// allocations cannot; an address with no root of this body's may name anything, which is the
// permissive direction and the only sound one for a pointer.
bool addressMayReach(OptContext& opt, Use address, Value& allocation) {
    if(!address.isSet()) return true;

    auto root = allocationRoot(opt, opt.base[address.value()]);
    return !root || root == &allocation;
}

} // namespace

bool mayWriteStorage(OptContext& opt, const Contained& contained, Inst& inst,
                     Value& allocation) {
    switch(inst.kind) {
        case Inst::Store:
            return addressMayReach(opt, ((InstStore&)inst).address, allocation);

        /*
         * The **handover**: a destructive read takes the object out and leaves the slot empty, which
         * is a write of the storage however it is spelled. It is also one of the four events that
         * end a loan - `resolve_ssa/README.md` - and the two facts are the same fact: what a borrow
         * of the slot would be reading afterwards is storage the program has emptied.
         */
        case Inst::Load: {
            if(!inst.results.size()) return false;
            if(!isTracked(opt.base[inst.results.get(opt.base, 0)]->convention)) return false;

            return addressMayReach(opt, ((InstLoad&)inst).address, allocation);
        }

        // A teardown receives an address and may write through it, so a drop is a read *and* a write
        // of its own storage. What it may not reach is an allocation whose address it was never
        // given, which is the opaque case below.
        case Inst::Drop:
            if(addressMayReach(opt, ((InstDrop&)inst).address, allocation)) return true;
            return !contained[allocation];

        case Inst::Atomic: {
            auto& atomic = (InstAtomic&)inst;
            if(!atomic.arguments.size()) return true;

            return addressMayReach(opt, atomic.arguments.get(opt.base, 0), allocation);
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
         * The opaque kinds, and the one fact that answers them.
         *
         * A callee cannot write storage whose address it was never given, and `Contained` is exactly
         * that statement about this allocation. Where the allocation escapes, what would answer
         * instead is the callee's own `mutates` row - read here through the `Fact` API so that
         * filling it later changes nothing at this site. It is `Unknown` today, so `get` hands back
         * the caller's conservative answer and every argument counts as written.
         */
        case Inst::Call: {
            if(contained[allocation]) return false;

            auto callee = ((InstCall&)inst).callee;
            if(!callee) return true;

            /*
             * `Unknown` reads back as `ArgumentSet::all()` - every argument written, and therefore
             * a barrier. The read is here rather than a constant `true` so that the day
             * `computeSummaries` fills the row, a callee that writes through nothing stops being one
             * without this file changing; and the judgement about storage a callee reaches *without*
             * being handed it belongs with the row rather than with this reader.
             */
            return opt.decl[callee]->summary.mutates().get(mir::ArgumentSet::all()).any();
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
