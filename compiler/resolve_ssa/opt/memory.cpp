#include "pass.h"

/*
 * Storage: a read answered by a write above it, a second read answered by the first, a write nothing
 * reads, and an allocation nothing reads at all - README.md's section A rows for `forwardPlaces` and
 * `scalarizeLocals`.
 *
 * Two passes rather than one, and section B is the division: `forwardMemory` carries a fact *across*
 * instructions, which is a scan with state and is what a worklist over values cannot express, while
 * `removeDeadStorage` asks `hasTeardown` of a held occurrence and deletes a `Drop` with an `Alloc`,
 * which is an ownership edit. They are in one file because the second exists to collect what the
 * first leaves behind: forwarding is what makes an allocation's reads go away, and an allocation
 * with no reads is what the second removes.
 *
 * ## Why this is answerable here at all
 *
 * At this tier "can these two accesses touch the same storage" is **structural**. An address is an
 * `Alloc` plus a chain of `FieldAddress`/`ElementAddress`, two different fields of one record are
 * two different pieces of storage by construction, and two different allocations are two different
 * pieces of storage with nothing to prove. Below the fork the same question is pointer
 * disambiguation over `add ptr, imm`. This is the tier where the ownership model pays for itself as
 * an optimization, and it is the shipping stage's `opt_place.cpp` argument arriving with `Place`
 * replaced by an address value.
 *
 * Two rules, both of them stated once and both of them in `resolve_ssa/README.md`'s decision table:
 *
 * **Whether two addresses are the same one.** They are the same SSA value; or they are
 * `FieldAddress` chains on the same base naming the same record and the same field; or
 * `ElementAddress` chains on the same base and the same container whose indices are the same value
 * or two constants that agree. Nothing else is the same address, and in particular a `Deref` - a
 * pointer loaded out of storage - is not the same as itself, because two loads of one slot are two
 * addresses this pass has nothing to say about. `simplify`'s block-local CSE has usually already
 * unified the two spellings of one projection, so the common case is value identity.
 *
 * **Whether two addresses may alias.** They may not when they are rooted in two *different*
 * allocations of this body, or in one allocation on paths that provably separate: a different field
 * of the same record at the same depth, or two constant element indices that differ. Everything else
 * may alias - an address with no allocation root of this body's names storage this pass cannot
 * bound, and a chain whose steps diverge at an index it cannot put a number on is two elements it
 * cannot tell apart.
 *
 * A **prefix** is not a separation. A whole-record store kills every field of it, because the
 * comparison stops at the shorter path and everything up to there was the same step - which is what
 * makes `store %record` followed by a load of `%record.x` decline rather than forward.
 *
 * ## Loans, and the one thing this must not move
 *
 * A loan ends at four events, two of which are instructions this pass rewrites: a `store` to the
 * storage, and a `load` out of it whose result is tracked. Neither may be moved past a use of a
 * borrow on that loan.
 *
 * Nothing here moves one, and the reason is that **only a `Trivial` result is ever forwarded**. A
 * forward replaces a load's result by a value that was already standing above it, so no instruction
 * changes position; the only thing that changes is which value a reader names, and a `Trivial` value
 * names no storage and depends on no loan. A `Guaranteed` result is declined outright for that
 * reason: it *is* a borrow, and substituting one is `Editor::replaceAllUses`'s loan rule rather than
 * this pass's. What the two erasing rules remove - a dead store, a store of what is already there -
 * removes an endpoint rather than moving one, which can only make more IR verify and never less.
 *
 * ## The tracked load, and why it is declined
 *
 * `store %a, %v` followed by `%x = load %a` producing an `Owned` or `Affine` value is the
 * **handover**, and forwarding it is not a substitution: the value is consumed once either way, so
 * pointing the load's readers at `%v` requires erasing the store in the same rewrite, or `%v` is
 * consumed twice and the verifier says so. That much is mechanical. Three things above it are not,
 * and together they are the decision:
 *
 *  - the guard this pass has is the wrong guard. "Nothing in between may write it" is what a
 *    forwarded *value* needs; erasing the store means the slot is never written at all, so what the
 *    rewrite needs is that nothing in between **reads** it either - and a read is exactly what a
 *    `Copy`, a `field.addr` and a load through it, a `Drop`, or a call handed the address is;
 *  - what makes it sound *after* the load is not a rule this tier states. A destructive read empties
 *    the slot, so nothing may read it again before something re-fills it - which is MIR's move-path
 *    invariant, checked over there and nowhere here. A rewrite resting on it would be one whose
 *    failure mode is a stale read that verifies;
 *  - and what it buys is small. The shape is `let ->y = x` over a value that lives in storage, where
 *    both ends are address-only and the lowering hands the same address to both, so what the forward
 *    removes is a copy the manifestation has usually already elided.
 *
 * So a tracked load is declined, and `removeDeadStorage` declines a tracked stored value for the
 * matching reason: erasing a store of an owner leaves it unconsumed, and the instruction that would
 * discharge it is a `Drop`, which takes an address - the address of the storage being removed.
 */

namespace ssa {

namespace {

/*
 * One fact about one piece of storage: what is at an address, and the store that put it there.
 *
 * A store and a load establish the same fact and are not kept apart - "this address holds %v" is
 * what a later load and a later store of `%v` both rest on. `pending` is what separates them: it
 * names the store while nothing has read the slot back, and it is null for a fact a *load*
 * established, because there is no write to remove. Clearing it on a read is what keeps the
 * dead-store rule from deleting a write something saw.
 */
struct Known {
    Value* address = nullptr;
    Value* value = nullptr;
    Inst* pending = nullptr;

    /*
     * Which first-level fields of an `Aggregate` value here have been **written since**, one bit per
     * field index - see `Forwarder::narrowWritten`.
     *
     * A whole-record fact survives a write to one of its fields, minus that field. Without it a
     * record filled by a literal and then updated in place answered no field read at all: the write
     * to `.left` prefix-aliases the record, so the fact went, and the read of `.right` two
     * instructions later had nothing behind it. Zero for every entry that is not an aggregate, which
     * is nearly all of them, and a field past the width of this mask keeps the old behaviour of
     * dropping the entry outright.
     */
    U32 stale = 0;
};

struct Forwarder {
    OptContext& opt;

    /*
     * Read once at the top of the pass and held across its own rewrites - `analysis.h`'s discipline,
     * and the reason `mayWriteStorage` takes it rather than asking for it.
     *
     * Holding it is sound in the direction this pass edits in: every erase below removes a *use* of
     * an allocation, and removing a use can only take one out of escaping. So an allocation this
     * reading calls escaped may have become contained by the end, which costs a forward the next
     * round picks up, and one it calls contained cannot have become anything else.
     */
    const Contained& contained;

    /*
     * What is settled before the program runs, read on the same terms and held for the same reason.
     *
     * The block table below is what one write above a read establishes; this is what a *slot* holds
     * for the whole of its life, which is the fact a call in between destroys and the one a string
     * literal's teardown needs. Two tables rather than one because they are two facts with two
     * lifetimes - one is emptied at every barrier and the other cannot be - and one rule reads them
     * in order (`tryForward`).
     */
    const ConstantContents& constants;

    // One block's worth. Inline, because a block holds a handful of distinct pieces of storage and
    // this is emptied at every block boundary and at every barrier.
    SmallArray<Known, 16> known;

    /*
     * Whether a value may stand for what a load produced.
     *
     * `Trivial` on both sides and nothing else - the file comment's loan argument is the whole of
     * the reason, and `Editor::replaceAllUses` refuses the other three anyway. The occurrences have
     * to agree as well as the conventions, because a value's occurrence is what the lowering
     * manifests it from: two values of one logical type can be an owner and a shared window into
     * one, and answering a read of the first with the second is handing the reader a different
     * representation.
     */
    bool isForwardable(Value& from, Value& to) {
        if(from.convention != Convention::Trivial) return false;
        if(to.convention != Convention::Trivial) return false;

        return from.occurrence == to.occurrence;
    }

    /*
     * The address an instruction writes storage through, where that address is **all** it writes -
     * and null for everything else, including everything that writes more than it names.
     *
     * The three kinds here are the ones whose effect on storage is exactly the place they were given:
     * a `Store`, an `Atomic` on its location, and the handover `Load`, which empties the slot it
     * reads. What is deliberately *not* here is a `Drop`: a teardown runs somebody else's code, so
     * naming an address bounds what it writes only for an allocation nothing else can reach, and
     * that is `mayWriteStorage`'s question rather than this one. A `Call` and a `Native` name nothing
     * at all.
     *
     * This is the narrowing half of `forgetWritten` and only the narrowing half. Widening a kill is
     * `mayWriteStorage`'s, and keeping the two apart is what stops an address that provably
     * separates from answering a question about an allocation whose address escaped.
     */
    Value* writtenThrough(Inst& inst) {
        auto through = [&](Use used) {
            return used.isSet() ? opt.base[used.value()] : nullptr;
        };

        switch(inst.kind) {
            case Inst::Store:
                return through(((InstStore&)inst).address);

            case Inst::Load: {
                if(!inst.results.size()) return nullptr;
                if(!isTracked(opt.base[inst.results.get(opt.base, 0)]->convention)) return nullptr;

                return through(((InstLoad&)inst).address);
            }

            case Inst::Atomic: {
                auto& atomic = (InstAtomic&)inst;
                return atomic.arguments.size() ? through(atomic.arguments.get(opt.base, 0)) : nullptr;
            }

            default:
                return nullptr;
        }
    }

    /*
     * Whether two addresses name the same storage - the first of the two rules.
     *
     * Value identity first, which is nearly always the answer: `simplify`'s block-local CSE has
     * already unified the two `field.addr` instructions a read and a write of one field are. What
     * this adds is the pair the CSE could not reach, which is a projection whose base is itself a
     * projection built twice.
     */
    bool sameAddress(Value* left, Value* right) {
        while(left && right) {
            /*
             * The second names taken off first - `throughNames`, and the same pair `allocationRoot`
             * walks. A borrow of a slot and the slot are one piece of storage, so a fact recorded
             * under one of them answers a read through the other; without this every write through a
             * `&` parameter established a fact nothing could find again, because the write named the
             * borrow and the read named the slot.
             */
            left = throughNames(left);
            right = throughNames(right);
            if(!left || !right) return false;

            if(left == right) return true;

            Value* leftBase = nullptr;
            Value* rightBase = nullptr;
            auto a = addressStepOf(opt, *left, leftBase);
            auto b = addressStepOf(opt, *right, rightBase);
            if(!a || !b || !sameAddressStep(opt, a.unwrap(), b.unwrap())) return false;

            left = leftBase;
            right = rightBase;
        }

        return false;
    }

    /*
     * Whether two addresses may name overlapping storage - the second rule.
     *
     * The two paths are walked back to their roots and compared from the roots down, because that is
     * the direction the question is asked in: a step separates two addresses only if everything
     * above it was the same step. Where the two roots are different allocations of this body there
     * is nothing to compare at all.
     */
    bool mayAlias(Value* left, Value* right) {
        if(left == right) return true;

        SmallArray<AddressStep, 8> leftPath;
        SmallArray<AddressStep, 8> rightPath;

        auto walk = [&](Value* address, SmallArray<AddressStep, 8>& path) {
            while(address) {
                Value* base = nullptr;
                auto step = addressStepOf(opt, *address, base);
                if(!step) return address;

                path.push(step.unwrap());
                address = base;
            }

            return address;
        };

        auto leftRoot = walk(left, leftPath);
        auto rightRoot = walk(right, rightPath);

        /*
         * Two different allocations are two pieces of storage. Anything else may meet: a global's
         * address, a pointer loaded out of storage and a borrowed parameter are all roots this pass
         * cannot bound, and the same root reached two ways is the same root.
         */
        if(leftRoot != rightRoot) {
            auto leftAlloc = allocationRoot(opt, leftRoot);
            auto rightAlloc = allocationRoot(opt, rightRoot);
            if(!leftAlloc || !rightAlloc) return true;
            if(leftAlloc != rightAlloc) return false;
        }

        // From the root down, which is the order the paths were pushed in reverse. A prefix is not a
        // separation: the walk stops at the shorter path, and everything up to there having been the
        // same step is what makes a whole-record write reach every field of it.
        auto depth = min(leftPath.size(), rightPath.size());
        for(Size i = 0; i < depth; i++) {
            auto& a = leftPath[leftPath.size() - 1 - i];
            auto& b = rightPath[rightPath.size() - 1 - i];

            if(separateAddressStep(opt, a, b)) return false;
            if(!sameAddressStep(opt, a, b)) return true;
        }

        return true;
    }

    /*
     * Whether an instruction names, at any operand, a value rooted in the same allocation as this
     * address - the read half, which is what makes a store no longer removable.
     *
     * Blunt on purpose: a store's *value* may be an address-only value whose bytes the lowering
     * copies out of storage, so "which operands are addresses" is not a question with a safe answer
     * here, and the allocation rather than the address is what this is asked about.
     *
     * Edge operands are not walked, and that costs nothing rather than being an omission: the only
     * instruction that has one is a terminator, and the table is emptied at every block boundary, so
     * a fact this failed to mark read is a fact nothing reaches afterwards.
     */
    bool touches(Inst& inst, Value* address) {
        auto root = allocationRoot(opt, address);
        if(!root) return true;

        // `addressMayName` rather than a root comparison, since a borrow of the allocation handed
        // over an edge roots nowhere and reads it all the same.
        auto found = false;
        forEachUse(opt.base, inst, [&](Use used) {
            if(!used.isSet() || found) return;
            if(addressMayName(opt, opt.base[used.value()], *root)) found = true;
        });

        return found;
    }

    /*
     * Everything this instruction may have written, taken out of the table - and everything it may
     * have read, left in it with its store no longer removable.
     *
     * Two questions in that order, and the order is the whole of it. `mayWriteStorage` is the
     * **allocation-level** one: can this instruction reach the storage at all, by any route. Only
     * where it says yes does the **address-level** one narrow the answer, and only for the kinds
     * `writtenThrough` admits - the ones whose whole effect on storage is the place they were given.
     * A store through one field of a record reaches the allocation and not the entry for its
     * neighbour; a call, a native and a teardown reach whatever the address they were handed some
     * instructions ago still names, which is why neither narrows.
     *
     * An entry with no allocation root of this body's has only the first question, and it answers
     * yes to every instruction that is not pure.
     */
    void forgetWritten(Inst& inst) {
        if(isPure(inst.kind)) return;

        auto written = writtenThrough(inst);

        for(Size i = known.size(); i-- > 0;) {
            auto root = allocationRoot(opt, known[i].address);

            if(!root || mayWriteStorage(opt, contained, inst, *root)) {
                if(!root || !written || mayAlias(known[i].address, written)) {
                    U32 field = 0;

                    /*
                     * One field of a record whose literal is still in hand, marked rather than
                     * forgotten - and the store that filled the record is no longer removable, since
                     * part of what it wrote is now what the program wants.
                     */
                    if(root && written && narrowWritten(known[i], written, field)) {
                        known[i].stale |= U32(1) << field;
                        known[i].pending = nullptr;
                        continue;
                    }

                    known.remove(i);
                    continue;
                }
            }

            if(touches(inst, known[i].address)) known[i].pending = nullptr;
        }
    }

    /*
     * Whether a write that reaches one entry's record touches only **one field** of it, and which -
     * the narrowing `Known::stale` exists for.
     *
     * The entry has to hold an `Aggregate`, because that is the only value whose fields this pass can
     * still name one at a time; and the write has to be a field address taken directly of the entry's
     * own storage, so a nested projection or an element is not this and drops the entry as before.
     * The single-constructor rule is `separateFields`', arriving here for its own reason: two indices
     * of a sum may be one piece of payload storage, so writing one says nothing about the other.
     */
    bool narrowWritten(Known& entry, Value* written, U32& field) {
        if(!entry.value || !entry.value->definer) return false;
        if(opt.base[entry.value->definer]->kind != Inst::Aggregate) return false;
        if(!written) return false;

        Value* base = nullptr;
        auto step = addressStepOf(opt, *written, base);
        if(!step || !base) return false;

        auto projection = step.unwrap();
        if(projection.kind != Inst::FieldAddress) return false;
        if(projection.property != InstFieldAddress::kNoProperty) return false;
        if(projection.field == InstFieldAddress::kDiscriminant) return false;
        if(projection.field >= 32) return false;
        if(!projection.of || projection.of != entry.value->occurrence.type) return false;

        auto declared = opt.decl[projection.of];
        if(declared->kind != mir::Type::Record) return false;
        if(((mir::RecordType*)declared)->layout != mir::RecordLayout::Single) return false;

        // `sameAddress` takes the second names off both sides itself, so the write may name a
        // borrow of the storage the entry was recorded under.
        if(!sameAddress(base, entry.address)) return false;

        field = projection.field;
        return true;
    }

    // One entry per piece of storage, so that "what is here" and "which store put it there" are one
    // answer rather than the most recent of several.
    void remember(Value* address, Value* value, Inst* pending) {
        for(Size i = known.size(); i-- > 0;) {
            if(sameAddress(known[i].address, address)) known.remove(i);
        }

        known.push(Known { address, value, pending, 0 });
    }

    Known* entryFor(Value* address) {
        for(Size i = known.size(); i-- > 0;) {
            if(sameAddress(known[i].address, address)) return &known[i];
        }

        return nullptr;
    }

    /*
     * A read answered by a value already in hand - the store-to-load and load-to-load rules, which
     * are one rewrite because the fact they read is one fact.
     *
     * A load that is answered this way stops being an access at all, so the store it would have read
     * stays removable - which is what makes a read-modify-write chain collapse rather than merely
     * shorten.
     */
    /*
     * A read whose answer was settled before the program ran - a literal's length, the pointer to
     * its bytes, the flag its teardown tests, a number read out of a placed constant.
     *
     * The second half of `tryForward` and tried after the block table, which is the cheaper and the
     * more specific of the two: a store standing above the read answers it exactly, and this answers
     * the reads that store no longer reaches - the ones behind a call, and the ones in another
     * block.
     *
     * **`Trivial` only, and the type has to agree.** A leaf of a constant is a number or an address;
     * a read that hands back a *record* is answered by the storage it names rather than by a value,
     * and substituting a constant for one would be handing the reader a different representation -
     * which is `isForwardable`'s clause, asked here of a value that does not exist yet.
     */
    bool tryConstantForward(Inst& inst) {
        auto& load = (InstLoad&)inst;
        if(!inst.results.size()) return false;

        auto result = opt.base[inst.results.get(opt.base, 0)];
        if(result->convention != Convention::Trivial) return false;

        auto held = constantAt(opt, constants, opt.base[load.address.value()]);
        if(!held) return false;
        if(held.unwrap().type != result->occurrence.type) return false;

        auto value = makeConstantValue(opt, inst, held.unwrap());
        if(!value) return false;

        auto editor = opt.ir();
        editor.replaceAllUses(*result, *value);
        editor.eraseInst(inst);
        return true;
    }

    /*
     * How far a field read is chased back through nested records. A component of a component of a
     * component is past what a literal in the corpus builds, and the bound is what keeps this a
     * constant-cost lookup rather than a walk of an arbitrary address chain.
     */
    static constexpr U32 kAggregateDepth = 4;

    /*
     * An address with its **second names peeled off** - a `Guaranteed` load of a slot is how a borrow
     * is spelled here and a `represent` restates one at another occurrence, and a field taken of
     * either names the same field of the same storage. `allocationRoot` walks exactly this pair; what
     * this does not walk is a projection, because a projection is the step the caller is asking
     * about.
     *
     * It is what makes the rewrite reach anything at all: a record is nearly always read through a
     * borrow of the slot rather than through the slot, so `field.addr .run (load %xs)` was a base the
     * block table had no entry for.
     */
    Value* throughNames(Value* address) {
        for(U32 i = 0; address && address->definer && i < kAggregateDepth; i++) {
            auto inst = opt.base[address->definer];

            if(inst->kind == Inst::Load) {
                if(!inst->results.size()) break;
                if(opt.base[inst->results.get(opt.base, 0)]->convention != Convention::Guaranteed) {
                    break;
                }

                address = opt.base[((InstLoad*)inst)->address.value()];
                continue;
            }

            if(inst->kind == Inst::Unary && ((InstUnary*)inst)->op == mir::UnaryOp::Represent) {
                address = opt.base[((InstUnary*)inst)->value.value()];
                continue;
            }

            break;
        }

        return address;
    }

    /*
     * The component of an **`Aggregate` still in hand** that a field address names - the third
     * answer a read can have, beside the block table and the constant one.
     *
     * `store %slot, (aggregate.Pair %a %b)` establishes what is at `%slot`, and the block table
     * already holds it; what it does not answer is `load (field.addr .left %slot)`, because that is a
     * *different* address and `entryFor` compares addresses. The record is right there in the
     * instruction that built it, so the field is the operand at that index - which is the rewrite the
     * file comment's "a whole-record store kills every field of it" leaves undone. That refusal is
     * about the **kill** and stays exactly as it was: an entry this reads has survived every write
     * that may alias any part of the record, prefix included, so nothing has touched the field since
     * the aggregate was stored.
     *
     * Three restrictions, and each is a rule already stated in this file:
     *
     *  - **a single-constructor record only**, which is `separateFields`' condition and has its
     *    reason: a sum keeps one flat field list with a range per constructor and may fold its tag
     *    into a niche of the payload, so which storage a field occupies is `lower/repr.cpp`'s answer
     *    and a **target's**. A `property` address and a discriminant are refused there and here;
     *  - **`isForwardable`**, at the call site - `Trivial` at both ends with agreeing occurrences,
     *    which is what makes handing the component to the reader a renaming rather than a second
     *    owner or a different representation;
     *  - and the base is resolved by the block table *or* by one more step of this same walk, which
     *    is what reads a field of a nested record - `field.addr .items (field.addr .run %xs)`, the
     *    shape every container header has.
     */
    Value* aggregateComponent(Value* address, U32 depth) {
        if(depth > kAggregateDepth || !address) return nullptr;

        Value* base = nullptr;
        auto step = addressStepOf(opt, *address, base);
        if(!step || !base) return nullptr;

        auto projection = step.unwrap();
        if(projection.kind != Inst::FieldAddress) return nullptr;
        if(projection.property != InstFieldAddress::kNoProperty) return nullptr;
        if(projection.field == InstFieldAddress::kDiscriminant) return nullptr;
        if(!projection.of) return nullptr;

        auto declared = opt.decl[projection.of];
        if(declared->kind != mir::Type::Record) return nullptr;
        if(((mir::RecordType*)declared)->layout != mir::RecordLayout::Single) return nullptr;

        base = throughNames(base);

        auto entry = entryFor(base);
        if(entry && entry->stale & (U32(1) << (projection.field & 31))) return nullptr;
        if(entry && projection.field >= 32 && entry->stale) return nullptr;

        auto held = entry ? entry->value : aggregateComponent(base, depth + 1);
        if(!held || !held->definer) return nullptr;

        auto definer = opt.base[held->definer];
        if(definer->kind != Inst::Aggregate) return nullptr;

        /*
         * And the record the field is *of* is the record in hand. A field index means nothing on its
         * own - it is an index into one declaration's list - so a `represent` that restated the
         * storage at another type, or a slot holding a record of a type this projection does not
         * name, is a component this may not pick.
         */
        if(projection.of != held->occurrence.type) return nullptr;

        auto& built = (InstAggregate&)*definer;
        if(built.constructor != 0) return nullptr;
        if(projection.field >= built.components.size()) return nullptr;

        auto used = built.components.get(opt.base, projection.field);
        if(!used.isSet()) return nullptr;

        return opt.base[used.value()];
    }

    bool tryAggregateForward(Inst& inst) {
        auto& load = (InstLoad&)inst;
        if(!inst.results.size()) return false;

        auto result = opt.base[inst.results.get(opt.base, 0)];
        auto component = aggregateComponent(opt.base[load.address.value()], 0);
        if(!component || !isForwardable(*result, *component)) return false;

        auto editor = opt.ir();
        editor.replaceAllUses(*result, *component);
        editor.eraseInst(inst);
        return true;
    }

    bool tryForward(Inst& inst) {
        auto& load = (InstLoad&)inst;
        if(load.isVolatile) return false;
        if(!inst.results.size()) return false;

        auto result = opt.base[inst.results.get(opt.base, 0)];
        auto address = opt.base[load.address.value()];

        auto entry = entryFor(address);
        if(!entry || !entry->value || !isForwardable(*result, *entry->value)) {
            if(tryAggregateForward(inst)) return true;
            return tryConstantForward(inst);
        }

        /*
         * The condition the held `Contained` reading rests on, asserted where it is relied upon -
         * see `analysis.h`, where the reason is written out.
         *
         * This is the one rewrite in this pass that *adds* uses rather than removing them, and a use
         * added to an allocation is how a contained one comes to escape. It cannot happen because
         * the substituted value is `Trivial` and an allocation's value is an `Address`, which
         * `isForwardable` has just established - so the assertion states the dependency rather than
         * discovering it.
         */
        assertTrue(entry->value->convention != Convention::Address);

        auto editor = opt.ir();
        editor.replaceAllUses(*result, *entry->value);
        editor.eraseInst(inst);
        return true;
    }

    /*
     * A write of exactly what the place already holds, which is no write at all.
     *
     * Sound because a store is only a write by the time this runs: whatever the old contents owed
     * was emitted as its own `Drop` by the elaboration, and the value's own ownership is untouched
     * because the place is left holding the value it was already holding. `Trivial` on both sides
     * for the reason the file comment gives - erasing a store of a tracked value leaves it
     * unconsumed - and the entry the fact came from is a `Trivial` one either way.
     */
    bool tryRedundantStore(Inst& inst) {
        auto& store = (InstStore&)inst;
        if(store.isVolatile) return false;
        auto address = opt.base[store.address.value()];
        auto value = opt.base[store.value.value()];

        if(value->convention != Convention::Trivial) return false;

        auto entry = entryFor(address);
        if(!entry || entry->value != value) return false;

        opt.ir().eraseInst(inst);
        return true;
    }

    /*
     * The store this one overwrites, where nothing has read it.
     *
     * Within one block and one address, which is what makes it sound without an availability
     * analysis: the entry survives only while nothing that may alias it was written and nothing that
     * names its allocation was read, and the overwrite is total because `sameAddress` compares the
     * whole chain rather than a prefix of it.
     *
     * **Only a `Trivial` stored value, and only a held occurrence with no teardown.** Erasing a
     * store of a tracked value leaves it unconsumed and the verifier reports it at this pass's
     * checkpoint; and a place that owed a teardown for its old contents had one emitted before the
     * overwrite, so removing the write it was emitted for would be removing half of a pair.
     */
    bool tryDeadStore(Inst& inst) {
        auto& store = (InstStore&)inst;
        if(store.isVolatile) return false;
        auto address = opt.base[store.address.value()];
        auto value = opt.base[store.value.value()];

        if(value->convention != Convention::Trivial) return false;

        auto entry = entryFor(address);
        if(!entry || !entry->pending || entry->pending == &inst) return false;

        // The value being overwritten, which is the one the removal is about. `Trivial` is the whole
        // guard and `isLinearAt` is what it means - a trivial value owes neither a teardown nor a
        // disposal, so there is no `Drop` of the old contents that this store was emitted behind.
        auto overwritten = entry->value;
        if(!overwritten || overwritten->convention != Convention::Trivial) return false;
        if(isLinearAt(opt, overwritten->occurrence)) return false;

        auto dead = entry->pending;
        entry->pending = nullptr;
        opt.ir().eraseInst(*dead);
        return true;
    }

    void run(Block& block) {
        known.clear();

        SmallArray<SsaPtr<Inst>, 32> instructions;
        for(auto pointer: block.instructions(opt.base)) instructions.push(pointer);

        for(auto pointer: instructions) {
            auto& inst = *opt.base[pointer];

            // An instruction an earlier rewrite in this walk took out - the erase leaves the table
            // entry standing, so what says it is gone is that it is in no block.
            if(!inst.block) continue;

            if(inst.kind == Inst::Load) {
                if(((InstLoad&)inst).isVolatile) {
                    // It is an observable read, not a fact from which later ordinary reads may be
                    // answered. Retire all forwarding facts while retaining the instruction.
                    known.clear();
                    continue;
                }
                if(tryForward(inst)) continue;

                auto address = opt.base[((InstLoad&)inst).address.value()];
                auto result = inst.results.size() ? opt.base[inst.results.get(opt.base, 0)] : nullptr;

                // The handover writes the storage it reads, which `mayWriteStorage` says and
                // `forgetWritten` therefore already applies; what is left is the fact this load
                // itself establishes, and a tracked result establishes none.
                forgetWritten(inst);
                if(result && result->convention == Convention::Trivial) {
                    remember(address, result, nullptr);
                }

                continue;
            }

            if(inst.kind == Inst::Store) {
                if(((InstStore&)inst).isVolatile) {
                    // A device write may change any location visible through the raw address.
                    known.clear();
                    continue;
                }
                if(tryRedundantStore(inst)) continue;
                tryDeadStore(inst);

                auto& store = (InstStore&)inst;
                auto address = opt.base[store.address.value()];
                auto value = opt.base[store.value.value()];

                forgetWritten(inst);

                // A fact worth keeping is a fact something can be answered with, and only a
                // `Trivial` value is one - see `isForwardable`. The entry is recorded for a value of
                // any other convention too, because what the dead-store rule needs is the *store*,
                // and it declines on the convention rather than on the entry being there.
                remember(address, value, &inst);
                continue;
            }

            forgetWritten(inst);
        }
    }
};

/*
 * An allocation nothing reads, with the writes into it - the removal half of the shipping stage's
 * `scalarizeLocals`. The splitting half is `splitAggregateSlots` at the lower tier and is not here.
 *
 * "Reads" is every use that is not a write through this allocation's own address, which is what
 * makes the test the use list and one question about the held occurrence: a `Copy` reads the bytes,
 * a call argument reads the whole value, an aggregate component reads it, and each of those is a use
 * `Contained` has already refused. So the walk below sees only the kinds containment admits, and
 * what it has left to decide is which of those are writes.
 *
 * A `Drop` is admitted and is the one that needs the argument. It is not a write and it is not a
 * read - it is the end of the object's life - and erasing one is sound exactly where it lowers to
 * nothing: `Lowering::emitTeardown` returns immediately for a type whose `Ownership` names neither a
 * drop nor a reclaim, so a drop of a teardown-free occurrence emits no instruction at all. That is
 * what the `hasTeardownAt` test is, and `isLinearAt` beside it is the second half of the same
 * question: a type may owe no teardown and still owe a disposal the author has to write, and a slot
 * holding one of those is not a slot this may quietly delete.
 */
struct StorageRemover {
    OptContext& opt;
    const Contained& contained;
    const ConstantContents& constants;

    /*
     * Whether the allocation being considered holds a constant whose **teardown runs nothing** -
     * decided where the constant was minted and recorded on the program
     * (`mir::Program::inertConstants`).
     *
     * It is what makes a string literal disappear rather than merely shrink. A literal is a
     * duplicate of a placed constant in a slot, and once every read of it has been answered by the
     * constant itself, the slot's remaining users are the write that filled it and the `Drop` at the
     * end - and the drop is a call to a reclaim that tests a flag the constant sets to "borrowed".
     * A teardown that runs nothing is a teardown whose removal changes nothing, so the three go
     * together: the drop, the duplicate, and the storage.
     *
     * Set per allocation in `remove` and read by `writesOnly`, because it is the same fact answering
     * two of that walk's questions - the drop is admitted by it, and so is the tracked value the
     * store consumes.
     */
    bool inert = false;

    // The writes into this allocation and the chains they are written through, in the order they
    // have to be erased in: an address chain cannot go before the accesses through it, and the
    // allocation cannot go before the chains.
    SmallArray<Inst*, 8> writes;
    SmallArray<Inst*, 8> chains;

    /*
     * Whether every use of one address is a write through it, or a projection that is.
     *
     * `Contained` has already established that the use list holds nothing else - no call argument,
     * no aggregate component, no store of the address as a value - so what is asked here is the
     * narrower question: a `Load`, a `Copy` or an `Atomic` is a *read* and declines, and a `Store`
     * of a tracked value declines because erasing it would leave the value unconsumed.
     */
    bool writesOnly(Value& address, bool isRoot) {
        for(auto userPointer: address.uses(opt.base)) {
            auto& user = *opt.base[userPointer];

            switch(user.kind) {
                /*
                 * The release of a heap or region placement, admitted **only at the allocation
                 * itself** - `free` names what the allocator handed out, so a `free` reached through
                 * a projection would be a release of an interior address and is a defect rather than
                 * a case this may erase.
                 *
                 * It is a write in the sense this list means: it reads nothing the program wants and
                 * it ends the storage's life. Erasing it with the allocation is the pair - one
                 * without the other is either a leak or a release of storage nothing asked for.
                 */
                case Inst::Free:
                    if(!isRoot) return false;

                    writes.push(&user);
                    break;

                /*
                 * A store, and the one tracked value one may carry.
                 *
                 * Erasing a store of a tracked value leaves that value unconsumed, which the
                 * verifier reports at this pass's checkpoint - unless the value goes with it. A
                 * `const` whose teardown runs nothing is the case where it can: it is the only
                 * definition of an owner that owes nothing to run, this store is its only reader,
                 * and the constant it names is still standing in the data section for anybody else
                 * who wants it. So the pair is erased as a pair.
                 */
                case Inst::Store: {
                    auto value = opt.base[((InstStore&)user).value.value()];
                    if(isTracked(value->convention)) {
                        if(!inert || !value->definer) return false;
                        if(opt.base[value->definer]->kind != Inst::Const) return false;
                        if(value->uses(opt.base).size() != 1) return false;

                        chains.push(opt.base[value->definer]);
                    }

                    writes.push(&user);
                    break;
                }

                /*
                 * A `Drop`, admitted where it lowers to nothing - and `inert` is the second way that
                 * is true.
                 *
                 * `hasTeardownAt` is the **type's** answer: `Lowering::emitTeardown` returns
                 * immediately for an occurrence whose `Ownership` names neither a drop nor a
                 * reclaim, and `isLinearAt` beside it is the other half - a type may owe no teardown
                 * and still owe a disposal the author has to write. `inert` is the same question
                 * asked of the **value**, which is what a constant makes answerable: the type owes a
                 * reclaim and this value's reclaim was run at compile time and did nothing.
                 */
                case Inst::Drop:
                    if(!inert) {
                        if(hasTeardownAt(opt, ((InstDrop&)user).held)) return false;
                        if(isLinearAt(opt, ((InstDrop&)user).held)) return false;
                    }

                    writes.push(&user);
                    break;

                case Inst::FieldAddress:
                case Inst::ElementAddress: {
                    if(!user.results.size()) return false;
                    if(!writesOnly(*opt.base[user.results.get(opt.base, 0)], false)) return false;

                    chains.push(&user);
                    break;
                }

                default:
                    return false;
            }
        }

        return true;
    }

    bool remove(Value& allocation) {
        auto& alloc = (InstAlloc&)*opt.base[allocation.definer];

        // The contents, where they are settled and their teardown was shown to run nothing - see
        // `inert`, where the whole of what it admits is argued.
        auto held = constants[allocation];
        inert = held.isSet() && opt.program.teardownIsNothing(held);

        // The occurrence rather than the type: a slot holding a borrowed occurrence of a type that
        // owes a teardown owes nothing, and one holding an owner of the same type owes everything.
        if(!inert && (hasTeardownAt(opt, alloc.held) || isLinearAt(opt, alloc.held))) return false;
        if(!contained[allocation]) return false;

        /*
         * A heap placement is removed with the `Free` that hands it back, which is the correction
         * this rule used to be: "frame storage only, because the release is a second instruction
         * this deletion would have to find". Finding it is the use list, which is the same list
         * `writesOnly` is already walking - `Contained` admits a `free` at the allocation since the
         * closure work, so an allocation that reaches here with one in its uses is one nothing
         * outside this body can name, and the allocator call and its matching release go together.
         *
         * A **region** placement is still declined. What a region hands back is not the object's own
         * bytes and the release is not per object, so removing one allocation out of a region is a
         * question about the region rather than about this slot, and there is no producer of one to
         * measure the answer against.
         */
        if(alloc.storage == mir::StorageClass::Region) return false;

        writes.clear();
        chains.clear();
        if(!writesOnly(allocation, true)) return false;

        // An allocation with no users at all is one `simplify` would take if `Alloc` were pure, and
        // it is not - storage coming into existence is not a computation. So it is removed here, and
        // the empty write list is the ordinary way that happens after a round of forwarding.
        auto editor = opt.ir();

        // Guarded, because a use list holds one entry per naming **operand**: an instruction that
        // reaches the allocation twice is on the list twice, and an erase of one already taken out
        // is what `eraseInst` asserts against.
        for(auto write: writes) {
            if(write->block) editor.eraseInst(*write);
        }

        // Leaf first, which is the order the list is already in: `writesOnly` recurses before it
        // pushes, so a projection off a projection stands *in front of* its own base. Walking the
        // other way would erase `x.a` while `x.a.b` still named it, which is the one thing
        // `eraseInst` asserts against.
        for(auto chain: chains) {
            if(chain->block) editor.eraseInst(*chain);
        }

        editor.eraseInst(alloc);
        return true;
    }
};

} // namespace

void forwardMemory(OptContext& opt) {
    Forwarder forwarder { opt, containedOf(opt), constantContentsOf(opt) };

    for(auto blockPointer: opt.body->blocks.contents(opt.base)) {
        auto block = opt.base[blockPointer];
        if(block->deleted) continue;

        forwarder.run(*block);
    }
}

void removeDeadStorage(OptContext& opt) {
    /*
     * Over a snapshot of the value table, because the removals below edit the use lists this walk is
     * reading and the table grows for nobody here.
     *
     * One containment reading for the whole pass, on `forwardMemory`'s terms and with the same
     * consequence: erasing the last use of an address is exactly the edit that can turn an escaping
     * allocation into a contained one, so a removal here can enable another - and what collects that
     * one is the *next round* of the fixed point rather than a second reading inside this walk. The
     * alternative is rebuilding an analysis over every value in the body once per allocation
     * removed, which is a walk squared for an answer one more round produces anyway.
     */
    auto& contained = containedOf(opt);
    SmallArray<Value*, 32> allocations;

    for(auto valuePointer: opt.body->values.contents(opt.base)) {
        auto value = opt.base[valuePointer];
        if(!value->definer) continue;

        auto definer = opt.base[value->definer];
        if(definer->kind != Inst::Alloc || !definer->block) continue;

        allocations.push(value);
    }

    for(auto allocation: allocations) {
        if(!allocation->definer || !opt.base[allocation->definer]->block) continue;

        StorageRemover remover { opt, contained, constantContentsOf(opt) };
        remover.remove(*allocation);
    }
}

namespace {

/*
 * Move elision: two frame slots that hold one value, one after the other, made into one slot.
 *
 * `let m2 = insert(m1, k, v)` lowers to a slot for `m1`, a slot for `m2`, and forty bytes copied
 * between them - and the copy is all there is between the two, because a **handover** takes the
 * value out of the first slot and puts it in the second. The shape is exactly:
 *
 *     %v = load %a         ; a tracked result, so this empties `%a`
 *     store %b, consume %v ; and this is the only thing that reads it
 *
 * `test/bench/findings.md` §102.3 is where this was measured as the last of the new stage's size
 * gap that buys nothing: 468 such copies over `test/resolve`, and `Tree` in the benchmark corpus
 * running 9.2% slower than the shipping stage on four instructions per iteration doing one of them.
 * `destinationFor`'s `Store` arm in `lower/inst.cpp` is the other half - it covers the case where
 * the value is *produced* into the second slot, and this covers the case where it comes out of the
 * first.
 *
 * ## What makes it sound
 *
 * The two slots are merged, so what has to hold is that their **live ranges do not overlap** - which
 * is the register allocator's coalescing condition, asked of storage. Two dominance tests state it,
 * and neither is an approximation of the other:
 *
 *  - **every use of `%b` other than the store is dominated by the store.** So nothing touches the
 *    second slot before it holds the moved value. This is the condition a conditional teardown makes
 *    necessary rather than a formality: MIR emits a `Drop` of `%b` on a path the store may not have
 *    run on, and merging without this would release a value `%a` still owns.
 *  - **no use of `%a` other than the load is reachable from the store.** So nothing touches the
 *    first slot after the value has left it. A *refill* is a use like any other and is what this
 *    catches: `%a` written again below would otherwise write over `%b`.
 *
 * `%a`'s allocation dominating every use of `%b` falls out of the two rather than being asked: it
 * dominates the store, because the load reads it there, and the store dominates all of them.
 *
 * Three structural conditions in front of those, each of which is about what a slot *is*:
 *
 *  - both are `Alloc` of the same `StorageClass`, so what is merged is two pieces of storage of the
 *    same kind. A heap or region allocation is somebody's to free, and two of them are two.
 *  - the two `held` occurrences are **equal**, not merely the same type. The occurrence is what the
 *    manifestation is asked of - `Occurrence::sameManifestation` - so an owned `String` and a shared
 *    window into one are two slots of different sizes with one type between them.
 *  - the load's result is **tracked** and has this store as its only use. A `Guaranteed` result is a
 *    borrow rather than a handover: the value stays where it is, and there is nothing to move.
 *
 * ## Why it is here and not in `forwardMemory`
 *
 * The file comment above declines a tracked load, and gives three reasons for it. This is not that
 * rewrite: it does not point the load's readers at the stored value, it removes the *storage*, and
 * the pair of dominance tests is exactly the guard that comment says a forward would need and does
 * not have ("what the rewrite needs is that nothing in between reads it either"). Asked over a whole
 * body rather than inside one block, which is the other half of why it could not live there.
 */
/*
 * How many distinct names for one allocation `eachTouch` will walk before giving up.
 *
 * A bound rather than a limit, on `kRootDepth`'s terms one file over: the chains this asks about are
 * a handful of projections off a container header, and a walk with no bound is one whose cost a body
 * chooses. Past it the merge is declined.
 */
constexpr Size kNameLimit = 64;

struct MoveElider {
    OptContext& opt;
    const Dominators& dominators;

    // The candidates, collected before anything is rewritten - the removals below edit the use lists
    // this walk reads, which is `removeDeadStorage`'s arrangement one file down for the same reason.
    SmallArray<Inst*, 16> stores;

    // Whether `use` comes after `at` in the block they share.
    bool after(Block& block, SsaPtr<Inst> at, SsaPtr<Inst> use) {
        auto count = block.instructionCount();
        for(Size i = 0; i < count; i++) {
            if(block.instructionAt(opt.base, i) == use) return false;
            if(block.instructionAt(opt.base, i) == at) return true;
        }

        return false;
    }

    // Whether the store dominates this use of the second slot - the block relation, and the position
    // inside the block they share.
    bool afterStore(Inst& store, Inst& use) {
        if(use.block == store.block) {
            return after(*opt.base[store.block], SsaPtr<Inst>(&store - opt.base),
                         SsaPtr<Inst>(&use - opt.base));
        }

        return dominators.dominates(opt.base[store.block]->index.index,
                                    opt.base[use.block]->index.index);
    }

    /*
     * Whether control can get from the store to any of the marked blocks.
     *
     * A forward walk from the store's *successors*, so the store's own block counts only when a path
     * comes back to it - which is what a loop does, and is the case that makes the walk necessary
     * rather than a position comparison. A use inside the store's block and below it is not this
     * walk's to find: `afterStore` above is asked of every use first, and this is asked of what is
     * left.
     */
    bool reachesMarked(Inst& store, const IndexSet& marked) {
        ScratchSet seen(opt.sets, opt.body->blocks.size());
        SmallArray<U32, 32> queue;

        forEachSuccessor(opt.base, *opt.base[store.block], [&](U32 target) {
            if(seen->operator [] (target)) return;
            seen->set(target, true);
            queue.push(target);
        });

        while(queue.size()) {
            auto index = queue.pop().unwrap();

            if(marked[index]) return true;

            auto block = opt.body->block(opt.base, BlockId(index));
            if(block->deleted) continue;

            forEachSuccessor(opt.base, *block, [&](U32 target) {
                if(seen->operator [] (target)) return;
                seen->set(target, true);
                queue.push(target);
            });
        }

        return false;
    }

    /*
     * Every instruction that touches an allocation's storage, through **any** name it has.
     *
     * The use list of the `Alloc` is not that set, and the difference is the whole of what a
     * direct-use test gets wrong: `field.addr .entries %m` and the `load` of a borrow off it are two
     * more names for the same bytes, and an instruction reading one of *those* below the store is an
     * instruction that would see the merged slot's contents instead of the moved-from slot's. The
     * recursion is `Containment::admits`' - a projection and a `Guaranteed` load are steps, and
     * everything else is a use rather than a name - asked for the set instead of for a verdict.
     *
     * `false` where the walk runs past its bound, which every caller reads as "decline".
     */
    template<class F> bool eachTouch(Value& allocation, F&& visit) {
        SmallArray<Value*, 16> names;
        names.push(&allocation);

        for(Size at = 0; at < names.size(); at++) {
            if(names.size() > kNameLimit) return false;

            for(auto used: names[at]->uses(opt.base)) {
                auto use = opt.base[used];
                visit(*use, used);

                if(!use->results.size()) continue;

                auto result = opt.base[use->results.get(opt.base, 0)];
                auto isName = use->kind == Inst::FieldAddress || use->kind == Inst::ElementAddress ||
                              (use->kind == Inst::Load &&
                               result->convention == Convention::Guaranteed);

                if(isName) names.push(result);
            }
        }

        return true;
    }

    // The `Alloc` that defines this address, and null for everything else - the two ends of a merge
    // are two allocations, and nothing looser is one.
    Value* allocationOf(SsaPtr<Value> pointer) {
        if(!pointer) return nullptr;

        auto value = opt.base[pointer];
        if(!value->definer) return nullptr;

        auto definer = opt.base[value->definer];
        if(definer->kind != Inst::Alloc || !definer->block) return nullptr;

        return value;
    }

    bool tryElide(Inst& store) {
        auto& written = (InstStore&)store;
        if(written.isVolatile) return false;
        auto moved = opt.base[written.value.value()];

        if(!isTracked(moved->convention) || moved->useCount() != 1) return false;
        if(!moved->definer) return false;

        auto load = opt.base[moved->definer];
        if(load->kind != Inst::Load || !load->block || ((InstLoad*)load)->isVolatile) return false;

        auto from = allocationOf(((InstLoad*)load)->address.value());
        auto into = allocationOf(written.address.value());
        if(!from || !into || from == into) return false;

        auto& source = (InstAlloc&)*opt.base[from->definer];
        auto& target = (InstAlloc&)*opt.base[into->definer];
        if(source.storage != target.storage) return false;
        if(!source.held.sameManifestation(target.held)) return false;

        // Every touch of the second slot but this store is below it. One in an unreachable block
        // fails the dominance test and declines the merge, which is the conservative direction.
        auto below = true;
        if(!eachTouch(*into, [&](Inst& use, SsaPtr<Inst>) {
            if(&use == &store) return;
            if(!use.block || !afterStore(store, use)) below = false;
        })) {
            return false;
        }

        if(!below) return false;

        /*
         * And no touch of the first slot is reachable from the store. The load is not one of them -
         * it is above the store by construction, being what produced its operand - and one below the
         * store inside the store's own block is refused here rather than by the walk.
         */
        ScratchSet marked(opt.sets, opt.body->blocks.size());
        auto any = false;
        auto reached = false;

        if(!eachTouch(*from, [&](Inst& use, SsaPtr<Inst> pointer) {
            if(&use == load || &use == &store) return;
            if(!use.block) { reached = true; return; }

            if(use.block == store.block &&
               after(*opt.base[store.block], SsaPtr<Inst>(&store - opt.base), pointer)) {
                reached = true;
                return;
            }

            marked->set(opt.base[use.block]->index.index, true);
            any = true;
        })) {
            return false;
        }

        if(reached) return false;
        if(any && reachesMarked(store, *marked)) return false;

        auto editor = opt.ir();
        editor.replaceAllUses(*into, *from);
        editor.eraseInst(store);
        editor.eraseInst(*load);
        editor.eraseInst(*opt.base[into->definer]);
        return true;
    }

    void run() {
        for(auto blockPointer: opt.body->blocks.contents(opt.base)) {
            auto block = opt.base[blockPointer];
            if(block->deleted) continue;

            for(auto instPointer: block->instructions(opt.base)) {
                auto inst = opt.base[instPointer];
                if(inst->kind == Inst::Store) stores.push(inst);
            }
        }

        for(auto store: stores) {
            if(store->block) tryElide(*store);
        }
    }
};

} // namespace

void elideMoves(OptContext& opt) {
    MoveElider elider { opt, dominatorsOf(opt) };
    elider.run();
}

} // namespace ssa
