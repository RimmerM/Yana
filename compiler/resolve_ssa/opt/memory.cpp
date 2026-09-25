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

    // Whether the fact came in from the block's sole predecessor rather than being established
    // here - see `Forwarder::ends`, which is why a call ends it.
    bool inherited = false;

    // Whether a call has run since the fact was established - `tryForward`, J154.
    bool crossed = false;

    /*
     * Where `address` is rooted, found once when the fact is made (`Forwarder::remember`) - J163.
     * `forgetWritten` asks it of every entry at every instruction that is not pure, and used to walk
     * `allocationRoot` and then `globalRootOf` each time. The address of an entry never changes, so
     * neither does its root; a rewrite later in the pass can only make a walk find a root it did not
     * find before, and the answer kept is then the cautious one.
     */
    Value* root = nullptr;
    bool global = false;
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
     * What is settled before the program runs, read on the same terms and held for the same reason:
     * `soleContents`, once per slot for the pass.
     *
     * The block table below is what one write above a read establishes; this is what a *slot* holds
     * for the whole of its life, which is the fact a call in between destroys and the one a string
     * literal's teardown needs. Two tables rather than one because they are two facts with two
     * lifetimes - one is emptied at every barrier and the other cannot be - and one rule reads them
     * in order (`tryForward`). The answer is read against the body as the first question found it,
     * the way `contained` is: what this pass erases is reads, which cannot make a slot written by
     * more than its fill.
     */
    SoleContents slots;

    // One block's worth. Inline, because a block holds a handful of distinct pieces of storage and
    // this is emptied at every block boundary and at every barrier - except into a block with one
    // way in, which starts from its predecessor's (`ends`).
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
            auto a = linkOf(left);
            auto b = linkOf(right);
            if(!a.address || !b.address) return false;

            if(a.address == b.address) return true;

            // Two `global.addr`s of one global, which the conversion writes one per access.
            if(auto global = globalNamed(a.address)) return global == globalNamed(b.address);

            if(!stepOf(a) || !stepOf(b) || !sameAddressStep(opt, a.step.unwrap(), b.step.unwrap())) return false;

            left = a.base;
            right = b.base;
        }

        return false;
    }

    /*
     * One link of an address chain, which `sameAddress` walks two of in step and `globalRootOf` one
     * of to its end - J163, where each had its own copy of the walk.
     *
     * The second names taken off first - `throughNames`, and the same pair `allocationRoot` walks. A
     * borrow of a slot and the slot are one piece of storage, so a fact recorded under one of them
     * answers a read through the other; without this every write through a `&` parameter
     * established a fact nothing could find again, because the write named the borrow and the read
     * named the slot. `stepOf` is then the projection the address is, and what it projects from,
     * asked separately because the commonest answer - the two addresses are one value - needs none.
     */
    struct Link {
        Value* address = nullptr;
        Maybe<AddressStep> step;
        Value* base = nullptr;
    };

    Link linkOf(Value* address) {
        Link link;
        link.address = throughNames(address);
        return link;
    }

    // No step for a `global.addr`, which is a root, nor for anything else that is not a projection.
    bool stepOf(Link& link) {
        if(globalNamed(link.address)) return false;

        link.step = addressStepOf(opt, *link.address, link.base);
        return link.step.isJust();
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

        return pathsMayOverlap(leftPath, rightPath);
    }

    /*
     * Whether two paths from one root may reach overlapping storage - the step-by-step half of
     * `mayAlias` and of `mayWriteGlobal`, which had a copy each until J163.
     *
     * From the root down, which is the order the paths were pushed in reverse. A prefix is not a
     * separation: the walk stops at the shorter path, and everything up to there having been the
     * same step is what makes a whole-record write reach every field of it.
     */
    bool pathsMayOverlap(const SmallArray<AddressStep, 8>& leftPath, const SmallArray<AddressStep, 8>& rightPath) {
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
     * instruction that has one is a terminator, and a pending store never crosses a block boundary -
     * a successor inherits the facts and not the stores (`ends`) - so a store this failed to mark read
     * is one nothing can remove afterwards.
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

    // The global a `global.addr` names, or null for any other value.
    DeclPtr<mir::Global> globalNamed(Value* address) {
        if(!address || !address->definer) return nullptr;

        auto inst = opt.base[address->definer];
        if(inst->kind != Inst::GlobalAddress) return nullptr;
        return ((InstGlobalAddress*)inst)->global;
    }

    /*
     * The `global.addr` an address is a projection of, through the second names `throughNames`
     * peels - and the steps from it, root last, where `path` is given. Null where the address is
     * rooted anywhere else, and past `kAggregateDepth * 4` steps, which no access in the corpus
     * approaches.
     */
    Value* globalRootOf(Value* address, SmallArray<AddressStep, 8>* path) {
        for(U32 i = 0; address && i < kAggregateDepth * 4; i++) {
            auto link = linkOf(address);
            if(!link.address) return nullptr;
            if(globalNamed(link.address)) return link.address;
            if(!stepOf(link)) return nullptr;

            if(path) path->push(link.step.unwrap());
            address = link.base;
        }

        return nullptr;
    }

    /*
     * Whether a write through `written` may reach the storage of a **global** entry - `mayAlias`'s
     * question for the one root it cannot bound, answered with the one fact that bounds it. J154.
     *
     * Three roots a write can have. Another global's is other storage, and the same global's is
     * compared step by step exactly as `mayAlias` compares two paths into one allocation. An
     * allocation of this body is a frame slot or a run, and no global is either. What is left - a
     * pointer loaded out of storage, a parameter, a constant - may be *any* address the program
     * made, and the global's is among them only if it was ever used as something other than the
     * place of an access: `OptContext::addressedGlobals`. A global nothing took the address of is
     * one no raw store can name, which is the property the shipping stage's `placesMayAlias` gives
     * up on ("a raw pointer may name anything at all").
     */
    bool mayWriteGlobal(Value* entry, Value* written) {
        SmallArray<AddressStep, 8> entryPath;
        SmallArray<AddressStep, 8> writtenPath;

        auto entryRoot = globalRootOf(entry, &entryPath);
        if(!entryRoot) return true;

        auto global = globalNamed(entryRoot);
        auto writtenRoot = globalRootOf(written, &writtenPath);

        if(!writtenRoot) {
            if(allocationRoot(opt, written)) return false;
            if(!opt.globalsScanned) return true;

            auto addressed = opt.addressedGlobals.get(global.offset);
            return !addressed || addressed.unwrap();
        }

        if(globalNamed(writtenRoot) != global) return false;
        return pathsMayOverlap(entryPath, writtenPath);
    }

    /*
     * Whether an instruction may change what a **global** entry says - J154, J94 §6.2's B.
     *
     * The table used to drop every fact about storage with no allocation root at every instruction
     * that was not pure, so `seen = seen + 5; return seen` read the global back after writing it and
     * so did every read-modify-write of one - `GlobalOrder`, `Operators`' `@trace`. What may change
     * a global is narrower than that:
     *
     *  - a **store** and a handover through the place they name, which `mayWriteGlobal`
     *    separates;
     *  - an **atomic** of any location, whatever it names - an atomic write through the global's
     *    own place included, which is why there is no `Atomic` arm. It is where another thread's writes
     *    become visible to this one, so a fact about a global read before it may be stale after it.
     *    A plain read racing a write is undefined, so between two synchronizing operations the
     *    table is this thread's alone;
     *  - and a **call**, every one. `mir::reachesGlobalState` says whether a callee names a global,
     *    but not whether it synchronizes: `lock(m)` over a mutex handed in as an argument names no
     *    global and is exactly where a global it guards may change. The shipping stage ends the
     *    table at every call too;
     *  - a drop, a native and whatever is not listed: may.
     *
     * A read, an allocation, an aggregate, an edge and a copy (which reads its address and writes
     * storage of its own) change no global.
     */
    bool globalWritten(Inst& inst, Value* entry) {
        switch(inst.kind) {
            case Inst::Load: {
                if(!inst.results.size()) return false;
                if(!isTracked(opt.base[inst.results.get(opt.base, 0)]->convention)) return false;

                return mayWriteGlobal(entry, opt.base[((InstLoad&)inst).address.value()]);
            }

            case Inst::Store:
                return mayWriteGlobal(entry, opt.base[((InstStore&)inst).address.value()]);

            case Inst::Alloc:
            case Inst::Aggregate:
            case Inst::Jump:
            case Inst::Branch:
            case Inst::Switch:
            case Inst::Copy:
                return false;

            default:
                return true;
        }
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
     * An entry with no allocation root of this body's is a **global**'s or nobody's. A global's is
     * asked `globalWritten` instead (J154), which answers the address-level question for a store and
     * a handover and yes for a call, an atomic, a native and a drop; an entry rooted nowhere answers
     * yes to every instruction that is not pure.
     */
    void forgetWritten(Inst& inst) {
        if(isPure(inst)) return;

        auto written = writtenThrough(inst);

        for(Size i = known.size(); i-- > 0;) {
            auto root = known[i].root;

            // A global's entry has a rule of its own - `globalWritten`, J154.
            if(known[i].global) {
                if(!globalWritten(inst, known[i].address)) {
                    if(touches(inst, known[i].address)) known[i].pending = nullptr;
                    continue;
                }

                U32 field = 0;
                if(written && narrowWritten(known[i], written, field)) {
                    known[i].stale |= U32(1) << field;
                    known[i].pending = nullptr;
                    continue;
                }

                known.remove(i);
                continue;
            }

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

        Known entry { address, value, pending, 0 };
        entry.root = allocationRoot(opt, address);
        entry.global = !entry.root && globalRootOf(address, nullptr);
        known.push(entry);
    }

    Known* entryFor(Value* address) {
        for(Size i = known.size(); i-- > 0;) {
            if(sameAddress(known[i].address, address)) return &known[i];
        }

        return nullptr;
    }

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

        auto held = constantAt(opt, slots, contained, opt.base[load.address.value()]);
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
        if(entry && entry->crossed) componentCrossed = true;

        auto held = entry ? entry->value : aggregateComponent(base, depth + 1);
        if(!entry && !held) held = storedAggregate(base);
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

        if(undecidedFlag(built, projection.field)) return nullptr;

        return opt.base[used.value()];
    }

    /*
     * The `ownsHeap` of a run whose placement `placeRuns` has not decided yet - J137.
     *
     * The flag is the one component of a descriptor another pass rewrites in place: a placement
     * turns `runFromHeap` into `runBorrowed` in the aggregate, and it is sound only because the
     * reclaim reads the flag at run time. A read answered before the decision keeps the old
     * answer, so a frame placement after it hands a frame address to an unconditional `freeHeap`.
     * That could not happen while every reclaim was a call the lowering emitted; the first J137
     * build inlined one into the block that built the literal, folded its test to "heap", placed
     * the run on the frame a round later, and `iter/Inline.yana` crashed in `freeHeap`.
     *
     * A request with `placed` set has been decided and its flag is final. One without it may still
     * be placed, and its flag is answered by the storage rather than by the component until the
     * pass has run over the body for the last time - which is the answer the field had before.
     */
    bool undecidedFlag(InstAggregate& built, U16 field) {
        if(field != kRunOwnsHeap || !built.results.size()) return false;

        // The run row names the generic declaration and a descriptor is one of its instances -
        // `place.cpp`'s `isDescriptor` asks it the same two ways.
        auto type = opt.base[built.results.get(opt.base, 0)]->occurrence.type;
        auto descriptor = opt.program.roles.record(intrinsic::Known::run);
        if(!type || opt.decl[type]->kind != mir::Type::Record) return false;

        auto& record = *(mir::RecordType*)opt.decl[type];
        if(record.instanceOf != descriptor && &record != opt.decl[descriptor]) return false;
        if(!built.components.size()) return false;

        auto items = built.components.get(opt.base, 0);
        auto slots = items.isSet() ? opt.base[items.value()] : nullptr;
        if(!slots || !slots->definer) return false;

        auto request = opt.base[slots->definer];
        return request->kind == Inst::RunSlots && !((InstRunSlots*)request)->placed;
    }

    // `place.cpp`'s `kOwnsHeap`, and `known.def`'s `run` row is where the order is stated.
    static constexpr U16 kRunOwnsHeap = 2;

    /*
     * The fourth answer, and the one that crosses blocks: an allocation `aggregateFillOf` saw filled
     * exactly once, whole, by an `Aggregate`, and written by nothing else for the whole body - J137.
     *
     * The block table ends at every join and at every barrier, which is right for what it
     * holds and wrong for this: the fill is the *only* write, so no barrier can have changed what it
     * put there. What is left to ask is whether the fill has happened at all where the read is, and
     * that is dominance - the store standing above the read on every path in. A read it does not
     * dominate reads storage something may not have filled yet, and nothing is concluded.
     *
     * The components the aggregate was built from are defined above it and it is defined above the
     * store, so every value this hands back dominates the read as well. It is `[5, 6, 7]`'s case:
     * the literal is filled once in the entry, three bounds checks later its reclaim - copied in by
     * the inliner since `opt/teardown.cpp` made it a call - reads `.run.ownsHeap`, and that is the
     * constant zero `placeRuns` wrote into the aggregate.
     */
    Value* storedAggregate(Value* base) {
        if(!base || !reading || !base->definer) return nullptr;
        if(opt.base[base->definer]->kind != Inst::Alloc) return nullptr;

        auto fill = fillOf(*base);
        if(!fill || !fill->block || !reading->block) return nullptr;
        if(!storeReaches(*fill, *reading)) return nullptr;

        return opt.base[((InstStore*)fill)->value.value()];
    }

    // `aggregateFillOf`'s filter over the pass's memo - J138 memoized it here, J148 moved the memo
    // under `constantAt` too.
    Inst* fillOf(Value& allocation) {
        auto fill = slots.of(opt, contained, allocation);
        if(!fill) return nullptr;

        auto stored = opt.base[((InstStore*)fill)->value.value()];
        return opt.base[stored->definer]->kind == Inst::Const ? nullptr : fill;
    }

    bool storeReaches(Inst& store, Inst& read) {
        auto from = opt.base[store.block];
        auto to = opt.base[read.block];

        if(from != to) {
            if(!dominators) dominators = &dominatorsOf(opt);
            return dominators->dominates(from->index.index, to->index.index);
        }

        for(auto pointer: from->instructions(opt.base)) {
            auto inst = opt.base[pointer];
            if(inst == &store) return true;
            if(inst == &read) return false;
        }

        return false;
    }

    // The load `tryAggregateForward` is answering, for `storedAggregate`'s dominance question, and
    // the tree it asks - read the first time a fill is in play, since the pass edits no edge.
    Inst* reading = nullptr;
    Dominators* dominators = nullptr;

    // Whether the record `aggregateComponent` last answered from was a fact that crossed a call.
    bool componentCrossed = false;

    bool tryAggregateForward(Inst& inst) {
        auto& load = (InstLoad&)inst;
        if(!inst.results.size()) return false;

        auto result = opt.base[inst.results.get(opt.base, 0)];
        reading = &inst;
        componentCrossed = false;
        auto component = aggregateComponent(opt.base[load.address.value()], 0);
        reading = nullptr;
        if(!component || !isForwardable(*result, *component)) return false;

        // J154's reload rule, for a field of a record the table holds as well as for the whole
        // slot (J161): a component that crossed a call is read again unless it is a constant.
        // It used to be asked on the exact-address path only, which is how J159's `{0, 0}` reached
        // the reads after `readClock` - the wrong `mutates` row was the bug, but "a value that
        // crossed a call is reloaded" was not what happened to a field.
        if(componentCrossed && keepsReloads() && !isConstantValue(*component)) return false;

        auto editor = opt.ir();
        editor.replaceAllUses(*result, *component);
        editor.eraseInst(inst);
        return true;
    }

    /*
     * Whether a read after a call is left to read the storage again rather than answered by what
     * was there before the call - J154, the within-block half of `ends`' rule.
     *
     * Native only. There the answer that crossed the call is a value held in a register over it,
     * and a register over a call is a callee-saved one or a spill slot: a store and a load of a
     * second copy of what the slot the load reads already holds. On JavaScript a forwarded value is
     * a `var` and the reload a property read, and nothing is spilled.
     */
    bool keepsReloads() { return !isJsMode(opt.context.settings); }

    bool isConstantValue(Value& value) {
        return value.definer && opt.base[value.definer]->kind == Inst::Const;
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
        if(load.isVolatile) return false;
        if(!inst.results.size()) return false;

        auto result = opt.base[inst.results.get(opt.base, 0)];
        auto address = opt.base[load.address.value()];

        /*
         * An entry with a **stale** field is an aggregate that no longer describes the whole of the
         * storage - a later store replaced one field of it - so it answers a read of another field
         * (`aggregateComponent`) and never a read of the whole. The conversion never read a whole
         * record out of storage with a trivial load, it copied, so nothing reached this with one
         * until `aliasBorrowedCopies` made the copy a load, and then `let &b = Big {..}; b.depth =
         * Basic; show(b)` handed `show` the literal (J168).
         */
        auto entry = entryFor(address);
        if(!entry || !entry->value || entry->stale || !isForwardable(*result, *entry->value)) {
            if(tryAggregateForward(inst)) return true;
            return tryConstantForward(inst);
        }

        if(entry->crossed && keepsReloads() && !isConstantValue(*entry->value)) {
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
     * The value this address holds where a fact above names it: a store to the same address in
     * this block, or a field of an aggregate `aggregateComponent` answers. Unlike `tryForward`, the
     * convention is not asked - this is for a reader that wants to know *what* is there rather than
     * to be handed it, which is why a tracked value may answer.
     */
    Value* heldAt(Value* address, Inst& at) {
        // `stale` is not asked here: it marks fields of an `Aggregate` entry a later write replaced,
        // and the one reader of this wants a function value, which is never an aggregate - a stale
        // record answers `constantFunValue` nothing. A field read goes through `aggregateComponent`,
        // which does ask it.
        auto entry = entryFor(address);
        if(entry && entry->value) return entry->value;

        // A borrow of the slot names the slot: `retarget(s.run)` inlined drops and writes through
        // a `Guaranteed` load of the field address rather than through the address itself.
        address = throughNames(address);

        entry = entryFor(address);
        if(entry && entry->value) return entry->value;

        reading = &at;
        auto component = aggregateComponent(address, 0);
        reading = nullptr;
        return component;
    }

    /*
     * A **non-capturing** `fun.value` behind a held value - J145, J94 §6.2 E - through
     * `funValueBehind`, the walk the inliner reads too. Non-capturing is the whole condition: such a
     * value is a constant pair, `{thunk, null}`, so which declaration it names is all a call through
     * it or a drop of it needs, and where the `fun.value` stands does not matter. A capturing one
     * names an environment, which only the inliner's `devirtualizeSite` passes on.
     */
    InstFunValue* constantFunValue(Value* value) {
        if(!value) return nullptr;

        auto dropped = false;
        auto made = funValueBehind(opt, slots, contained, *value, dropped);
        return !made || made->capturing || !made->callee ? nullptr : made;
    }

    /*
     * A call through a function value read out of storage that holds a known non-capturing one,
     * made a direct call to the declaration - J145, J94 §6.2 E. The load itself is not forwarded
     * (its result is a borrow, and substituting one is the loan rule's, see the file comment); what
     * changes is only which callee its readers name, and the load is left for the dead-value rules
     * once no call reads it.
     *
     * The inliner's `devirtualizeSite` answers the same question where the body built the pair and
     * stored it once. This is the rest: `data Step {run: (Int) -> Int}` built by a literal and
     * called through `s.run(10)`, and a slot written again before the call.
     */
    bool tryDirectReaders(Inst& inst) {
        if(!inst.results.size()) return false;

        auto result = opt.base[inst.results.get(opt.base, 0)];
        if(result->convention == Convention::Trivial) return false;

        SmallArray<InstCall*, 4> calls;

        for(auto pointer: result->uses(opt.base)) {
            auto user = opt.base[pointer];
            if(user->kind != Inst::Call) continue;

            auto& call = (InstCall&)*user;
            if(call.through != mir::CallThrough::Value || !call.target.isSet()) continue;
            if(opt.base[call.target.value()] != result) continue;

            calls.push(&call);
        }

        if(!calls.size()) return false;

        auto made = constantFunValue(heldAt(opt.base[((InstLoad&)inst).address.value()], inst));
        if(!made) return false;

        auto changed = false;
        for(auto call: calls) {
            // A call naming the value twice is on the list twice, and the first rewrite erased it.
            if(!call->block) continue;
            changed = callDeclarationDirectly(opt, *call, *made) || changed;
        }
        return changed;
    }

    /*
     * A drop of storage holding a known non-capturing function value, which lowers to
     * `teardown(fun)` of a null environment - erased. J145, J94 §6.2 E: `s.run = triple` drops the
     * `double` the record literal put there, and the teardown call it became was the one instruction
     * left of the old value.
     */
    bool tryInertDrop(Inst& inst) {
        auto& drop = (InstDrop&)inst;
        if(!drop.address.isSet()) return false;

        if(!constantFunValue(heldAt(opt.base[drop.address.value()], inst))) return false;

        opt.ir().eraseInst(inst);
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

        // Not over a stale field, for `tryForward`'s reason: the storage holds the aggregate with
        // one field replaced, and storing the aggregate again is what puts that field back.
        auto entry = entryFor(address);
        if(!entry || entry->value != value || entry->stale) return false;

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

    /*
     * Each block's table as it stood at the end, for a successor that has that block as its only
     * way in - J153.
     *
     * The table used to start empty at every block, so a read in a block entered from one place was
     * answered by nothing written above it: `while length(stack) > 0: let high = stack[length(stack)
     * - 1]` read the length in the loop header and again in the body the header branches to, and
     * `test/bench/programs/Sort`'s partition loop kept both loads. A block with **one incoming
     * edge** starts where that edge left, because every execution of it has just run its
     * predecessor to the end and a terminator writes nothing. What is carried is the facts and not
     * the pending stores: a store the predecessor made may be read on the other edge out of it, so
     * the dead-store rule stays inside the block that wrote it.
     *
     * The values the facts name dominate the successor, since its predecessor does. Blocks are
     * walked in reverse postorder so that a predecessor is finished before its sole successor starts;
     * a back edge's target has two ways in and is never seeded.
     *
     * **An inherited fact ends at the first call**, whatever the call writes - `J152`'s
     * `crossedCall` rule one tier down, and measured the same way: forwarded past a call, a value
     * read in one block is held in a register over a call in the next, beside the copy in memory,
     * and the reload was cheaper. Carried across calls, `test/base` was 1,392 bytes larger than
     * without the seeding at all; ended at them, it is 16 smaller.
     */
    Array<Known> ends;
    Array<U32> endFirst;
    Array<U32> endCount;
    IndexSet ended;

    void seed(Block& block) {
        known.clear();

        auto incoming = block.incoming(opt.base);
        if(incoming.size() != 1) return;

        auto from = opt.base[incoming[0]]->index.index;
        if(from >= ended.size() || !ended[from]) return;

        for(U32 i = 0; i < endCount[from]; i++) {
            auto fact = ends[endFirst[from] + i];
            fact.pending = nullptr;
            fact.inherited = true;
            known.push(fact);
        }
    }

    void finish(Block& block) {
        auto index = block.index.index;
        if(index >= ended.size()) return;

        endFirst[index] = U32(ends.size());
        endCount[index] = U32(known.size());
        for(auto& fact: known) ends.push(fact);
        ended.set(index, true);
    }

    void run(Block& block) {
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
                tryDirectReaders(inst);

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

            if(inst.kind == Inst::Drop && tryInertDrop(inst)) continue;

            if(inst.kind == Inst::Call) {
                for(Size i = known.size(); i-- > 0;) {
                    if(known[i].inherited) known.remove(i);
                    else known[i].crossed = true;
                }
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
        // Asked of this slot alone, and only of a contained one - J138: the whole table was read
        // here once per allocation, and every removal moved its stamp, so it was rebuilt per removal.
        if(!contained[allocation]) return false;
        inert = constantHeldBy(opt, contained, allocation).isSet();

        // The occurrence rather than the type: a slot holding a borrowed occurrence of a type that
        // owes a teardown owes nothing, and one holding an owner of the same type owes everything.
        if(!inert && (hasTeardownAt(opt, alloc.held) || isLinearAt(opt, alloc.held))) return false;

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

/*
 * Whether any use of a global's address - or of a projection of it, or a second name for it - is
 * something other than the place of an access. J154, and `OptContext::addressedGlobals` is the reader.
 *
 * The admitted uses are the ones that touch the global's own storage and hand its address to
 * nobody: the address of a load (whose `Guaranteed` result is a second name for the storage, and is
 * followed), the address - not the value - of a store, the location of an atomic, the source of a
 * copy, and the base of a projection or a `represent`, which are followed. Everything else is an
 * escape: a call argument, an aggregate component, a stored value, an edge argument, a bitcast
 * (which is what `addressOf` is), a drop, whose teardown is handed the address.
 */
static bool globalAddressEscapes(OptContext& opt, Value& address, U32 depth) {
    if(depth > 16) return true;

    for(auto userPointer: address.uses(opt.base)) {
        auto& user = *opt.base[userPointer];

        auto follow = [&]() {
            if(!user.results.size()) return false;
            return globalAddressEscapes(opt, *opt.base[user.results.get(opt.base, 0)], depth + 1);
        };

        switch(user.kind) {
            case Inst::Load: {
                auto& load = (InstLoad&)user;
                if(opt.base[load.address.value()] != &address) return true;
                if(!user.results.size()) continue;

                auto result = opt.base[user.results.get(opt.base, 0)];
                if(result->convention == Convention::Guaranteed && follow()) return true;
                continue;
            }

            case Inst::Store: {
                auto& store = (InstStore&)user;
                if(opt.base[store.value.value()] == &address) return true;
                continue;
            }

            case Inst::Atomic: {
                auto& atomic = (InstAtomic&)user;
                for(Size i = 1; i < atomic.arguments.size(); i++) {
                    if(opt.base[atomic.arguments.get(opt.base, i).value()] == &address) return true;
                }
                continue;
            }

            case Inst::Copy:
                continue;

            case Inst::FieldAddress:
                if(follow()) return true;
                continue;

            case Inst::ElementAddress: {
                auto& element = (InstElementAddress&)user;
                if(opt.base[element.base.value()] != &address) return true;
                if(follow()) return true;
                continue;
            }

            case Inst::Unary:
                if(((InstUnary&)user).op != mir::UnaryOp::Represent) return true;
                if(follow()) return true;
                continue;

            default:
                return true;
        }
    }

    return false;
}

void findAddressedGlobals(OptContext& opt) {
    opt.addressedGlobals.clear();

    // `opt.function` and `opt.body` are left as they are: `globalAddressEscapes` reads uses and
    // nothing of the body being optimized. They used to be saved and put back around this loop,
    // which never changed them - review J161 §5.5.
    for(auto modulePointer: opt.program.modules) {
        for(auto functionPointer: opt.decl[modulePointer]->functions.contents(opt.decl)) {
            auto function = opt.decl[functionPointer];
            if(!function->reachable || !function->ssa) continue;

            auto body = opt.base[function->ssa];
            for(auto blockPointer: body->blocks.contents(opt.base)) {
                auto block = opt.base[blockPointer];
                if(block->deleted) continue;

                for(auto pointer: block->instructions(opt.base)) {
                    auto inst = opt.base[pointer];
                    if(inst->kind != Inst::GlobalAddress || !inst->results.size()) continue;

                    auto global = ((InstGlobalAddress*)inst)->global;
                    auto known = opt.addressedGlobals.get(global.offset);
                    if(known && known.unwrap()) continue;

                    auto escapes = globalAddressEscapes(opt, *opt.base[inst->results.get(opt.base, 0)], 0);
                    opt.addressedGlobals[global.offset] = escapes;
                }
            }
        }
    }

    opt.globalsScanned = true;
}

void forwardMemory(OptContext& opt) {
    Forwarder forwarder { opt, containedOf(opt) };

    auto count = U32(opt.body->blocks.size());
    forwarder.endFirst.resize(count);
    forwarder.endCount.resize(count);
    forwarder.ended.reset(count);

    // Reverse postorder first, so that a block's sole predecessor has finished before it starts -
    // see `Forwarder::ends`. What the order leaves out is unreachable and starts empty.
    SmallArray<U32, 64> order;
    for(auto index: dominatorsOf(opt).order) order.push(index);

    IndexSet visited;
    visited.reset(count);

    for(auto index: order) {
        auto block = opt.body->block(opt.base, BlockId(index));
        if(block->deleted) continue;

        visited.set(index, true);
        forwarder.seed(*block);
        forwarder.run(*block);
        forwarder.finish(*block);
    }

    for(auto blockPointer: opt.body->blocks.contents(opt.base)) {
        auto block = opt.base[blockPointer];
        if(block->deleted || visited[block->index.index]) continue;

        forwarder.known.clear();
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

/*
 * Whether every argument `user` hands `value` to is a borrow at a shared, call-bounded position of a
 * direct call - the reader condition of `aliasBorrowedCopies` and `tryElideCopy`, for one reader.
 * Call-bounded as well as shared: a position that publishes an origin hands back a loan rooted in the
 * argument, and over an alias a later write to the source would show through it.
 */
bool borrowsOnly(OptContext& opt, Inst& user, Value& value) {
    if(user.kind != Inst::Call) return false;

    auto& call = (InstCall&)user;
    if(call.through != mir::CallThrough::Declaration) return false;
    auto function = opt.decl[call.callee];

    for(Size a = 0; a < call.arguments.size(); a++) {
        auto used = call.arguments.get(opt.base, a);
        if(!used.isSet() || opt.base[used.value()] != &value) continue;
        if(used.kind() != UseKind::Borrow || a >= function->args.size()) return false;

        auto& parameter = *opt.decl[function->args.get(opt.decl, a)];
        auto contract = parameter.occurrence.contract;
        if(parameter.lazy || contract.isOwned() || contract.access() != mir::Access::Shared ||
           contract.origin() != mir::Origin::CallBounded) {
            return false;
        }
    }

    return true;
}

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

    /*
     * Whether the second slot is still live where the first is touched - a walk forward from the
     * touch that stops at the store (the one write that starts the second slot's life) and succeeds
     * at any other touch of the second slot. A block entered from its top is scanned once; the
     * touch's own block is scanned from just below the touch and, if control comes back round to it,
     * again from its top.
     */
    bool liveAt(Inst& touch, Inst& store, const IndexSet& intoTouches) {
        auto touchedInto = [&](SsaPtr<Inst> pointer) {
            return intoTouches[opt.base[pointer]->id.index];
        };

        auto& first = *opt.base[touch.block];
        auto from = SsaPtr<Inst>(&touch - opt.base);
        auto until = SsaPtr<Inst>(&store - opt.base);
        auto below = false;

        for(Size i = 0; i < first.instructionCount(); i++) {
            auto at = first.instructionAt(opt.base, i);
            if(at == from) { below = true; continue; }
            if(!below) continue;
            if(at == until) return false;
            if(touchedInto(at)) return true;
        }

        ScratchSet seen(opt.sets, opt.body->blocks.size());
        SmallArray<U32, 32> queue;

        auto enqueue = [&](U32 target) {
            if(seen->operator [] (target)) return;
            seen->set(target, true);
            queue.push(target);
        };

        forEachSuccessor(opt.base, first, enqueue);

        while(queue.size()) {
            auto block = opt.body->block(opt.base, BlockId(queue.pop().unwrap()));
            if(block->deleted) continue;

            auto stopped = false;
            for(Size i = 0; i < block->instructionCount() && !stopped; i++) {
                auto at = block->instructionAt(opt.base, i);
                if(at == until) stopped = true;
                else if(touchedInto(at)) return true;
            }

            if(!stopped) forEachSuccessor(opt.base, *block, enqueue);
        }

        return false;
    }

    /*
     * The same merge for a **duplicate** - `%v = copy %a` whose one use is `store %b, %v` - J168.
     *
     * `let token = step(lx, command)` over an inlined `nextToken`: the callee's slot is filled a field
     * at a time by `readToken(&token)`, and `return token` copies it into the caller's local - a
     * 61-byte block copy whose loads the store buffer cannot serve out of the narrow stores just in
     * front of them, which is where `test/bench/lexer`'s `hand` driver loop spent most of its time.
     * The two slots hold one value one after the other, as a move's do.
     *
     * What changes against a move is the first slot's life: a copy leaves it holding the value, and
     * a loop fills it again on the next iteration - reachable from the store, so `tryElide`'s second
     * test refuses it. What has to hold is narrower and is a liveness question: **the second slot is
     * dead wherever the first is touched** (`liveAt`), and nothing between the copy and the store
     * touches the first. Then a write to the first cannot reach a read of the second, and neither
     * slot's contents can be told apart from the merged one's. The second is also written by nothing
     * but the store - a `let` - so the merge writes nothing into the first that the program did not
     * write into it already.
     */
    bool tryElideCopy(Inst& store) {
        auto& written = (InstStore&)store;
        if(written.isVolatile) return false;

        auto copied = opt.base[written.value.value()];
        if(copied->convention != Convention::Trivial || !copied->definer) return false;

        auto copy = opt.base[copied->definer];
        if(copy->kind != Inst::Copy || copy->block != store.block) return false;

        auto from = allocationOf(((InstCopy*)copy)->address.value());
        auto into = allocationOf(written.address.value());
        if(!from || !into || from == into) return false;

        auto& source = (InstAlloc&)*opt.base[from->definer];
        auto& target = (InstAlloc&)*opt.base[into->definer];
        if(source.storage != target.storage) return false;
        if(!source.held.sameManifestation(target.held)) return false;

        auto& contained = containedOf(opt);
        if(!contained[from] || !contained[into]) return false;

        // Every touch of the second slot but the store is below it and writes nothing.
        IndexSet intoTouches;
        intoTouches.reset(opt.body->insts.size());
        auto fine = true;
        if(!eachTouch(*into, [&](Inst& use, SsaPtr<Inst>) {
            intoTouches.set(use.id.index, true);
            if(&use == &store) return;
            if(!use.block || !afterStore(store, use)) fine = false;
            else if(mayWriteStorage(opt, contained, use, *into)) fine = false;
        })) {
            return false;
        }

        if(!fine) return false;

        /*
         * The duplicate's other readers, where the forwarder has already answered a read of the
         * second slot with it: each has to be one `aliasBorrowedCopies` would accept - a borrow at a
         * shared, call-bounded position of a direct call - and below the store, and each is a read
         * of the second slot for the liveness question, since after the merge it reads the slot.
         */
        for(auto userPointer: copied->uses(opt.base)) {
            auto& user = *opt.base[userPointer];
            if(&user == &store) continue;
            if(!borrowsOnly(opt, user, *copied) || !user.block || !afterStore(store, user)) return false;
            intoTouches.set(user.id.index, true);
        }

        // Nothing touches the first slot between the copy and the store, and the second is dead at
        // every other touch of it.
        auto& block = *opt.base[store.block];
        auto copyAt = SsaPtr<Inst>(copy - opt.base);
        auto storeAt = SsaPtr<Inst>(&store - opt.base);

        if(!eachTouch(*from, [&](Inst& use, SsaPtr<Inst> pointer) {
            if(!fine || &use == copy) return;
            if(!use.block) { fine = false; return; }

            if(use.block == store.block && after(block, copyAt, pointer) && !after(block, storeAt, pointer)) {
                fine = false;
                return;
            }

            if(liveAt(use, store, intoTouches)) fine = false;
        })) {
            return false;
        }

        if(!fine) return false;

        auto editor = opt.ir();

        // Those readers read the merged slot - the rewrite `aliasBorrowedCopies` makes.
        if(copied->useCount() > 1) {
            editor.setBlock(block);
            editor.setInsertBefore(*copy);

            auto load = editor.addLoad(copy->source,
                                       ((InstCopy*)copy)->address.withKind(UseKind::Inspect),
                                       copied->occurrence, Convention::Trivial);
            editor.replaceAllUses(*copied, *editor.resultOf(*load));
        }

        editor.replaceAllUses(*into, *from);
        editor.eraseInst(store);
        editor.eraseInst(*copy);
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
            if(!store->block || tryElide(*store)) continue;

            // Native only, for `aliasBorrowedCopies`' reason: a record on JavaScript is an object
            // and `js/opt_alias.cpp` decides what a duplicate of one may share (J72).
            if(!isJsMode(opt.context.settings)) tryElideCopy(*store);
        }
    }
};

} // namespace

void elideMoves(OptContext& opt) {
    MoveElider elider { opt, dominatorsOf(opt) };
    elider.run();
}

/*
 * How many scalars a value of this type is made of - the fields of a record, its nested records'
 * fields and a fixed array's elements, each counted once. The target-independent stand-in for a size
 * this tier does not have (`manifestNative` is one tier down), and the question it answers is the
 * convention's: past `kAliasLeaves` a record crosses a join and, in practice, a call as its address
 * rather than as its cells.
 */
static U32 scalarLeaves(OptContext& opt, DeclPtr<mir::Type> type, U32 depth = 0) {
    constexpr U32 kMany = 1u << 16;
    if(!type || depth > 6) return kMany;

    auto declared = opt.decl[type];

    if(declared->kind == mir::Type::Record) {
        auto& record = *(mir::RecordType*)declared;
        U32 total = 0;

        for(auto field: record.fields.contents(opt.decl)) {
            total += scalarLeaves(opt, field.occurrence.type, depth + 1);
            if(total >= kMany) return kMany;
        }

        return total ? total : 1;
    }

    if(declared->kind == mir::Type::Array) {
        auto& array = *(mir::ArrayType*)declared;
        if(!array.hasFixedLength()) return 1;

        auto each = scalarLeaves(opt, array.element, depth + 1);
        return array.length >= kMany || each * array.length >= kMany ? kMany : U32(each * array.length);
    }

    return 1;
}

/*
 * The smallest record this aliases. Below it the copy is a handful of cells that `promote`, the
 * forwarder and the lowering's slot passes take apart anyway, and pointing a callee at the source
 * is what would keep the *source* in memory instead: `Contain.Call.yana`'s two-flag record lost its
 * folded field reads to it, and `ownership/PropertyWholePacked.yana`'s packed `Box` its promotion.
 */
constexpr U32 kAliasLeaves = 8;

/*
 * A trivial copy that only a borrow reads, made a read of the storage it was copied from - J168.
 *
 * `advance(d, token)` over a `let token` of a trivially copyable record reads `token` with
 * `copy.trivial` (`readMode`), because a copy is what satisfies a shared position for such a type,
 * and the copy is an `InstCopy` because the record lives in storage: handing the callee the caller's
 * own record where a copy was meant is the miscompile `InstCopy` exists to prevent. Where the source
 * is a small slot `promote` takes the copy back; where it is not - the lexer's 61-byte `Token`, whose
 * slot `readToken` fills through `&token` - it was a second slot and every word of the record moved
 * into it on every call, and `test/bench/lexer`'s driver loops spent 3-5% of their cycles on it.
 *
 * The duplicate and the source are the same program exactly when nothing can tell them apart, and
 * that is three conditions, with one more that is about cost:
 *
 *  - **every reader is a borrow at a shared, call-bounded position of a direct call** - so no reader
 *    keeps the value, stores it, returns it, projects into it, hands it to a callee that may write
 *    it or gets back a loan rooted in it. A `borrow` at a position the callee's own declaration says
 *    is `shared` is read-only by the language, and the only one of these whose declaration this can
 *    read is a direct call;
 *  - **every reader is in the copy's block and below it**, so "between the copy and its last use" is
 *    a stretch of one block, which is what makes the next condition a walk rather than an analysis;
 *  - **the record is more than `kAliasLeaves` scalars**, the stand-in for "large" this tier has -
 *    below it the copy costs nothing the slot passes do not take back, and an alias would pin the
 *    source in memory instead;
 *  - **nothing in that stretch may write the source's allocation** - `mayWriteStorage`, over a
 *    `Contained` allocation, the readers themselves included. What it catches at a reader is
 *    `f(p, &p)`: the second argument is a name of `p`'s slot at a position the callee writes, and
 *    over a copy the first argument would not see that write.
 *
 * **A rewrite into a load, which every pass below then sees.** A `load` of the same storage is a
 * copy the lowering never makes - a value that lives in storage is its address (`lower/inst.cpp`'s
 * `Load`), so every reader is handed the slot itself - and, unlike a copy, one `promote` and
 * `forwardMemory` can take away entirely where the slot has nothing else keeping it in memory. Both
 * alternatives were built and measured on `test/bench/lexer` (J168): marking the copy for the
 * lowering to skip, set on a body in its final shape, left every slot that promotion would have
 * taken apart in memory instead, and on the iterator variants it moved a block copy of the item
 * onto the path in front of the call, which a word-at-a-time store in front of it could not forward
 * - 7% slower than not aliasing at all, where this is 4% faster. The first build of this rewrite
 * also found that `forwardMemory` answered a whole-record load with an aggregate one of whose fields
 * had been written since (`Known::stale`, now asked), since nothing had ever read a record whole.
 *
 * Native only. On JavaScript the same copy is a `clone` that `js/opt_alias.cpp` already turns into
 * an alias on that target's own terms (J72), and a record there is an object rather than bytes.
 */
void aliasBorrowedCopies(OptContext& opt) {
    if(isJsMode(opt.context.settings)) return;

    auto& contained = containedOf(opt);
    SmallArray<Inst*, 16> copies;

    for(auto blockPointer: opt.body->blocks.contents(opt.base)) {
        auto block = opt.base[blockPointer];
        if(block->deleted) continue;

        for(auto pointer: block->instructions(opt.base)) {
            auto inst = opt.base[pointer];
            if(inst->kind == Inst::Copy) copies.push(inst);
        }
    }

    for(auto inst: copies) {
        auto& copy = (InstCopy&)*inst;
        if(!copy.block || !copy.results.size()) continue;

        auto result = opt.base[copy.results.get(opt.base, 0)];
        if(result->convention != Convention::Trivial || !result->useCount()) continue;
        if(scalarLeaves(opt, result->occurrence.type) <= kAliasLeaves) continue;

        auto allocation = allocationRoot(opt, opt.base[copy.address.value()]);
        if(!allocation || !contained[allocation]) continue;

        auto& block = *opt.base[copy.block];
        auto count = block.instructionCount();

        // The copy's position, and then the last reader's - every reader has to be found below it.
        Size at = count;
        for(Size i = 0; i < count; i++) {
            if(opt.base[block.instructionAt(opt.base, i)] == inst) { at = i; break; }
        }
        if(at == count) continue;

        auto readers = result->useCount();
        Size found = 0;
        auto safe = true;

        for(Size i = at + 1; i < count && found < readers && safe; i++) {
            auto& user = *opt.base[block.instructionAt(opt.base, i)];

            if(user.kind == Inst::Call) {
                auto& call = (InstCall&)user;
                auto named = false;

                for(Size a = 0; a < call.arguments.size(); a++) {
                    auto used = call.arguments.get(opt.base, a);
                    if(!used.isSet() || opt.base[used.value()] != result) continue;
                    found++;
                    named = true;
                }

                if(named && !borrowsOnly(opt, user, *result)) safe = false;
            }

            if(safe && mayWriteStorage(opt, contained, user, *allocation)) safe = false;
        }

        // A reader somewhere else - another block, or an instruction of a kind other than a call.
        if(!safe || found != readers) continue;

        auto editor = opt.ir();
        editor.setBlock(block);
        editor.setInsertBefore(copy);

        auto load = editor.addLoad(copy.source, copy.address.withKind(UseKind::Inspect),
                                   result->occurrence, Convention::Trivial);

        editor.replaceAllUses(*result, *editor.resultOf(*load));
        editor.eraseInst(copy);
    }
}

} // namespace ssa
