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
     * One step of an address chain, told apart from every other step.
     *
     * `record`/`container` travel with the index because that is what decides what the index
     * *means*: field 1 of two different records is two different offsets, and an element of two
     * different containers is two different strides. The lowering asks one table for both, and a
     * comparison here that dropped them would be a second answer to agree with.
     */
    struct Step {
        Inst::Kind kind = Inst::Alloc;
        DeclPtr<mir::Type> of = nullptr;
        U16 field = 0;
        Value* index = nullptr;
        DeclPtr<mir::OwnershipSchema> shape = nullptr;
        U16 property = InstFieldAddress::kNoProperty;
    };

    // The step an address is, and the address it is taken of. `base` is null where the address is a
    // root - an allocation, a global, a pointer this pass cannot see behind.
    Maybe<Step> stepOf(Value& address, Value*& base) {
        base = nullptr;
        if(!address.definer) return Nothing();

        auto inst = opt.base[address.definer];
        if(inst->kind == Inst::FieldAddress) {
            auto& projection = (InstFieldAddress&)*inst;
            base = opt.base[projection.base.value()];

            return Just(Step { Inst::FieldAddress, projection.record, projection.field, nullptr,
                               projection.shape, projection.property });
        }

        if(inst->kind == Inst::ElementAddress) {
            auto& element = (InstElementAddress&)*inst;
            base = opt.base[element.base.value()];

            return Just(Step { Inst::ElementAddress, element.container, 0,
                               opt.base[element.index.value()] });
        }

        return Nothing();
    }

    // Whether two steps of one shape name the same component - `simplify`'s `sameImmediates` asked
    // of an address rather than of a computation, plus the index, which is a value there and a
    // number here.
    bool sameStep(const Step& left, const Step& right) {
        if(left.kind != right.kind || left.of != right.of) return false;

        if(left.kind == Inst::FieldAddress) {
            return left.field == right.field && left.shape == right.shape &&
                   left.property == right.property;
        }

        if(left.index == right.index) return true;

        auto a = constantValueOf(opt, left.index);
        auto b = constantValueOf(opt, right.index);
        return a && b && a.unwrap() == b.unwrap();
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
    bool separateFields(const Step& left, const Step& right) {
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
    // `sameStep`, and deliberately not its negation: two indices neither of which is a constant are
    // neither the same element nor two different ones.
    bool separateStep(const Step& left, const Step& right) {
        if(left.kind != right.kind || left.of != right.of) return false;
        if(left.kind == Inst::FieldAddress) return separateFields(left, right);

        auto a = constantValueOf(opt, left.index);
        auto b = constantValueOf(opt, right.index);
        return a && b && a.unwrap() != b.unwrap();
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
            if(left == right) return true;

            Value* leftBase = nullptr;
            Value* rightBase = nullptr;
            auto a = stepOf(*left, leftBase);
            auto b = stepOf(*right, rightBase);
            if(!a || !b || !sameStep(a.unwrap(), b.unwrap())) return false;

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

        SmallArray<Step, 8> leftPath;
        SmallArray<Step, 8> rightPath;

        auto walk = [&](Value* address, SmallArray<Step, 8>& path) {
            while(address) {
                Value* base = nullptr;
                auto step = stepOf(*address, base);
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

            if(separateStep(a, b)) return false;
            if(!sameStep(a, b)) return true;
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

        auto found = false;
        forEachUse(opt.base, inst, [&](Use used) {
            if(!used.isSet()) return;
            if(allocationRoot(opt, opt.base[used.value()]) == root) found = true;
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
                    known.remove(i);
                    continue;
                }
            }

            if(touches(inst, known[i].address)) known[i].pending = nullptr;
        }
    }

    // One entry per piece of storage, so that "what is here" and "which store put it there" are one
    // answer rather than the most recent of several.
    void remember(Value* address, Value* value, Inst* pending) {
        for(Size i = known.size(); i-- > 0;) {
            if(sameAddress(known[i].address, address)) known.remove(i);
        }

        known.push(Known { address, value, pending });
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
    bool tryForward(Inst& inst) {
        auto& load = (InstLoad&)inst;
        if(!inst.results.size()) return false;

        auto result = opt.base[inst.results.get(opt.base, 0)];
        auto address = opt.base[load.address.value()];

        auto entry = entryFor(address);
        if(!entry || !entry->value || !isForwardable(*result, *entry->value)) return false;

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
    bool writesOnly(Value& address) {
        for(auto userPointer: address.uses(opt.base)) {
            auto& user = *opt.base[userPointer];

            switch(user.kind) {
                case Inst::Store: {
                    auto value = opt.base[((InstStore&)user).value.value()];
                    if(isTracked(value->convention)) return false;

                    writes.push(&user);
                    break;
                }

                case Inst::Drop:
                    if(hasTeardownAt(opt, ((InstDrop&)user).held)) return false;
                    if(isLinearAt(opt, ((InstDrop&)user).held)) return false;

                    writes.push(&user);
                    break;

                case Inst::FieldAddress:
                case Inst::ElementAddress: {
                    if(!user.results.size()) return false;
                    if(!writesOnly(*opt.base[user.results.get(opt.base, 0)])) return false;

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

        // The occurrence rather than the type: a slot holding a borrowed occurrence of a type that
        // owes a teardown owes nothing, and one holding an owner of the same type owes everything.
        if(hasTeardownAt(opt, alloc.held) || isLinearAt(opt, alloc.held)) return false;
        if(!contained[allocation]) return false;

        /*
         * Frame storage only, which is `StorageClass`'s one reachable rung today and the rule
         * anyway. Deleting an allocation is deleting the request for it, and that is a complete
         * answer exactly where the frame is what supplied it; a region or a heap placement is storage
         * something *handed out*, and handing it back is `Reclaim`'s job rather than a deletion's -
         * `lower/erase.cpp` is where the two halves are decided and it reads this same field.
         */
        if(alloc.storage != mir::StorageClass::Frame) return false;

        writes.clear();
        chains.clear();
        if(!writesOnly(allocation)) return false;

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
    Forwarder forwarder { opt, containedOf(opt) };

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

        StorageRemover remover { opt, contained };
        remover.remove(*allocation);
    }
}

} // namespace ssa
