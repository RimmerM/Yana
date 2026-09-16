#include "pass.h"

namespace ssa {

/*
 * Where a run's slots live - the one placement decision this tier makes, and the only one it is
 * entitled to.
 *
 * ## What the decision is
 *
 * A run is `capacity` slots and a two-word descriptor over them, and `lib/Native/Run.native.yana`
 * splits the two: `runSlots` asks the allocator for the bytes, `runOver` builds the descriptor over
 * whatever came back, and the `ownsHeap` field carries the one thing the program did not write -
 * whether these bytes are anyone's to hand back. `newRun` is those two calls and `@inline(always)`,
 * so a body that builds a run has the *request* in its own frame rather than a heap buried three
 * functions down.
 *
 * This pass reads that request. A `runSlots` whose count is a constant and whose bytes the frame
 * can prove it never lets out is frame storage: the call becomes an `Alloc` of `[a *n]`, and the
 * `runFromHeap` the library passed becomes `runBorrowed`. Nothing else changes. `resize` still
 * relocates - a borrowed run's slots are not its to move, so it allocates and copies, which is
 * what the flag has always meant - and `Reclaim` still tests the same field and now folds to
 * nothing. That the library needs no rule about literals is the point: the shipping stage has
 * produced frame-placed runs since it had literals, and every reader of `HeapFlag` was written for
 * one.
 *
 * ## Why here and not in MIR
 *
 * `place.h` says SSA carries a placement down and does not make one, and that is still true of an
 * *object*: `analyze_escape.cpp` decides where a local lives and this pass touches none of them.
 * A run is not an object, and the reason is `analyze_escape.cpp`'s own thesis: **nothing escapes by
 * being a value**, because a returned record is copied into storage the caller passed in and its
 * storage stays behind. A run's buffer is the one counterexample in the language - the `Array` record
 * really is copied, and the bytes its `items` field points at are not - so the buffer cannot be an
 * object that walk places without the provenance dataflow that file declines by name. And the frame
 * that keeps the answer is not the one the request is written in either way: `leaving() = [7, 8, 9]`
 * is the allocator's under its own name and the frame's inside a caller that keeps what it returns.
 * So the question cannot be asked until the inliner has put the request in the frame that keeps the
 * answer, which is this tier and no earlier one. The shipping stage reaches the same conclusion from
 * the other end and re-runs its escape analysis behind `inlineCalls` for it.
 *
 * That the *request* is a library call rather than an allocation the literal expansion emitted is a
 * separate matter, and the shape this pass would rather have - see
 * `doc/progress/2026-09-09-J45-review-and-two-answers.md` §2.3, where what it would buy is costed:
 * the descriptor hunt below goes, the constant-count rule goes, and `runSlots` stops needing
 * `@noinline` to be findable.
 *
 * ## What makes it sound
 *
 * The buffer's address is in the frame if no value that may carry it leaves. So: taint the pointer,
 * follow it, and refuse at the first use that could put it somewhere this frame does not own.
 *
 *  - Through an `Aggregate` into the descriptor, and through a block argument, because that is the
 *    shape `runOver`'s refusal arm leaves - two arms joining on a `Run`.
 *  - Into memory only where the destination roots at a `Contained` `Alloc` of this body, and then
 *    that allocation is tainted too and everything read out of it is. A store whose root is a
 *    global, a parameter's pointer or an address loaded out of storage is an escape, which is the
 *    only sound reading of "somewhere else".
 *  - At a call argument, `Summary::escapes` **for a descriptor**, which is the row that says the
 *    callee wrote what it was handed somewhere outliving the call. A *bare address* at such a
 *    position does not ask: see `admitsCall`, and the bullet below it shares a reason with.
 *  - A comparison is a sink and not a carrier: `runOver` tests the pointer for null and a `Bool`
 *    is not a way back to anything.
 *  - **Anywhere the carrier is the address itself and is only handed over** - a native row's
 *    operand, a call argument. A raw pointer carries no `ownsHeap` word, so nothing reached through
 *    one can become a second owner of the bytes, and this body's reclaim stays where it was.
 *  - `Return` is an escape, and so is every kind not named - a `FunValue` environment, an `Atomic`.
 *    Forgetting too much costs a placement and forgetting too little is a use after free, so the
 *    default declines.
 *
 * **And what would make it wrong is a transfer, not a read.** The bytes are handed back by the
 * `Reclaim` this body emits, testing the `ownsHeap` the descriptor carries, so a frame answer is
 * wrong exactly where that reclaim is *not* the last event of the bytes' life - which is where a
 * descriptor over them was moved into something outliving the frame. Every refusal above is one of
 * those, and a use that only reads the address is not: with the run on the heap the reclaim runs at
 * the same instruction and frees the same bytes, so an address that outlived the frame outlives the
 * storage under both answers. That program is wrong before this pass sees it, and the analysis that
 * says so is the loan checker - `analyze_escape.cpp`'s seeds are its three conformance failures.
 * Placement is not a second, weaker copy of it, and had been charging every `print` an
 * `allocateHeap`/`freeHeap` pair for pretending to be one.
 *
 * The one route the walk does **not** follow is a callee reading the pointer back out of a slot it
 * was lent, and it does not follow it because the language does not have it: what such a callee
 * holds is a loan, and a loan of this frame's storage that outlives the frame is a *diagnostic*
 * here rather than a placement. `analyze_escape.cpp` says so in as many words - the escape seeds
 * are the loan checker's three conformance failures, and until a closure environment can be heap-
 * placed those sites refuse rather than report a placement. So containment of the slot plus the
 * loan rule is the whole of that case, and it is the one thing this pass gets from being in the
 * ownership stage rather than the shipping one, where the same question costs an 800-line dataflow.
 *
 * **And no two lifetimes of the bytes may be live at once.** One frame slot is per activation and a
 * heap request is per execution, so a literal built in a loop and kept past the iteration would be
 * one buffer behind two live descriptors. **The shipping stage does not have this rule and
 * miscompiles for it** - a loop that keeps one iteration's array in `prev` while the next is built
 * reads the new one back through the old descriptor (`test/bench/findings.md` §91.5), which is what
 * says the rule is load-bearing rather than cautious. The rules above do not catch that on their
 * own: writing the array into another frame slot is admitted, and it is admitted for good reason -
 * moving a value between two of this frame's slots is not leaving the frame. What this adds is that
 * the two lifetimes are the same one.
 *
 * It was "the request may not be in a loop" until J46 step 5, and it is now the liveness argument
 * that note asked for - `oneAtATime`, which admits a request inside a loop when the previous
 * iteration's descriptor is gone before the next one is built. What it buys is the common shape,
 * `for x in xs: let &row = [1, 2, 3]`; what it still refuses is §91.5's, and the two are told apart
 * by whether the slot the descriptor was written into is read before it is written on the way round.
 * A *dynamic* frame allocation needs a release point on top wherever it is in a loop, since one
 * released at frame exit grows the frame per iteration; that is why `sized` still asks for a number.
 *
 * ## What it is worth
 *
 * The 14 `test/resolve` fixtures where only this stage allocated, and the reclaim in the 74 where
 * both did - `doc/progress/2026-09-09-J43-six-items.md` §4 is the census, and J44 is the round.
 */

namespace {

/*
 * The run's three fields, by position - `known.def`'s `run` row, where the dependency is stated.
 *
 * And the two answers `ownsHeap` takes here. `runBorrowed` is zero because it is the arm that
 * releases nothing, which is the same number and the same argument the shipping stage's
 * `buildRunInto` writes at every literal: a run whose flag nothing ever set is one nobody hands
 * back, and that is the direction a missing decision has to fail in.
 */
constexpr U16 kFieldCount = 3;
constexpr U16 kItems = 0;
constexpr U16 kOwnsHeap = 2;

constexpr U64 kRunBorrowed = 0;
constexpr U64 kRunFromHeap = 1;

// The pointee of a `%T`, or null where the type is not a pointer. Written here rather than asked of
// `mir::` because the one caller wants the answer as "or not a pointer at all".
DeclPtr<mir::Type> pointeeOf(OptContext& opt, DeclPtr<mir::Type> type) {
    if(!type || opt.decl[type]->kind != mir::Type::Pointer) return nullptr;
    return ((mir::PointerType*)opt.decl[type])->to;
}

struct RunPlacement {
    OptContext& opt;

    // The `Run` record, from `known.def` - null in a library that declares none, which is the
    // program this pass does nothing to.
    DeclPtr<mir::RecordType> descriptor = nullptr;

    /*
     * The library's `runSlots`, still named here - but for **normalization** rather than for
     * recognition, which is the difference J46 part 3 made.
     *
     * A literal's request is an `InstRunSlots` from the resolver and needs no finding. What still
     * arrives as a call is every run the *library* builds - `String.ofCapacity`, `growArray`,
     * `resize` - and a bounded string interpolation is exactly one of those: its capacity is not a
     * number until `fold.cpp` has run, one tier below the resolver, so the request cannot be
     * emitted at the literal and is a call whose argument folds. `normalize` below turns such a
     * call into the instruction, and from there the walk has one shape to reason about.
     */
    DeclPtr<mir::Function> request = nullptr;

    // Values that may carry the buffer's address, and the allocations they may have been written
    // into. Two sets rather than one because the second answers a question about *storage* - what a
    // load out of it produces - and the first about a value.
    IndexSet carries;
    IndexSet within;

    /*
     * Which of the carriers were reached **through storage** - `reread`, and the one distinction
     * the two questions this walk answers do not share.
     *
     * Containment is a proof and has to over-approximate: a pointer stored into a slot may come
     * back out of *any* read of that slot, so `reread` taints every load under it and the answer is
     * only ever "this may still be the address". The refusal rewrite is the opposite kind of claim -
     * it replaces a comparison with a constant - so it may only read carriers the walk is *sure*
     * about, which is the ones it reached without passing through memory.
     *
     * Without the distinction a whole-record store of anything holding a run pointer taints every
     * field of that record, and `eq(field, 0)` on an unrelated integer field then reads as the
     * allocator's refusal test and folds to `False`. An iterator object's `$index` is what found it:
     * `f$new` stores the captured window - which carries the run - and the record whole, so the
     * step's `switch` on the resume index compared a "carrier" against zero. J52 is the account.
     */
    IndexSet reloaded;

    // Whether the value the worklist is following arrived through storage, which is what `taint`
    // writes into `reloaded`. A field rather than a parameter because `reread` recurses.
    bool throughStorage = false;

    /*
     * Where in the holder the pointer went, and where the descent is standing - `overlaps`, and the
     * offset precision J64 gave this walk.
     *
     * Without them a whole-record store taints every load anywhere under the holder, because "may
     * come back out" was asked of the *allocation* rather than of the place. That is the answer for
     * the field the pointer went into and is wrong for its siblings, and a `String`'s siblings are
     * its `length` - a thirty-bit count that reaches a `return` in the same body the moment
     * `appendUnits` is small enough to inline. `StringReclaim.yana` is the fixture.
     *
     * Fields rather than parameters for `throughStorage`'s reason, and cleared per store: one
     * holder written twice is two walks, each with its own place, and `taint` is idempotent.
     */
    Array<AddressStep> target;
    Array<AddressStep> here;
    bool targeted = false;

    // The same allocations as a list, because the loop rule walks them and a set answers only
    // membership. Filled beside `within` and cleared with it.
    Array<SsaPtr<Value>> holders;

    Array<SsaPtr<Value>> pending;

    // The stores of a carrier into a holder, kept until the worklist is empty - see the `Store` arm.
    Array<SsaPtr<Inst>> stores;

    /*
     * Which instructions the three lists below already hold - membership beside a list, which is the
     * arrangement `within` and `holders` make one field up and is here for the *same* reason twice
     * over.
     *
     * A list needs it because the walk reaches one instruction more than once: a value whose place
     * widens goes back on the worklist, and its whole use list is followed again. Without the set the
     * second visit pushes the descriptor a second time, `soleDescriptor` counts two and refuses a
     * placement it had found; a refusal test pushed twice is erased twice, which is an assertion. A
     * scan of the list would answer as well and is what `stores` was doing - quadratic in a body's
     * stores, for a question a bit answers.
     */
    IndexSet collected;

    // Whether this instruction is newly collected, and a note that it is.
    bool collect(Inst& inst) {
        if(collected[inst.id.index]) return false;

        collected.set(inst.id.index, true);
        return true;
    }

    // One allocation's two events, over instruction ids - see `classifyStorage`. Rebuilt per
    // allocation, and `live` per loop beside them.
    IndexSet reads;
    IndexSet kills;
    IndexSet live;

    // The `Run` aggregates the buffer reached, and the one thing the walk collects rather than
    // clears - see the `Aggregate` arm.
    Array<SsaPtr<Inst>> descriptors;

    /*
     * The comparisons against null that ask whether the allocator refused - collected for the same
     * reason the descriptors are, and discharged by the same rewrite.
     *
     * **A frame allocation does not fail**, so a placement makes this test answer a constant. It is
     * this pass's to answer and nobody else's: the resolver emits the test because it does not yet
     * know where the bytes will be, and no later pass knows that a `RunSlots` marked `placed`
     * answers an address of this frame. Leaving it was a comparison, a branch and an abort block at
     * every placed literal - which is what the first measurement of J46 part 3 found and is why
     * this is here.
     */
    Array<SsaPtr<Inst>> refusals;

    RunPlacement(OptContext& opt): opt(opt) {
        descriptor = opt.program.roles.record(intrinsic::Known::run);
        request = opt.program.roles.function(intrinsic::Known::runSlots);
    }

    SsaBase base() { return opt.base; }

    /*
     * Whether this aggregate is the run's own descriptor, with the buffer as its slots.
     *
     * Both halves are asked. The record has to be the one `known.def`'s `run` row names, because
     * the field this pass rewrites is that record's third and a different record's third field is
     * a different thing; and the pointer has to be at field zero, because a run that held the
     * buffer somewhere else would be one whose `items` this pass had not accounted for.
     */
    bool isDescriptor(Inst& user, Value& value) {
        if(!user.results.size()) return false;

        auto& made = *base()[user.results.get(base(), 0)];
        auto type = made.occurrence.type;
        if(!type || opt.decl[type]->kind != mir::Type::Record) return false;

        auto& record = *(mir::RecordType*)opt.decl[type];
        if(record.instanceOf != descriptor && &record != opt.decl[descriptor]) return false;

        auto& components = ((InstAggregate&)user).components;
        if(components.size() != kFieldCount) return false;

        auto items = components.get(base(), kItems);
        return items.isSet() && base()[items.value()] == &value;
    }

    /*
     * Whether this equality is the literal's "did the allocator refuse" test - the tainted value on
     * one side and a zero on the other.
     *
     * Both halves asked, because an equality against a *different* constant is a comparison of the
     * address with something and is not this question. `constantOf` rather than a kind test on the
     * instruction, so that a zero which arrived through a copy or a block argument is still one.
     */
    bool isRefusalTest(InstBinary& binary, Value& value) {
        auto lhs = base()[binary.lhs.value()];
        auto rhs = base()[binary.rhs.value()];
        auto other = lhs == &value ? rhs : (rhs == &value ? lhs : nullptr);
        if(!other) return false;

        auto held = constantOf(opt, other);
        return held && held->kind == mir::Constant::Int && held->asInt() == 0;
    }

    /*
     * Where inside a carrier the pointer sits - the second coordinate this walk carries, and what
     * makes `reread` a question about a *place* rather than about an allocation.
     *
     * A carrier is not always the pointer. A `String` is a record that holds one at `.units.run
     * .items`, and a body that stores such a record whole into a slot has put the pointer at that
     * chain within the slot and has put nothing anywhere else in it - so a load of `.units.length`
     * out of that slot cannot answer with the address. Without this the walk had to say it might,
     * and the count reaching a `return` refused the placement: `StringReclaim.yana` and
     * `Format.yana` are the two fixtures, and J64 is the account.
     *
     * Three states rather than two. `Unset` is a carrier nothing has said anything about yet,
     * `Known` is a chain, and `Unknown` is a carrier reached two ways that disagree or reached a
     * way this cannot describe. The lattice only ever moves `Unset` -> `Known` -> `Unknown`, which
     * is what makes the rounds in `inFrame` terminate: a value is re-walked at most twice, and the
     * answer it ends at is the widest any route gave it.
     *
     * Four steps deep, which is one more than the deepest chain in the library - `String` is three.
     * A pointer buried deeper than that is `Unknown`, which is what the walk said about every
     * carrier before this existed.
     */
    static constexpr U8 kPlaceUnset = 0;
    static constexpr U8 kPlaceKnown = 1;
    static constexpr U8 kPlaceUnknown = 2;
    static constexpr U32 kPlaceDepth = 4;

    struct Place {
        AddressStep steps[kPlaceDepth];
        U8 length = 0;
        U8 state = kPlaceUnset;
    };

    /*
     * The table: one word per value naming a row, and a row per carrier the walk in progress reached.
     *
     * **Indirect rather than a `Place` per value**, because a `Place` is a hundred and thirty bytes
     * and the values that hold one are the tens of carriers one request reaches - a row per value
     * would be most of a megabyte of a body this never looks at, written and never read. The rows
     * are cleared per request and the two words per value are not: `stamp` is what tells a row of
     * this walk from one the last walk left, which is the arrangement `Containment::visiting` makes
     * with its set and is here for the same reason - `inFrame` runs once per request and there are
     * tens of them in a body.
     */
    Array<U32> placeStamp;
    Array<U32> placeRow;
    Array<Place> rows;
    U32 stamp = 0;

    // The pointer itself, and anything derived from one: the carrier is the whole of the value.
    Place whole() {
        Place place;
        place.state = kPlaceKnown;
        return place;
    }

    Place nowhere() {
        Place place;
        place.state = kPlaceUnknown;
        return place;
    }

    Place placeOf(Value& value) {
        auto index = value.id.index;
        if(index >= placeStamp.size() || placeStamp[index] != stamp) return Place();

        return rows[placeRow[index]];
    }

    // Whether two chains are one chain - `widen`'s comparison, which is `overlaps`' asked of two
    // finished places rather than of one being walked.
    bool samePlace(const Place& left, const Place& right) {
        if(left.length != right.length) return false;

        for(U8 i = 0; i < left.length; i++) {
            if(!sameAddressComponent(opt, left.steps[i], right.steps[i])) return false;
        }

        return true;
    }

    // One route's answer, folded into the one already recorded - and whether that moved it.
    bool widen(Value& value, const Place& place) {
        auto index = value.id.index;

        while(placeStamp.size() <= index) { placeStamp.push(0); placeRow.push(0); }

        if(placeStamp[index] != stamp) {
            placeStamp[index] = stamp;
            placeRow[index] = U32(rows.size());
            rows.push(Place());
        }

        auto& held = rows[placeRow[index]];
        if(held.state == kPlaceUnknown) return false;

        if(held.state == kPlaceUnset) {
            held = place.state == kPlaceKnown ? place : nowhere();
            return true;
        }

        // A second route saying what the first one said moves nothing; one saying anything else
        // leaves a carrier whose place this cannot describe.
        if(place.state == kPlaceKnown && samePlace(held, place)) return false;

        held = nowhere();
        return true;
    }

    void taint(Value* value, const Place& place) {
        if(!value) return;

        auto index = value->id.index;
        auto fresh = !carries[index];

        if(fresh) {
            carries.set(index, true);
            if(throughStorage) reloaded.set(index, true);
        }

        // A value whose place widened is one everything derived from it has to be told about, so it
        // goes back on the worklist however long it has been off it.
        auto moved = widen(*value, place);
        if(fresh || moved) pending.push(value - base());
    }

    /*
     * Whether the carrier is the address itself rather than something holding it - the one reading
     * of a `Place` two of the arms below turn on, and the distinction the pass's own note argues.
     *
     * Length zero is the pointer, or a projection of it, which is the same word plus a displacement.
     * Anything longer is a record the pointer sits inside, and a record is the thing that can be
     * moved and so the thing that can carry `ownsHeap` out of this frame.
     */
    bool bareAddress(Value& value) {
        auto carried = placeOf(value);
        return carried.state == kPlaceKnown && !carried.length;
    }

    // The place a component of a record sits at, seen from the record - `[field] ++ the component's
    // own place`, or nothing where the record is one this stage may not index.
    Place componentPlace(InstAggregate& aggregate, Value& value) {
        if(!aggregate.results.size()) return nowhere();
        if(aggregate.constructor != 0) return nowhere();

        auto record = base()[aggregate.results.get(base(), 0)]->occurrence.type;
        if(!record) return nowhere();

        auto declared = opt.decl[record];
        if(declared->kind != mir::Type::Record) return nowhere();
        if(((mir::RecordType*)declared)->layout != mir::RecordLayout::Single) return nowhere();

        U16 at = 0;
        U16 index = 0;
        auto found = false;

        for(auto used: aggregate.components.contents(base())) {
            if(used.isSet() && base()[used.value()] == &value) {
                // One pointer in two fields is two places, and this carries one.
                if(found) return nowhere();
                found = true;
                at = index;
            }

            index++;
        }

        if(!found) return nowhere();

        auto inner = placeOf(value);
        if(inner.state != kPlaceKnown) return nowhere();
        if(inner.length + 1 > kPlaceDepth) return nowhere();

        Place place;
        place.state = kPlaceKnown;
        place.length = (U8)(inner.length + 1);
        place.steps[0] = AddressStep { Inst::FieldAddress, record, at, nullptr, nullptr,
                                       InstFieldAddress::kNoProperty };
        for(U8 i = 0; i < inner.length; i++) place.steps[i + 1] = inner.steps[i];

        return place;
    }

    /*
     * One use of a tainted value, and whether the address may still be in this frame after it.
     *
     * The switch is total on purpose and the default declines - `Contained::admits` is the
     * neighbour this is written like, and for its reason.
     */
    bool admits(Inst& user, Value& value) {
        auto named = [&](Use used) { return used.isSet() && base()[used.value()] == &value; };

        switch(user.kind) {
            /*
             * The descriptor, which is the one use this pass has to *find* rather than clear: the
             * flag it carries is the answer being rewritten. Recorded here, where the walk is
             * already standing on it, and checked once at the end - a pointer reaching two of them
             * or none is a shape this pass does not recognise and does not touch.
             */
            case Inst::Aggregate: {
                if(isDescriptor(user, value) && collect(user)) descriptors.push(&user - base());

                auto inner = componentPlace((InstAggregate&)user, value);
                for(Size i = 0; i < user.results.size(); i++) {
                    taint(base()[user.results.get(base(), i)], inner);
                }

                return true;
            }

            /*
             * A projection, a choice between two values and a restatement of one - each keeps the
             * address exactly as far as its own result does, which is the recursion this walk is.
             */
            case Inst::FieldAddress:
            case Inst::ElementAddress:
            case Inst::Select:
            case Inst::Unary: {
                /*
                 * A restatement and a choice both answer with the value they were given, so the
                 * place travels with them. A projection is taken *of an address*, so what it
                 * answers with is a pointer and the carrier is the whole of it - which is what the
                 * place already says where the carrier reaching here is one.
                 */
                auto inner = placeOf(value);
                if(user.kind == Inst::FieldAddress || user.kind == Inst::ElementAddress) {
                    inner = inner.state == kPlaceKnown && !inner.length ? whole() : nowhere();
                }

                for(Size i = 0; i < user.results.size(); i++) {
                    taint(base()[user.results.get(base(), i)], inner);
                }

                return true;
            }

            /*
             * A comparison, which is where `runOver` asks whether the allocator refused. What comes
             * out is a `Bool` and a `Bool` is not a way back, so this is a sink rather than a step -
             * and it is the one arithmetic use admitted, because pointer arithmetic that is not a
             * comparison produces another address and is caught by the projection arm above.
             */
            case Inst::Binary: {
                auto& binary = (InstBinary&)user;
                if(!mir::isComparison(binary.op)) {
                    for(Size i = 0; i < user.results.size(); i++) {
                        taint(base()[user.results.get(base(), i)], nowhere());
                    }

                    return true;
                }

                /*
                 * An equality against zero is the refusal test - see `refusals`. Order comparisons
                 * and equalities against anything else are sinks like any other and say nothing.
                 *
                 * And only where this carrier is the pointer rather than something a read of
                 * storage *may* have answered with - `reloaded`, where the reason is written. A
                 * carrier that came back out of memory is a proof obligation and not a fact, so
                 * folding a comparison of one is reading the wrong set.
                 */
                if(mir::binaryCategory(binary.op) == mir::OpCategory::Equality &&
                   !reloaded[value.id.index] && isRefusalTest(binary, value) && collect(user)) {
                    refusals.push(&user - base());
                }

                return true;
            }

            /*
             * Into storage. The address has to root at an allocation of this body that nothing else
             * can name; then the allocation carries the pointer and every load that **may read the
             * place it was written** does too.
             *
             * A store *through* the tainted address is the other direction and says nothing: it is
             * a write into the buffer, which is what the buffer is for.
             */
            case Inst::Store: {
                auto& store = (InstStore&)user;
                if(!named(store.value)) return true;

                auto written = base()[store.address.value()];
                auto root = allocationRoot(opt, written);
                if(!root || !containedOf(opt)[root]) return false;

                if(!within[root->id.index]) {
                    within.set(root->id.index, true);
                    holders.push(root - base());
                }

                /*
                 * Recorded rather than walked here - `inFrame`'s rounds, and the reason is what the
                 * walk below reads. Where in the holder the pointer went is decided by which
                 * *components* of the stored value carry it, and the worklist is not finished
                 * saying which do: a component reached after this store would narrow the answer
                 * that had already been given. So every store is walked once the worklist is empty
                 * and walked again if that taints anything, until nothing does.
                 */
                if(collect(user)) stores.push(&user - base());
                return true;
            }

            /*
             * A read of what is *at* the tainted address, which is the buffer's contents and not
             * its base. An element is what the run holds and the caller put there; carrying the
             * taint into it would make `xs[0] + xs[1]` an escape the moment the sum was returned,
             * which is what it did before this arm said so.
             *
             * The other direction - a read out of storage the buffer was written into - is not this
             * arm at all. `within` is the set for that, and `reread` walks it, because the address
             * a body loads the descriptor back through is a projection of the slot rather than the
             * slot itself.
             */
            case Inst::Load:
            case Inst::Copy:
                return true;

            /*
             * A teardown of storage holding the run. It reads the flag this pass is about to
             * rewrite and hands the bytes back or does not, which is the whole arrangement - and it
             * takes an address rather than the value, so nothing travels.
             */
            case Inst::Drop:
                return true;

            /*
             * An edge - the value arrives in the successor as a block argument, so the argument at
             * this position carries what the operand did.
             */
            case Inst::Jump:
            case Inst::Branch:
            case Inst::Switch:
                return followEdges(user, value);

            case Inst::Call:
                return admitsCall((InstCall&)user, value);

            /*
             * A native row's operand - and the arm where **taking an address is not a transfer** is
             * written down.
             *
             * What this pass decides is where a run's *bytes* live, and the bytes are handed back by
             * `Reclaim` reading `ownsHeap` out of the descriptor. So the only way the answer can be
             * wrong is if a descriptor over these bytes comes to be owned by something that outlives
             * the frame - because that is the one case where the reclaim is *not* emitted here. A
             * raw address carries no `ownsHeap` word, so it is structurally incapable of being that:
             * handing one to a native row leaves this frame's reclaim exactly where it was, and the
             * bytes therefore die at the same instruction under either placement.
             *
             * Which is why refusing here bought nothing. A `write` that keeps the pointer past the
             * call is a use-after-free with the run on the heap too - `String.reclaim` runs before
             * the `ret` either way - so the refusal never protected a program, it only charged every
             * `print` an `allocateHeap`/`freeHeap` pair. **A pointer outliving the local it names is
             * the loan checker's question**, it is answered where the annotation is, and a placement
             * that re-asks it with less information is a second, weaker copy of an analysis that
             * already exists.
             *
             * The place is what says which of the two this carrier is. Length zero is the address
             * itself - the buffer pointer, or a projection of it, which is the same word plus a
             * displacement. Anything longer is a *record holding* the pointer, and a block move
             * reading those bytes copies the pointer out with them, so that one still declines.
             *
             * Results carry with an unknown place, which is `admitsCall`'s rule and for its reason:
             * `mremap` answers with an address, and a row whose answer nothing unsafe is done with
             * is one the walk clears anyway.
             */
            case Inst::Native: {
                if(!bareAddress(value)) return false;

                for(Size i = 0; i < user.results.size(); i++) {
                    taint(base()[user.results.get(base(), i)], nowhere());
                }

                return true;
            }

            // Out of the frame, which is the question this pass is asking.
            case Inst::Return:
                return false;

            default:
                return false;
        }
    }

    /*
     * A call argument, and the one row that answers it.
     *
     * `escapes` is the whole answer since J48. It means "a value or an address derived from this
     * position was stored somewhere outliving the call", which is the question this walk asks
     * whether the buffer arrives as the pointer itself or inside a descriptor - so the type test
     * that used to pick between two rows here is gone with the second row's reason for being.
     *
     * That second row was `retains`, and it was the wrong one: it counts what a call hands *back*,
     * so every accessor in the library published its subject and the fixpoint carried that up to
     * `String.appendUnits`. The handed-back half is this walk's own to follow, and it does, below.
     * `findings.md` §98 is the trace.
     *
     * An unfilled row reads back as every argument, so a callee nothing analysed refuses here
     * without a special case.
     */
    bool admitsCall(InstCall& call, Value& value) {
        /*
         * A call whose callee is not a declaration answers nothing about what it keeps. For a slot
         * dispatch or a call through a function value, `callee` is the *signature* the arguments
         * were checked against rather than the body that runs - and a signature has no summary to
         * read, because what runs is whatever was in the slot. So the pointer stops here.
         */
        if(call.through != mir::CallThrough::Declaration || !call.callee) return false;

        /*
         * **A raw address is admitted at every position**, and the row is not consulted for one.
         *
         * `escapes` answers "was the address the caller handed over written somewhere outliving the
         * call", which is `ssa::Contained`'s question and the right one for it - a second name for
         * the storage is a second name whatever made it. It is not this pass's question. What would
         * make a frame answer wrong is the bytes being read after this body's `Reclaim`, and the
         * reclaim moves only when a *descriptor* is moved, because the descriptor is what carries
         * `ownsHeap`. A callee handed a bare pointer gets no `ownsHeap` with it and can transfer
         * nothing; this frame still reclaims at the same instruction.
         *
         * And a frame placement is safe for the pointer itself on the analysis that already owns
         * that question. A `%T` into a local's storage is a loan of that local, the loan checker
         * holds it to the local's lifetime and errors where it would outlive it - and the *frame*
         * outlives the local, which the buffer now lives in. So the pointer cannot reach past the
         * bytes unless a program the checker already rejected got here.
         *
         * `String.appendUnits(self, from, count)` is what the row was costing: `from` is a
         * `%CodeUnit` the callee copies out of and stores nothing of, but it reads as kept for the
         * reason `resolve_mir/summary.cpp`'s block-move arm gives, and every caller with a buffer at
         * that position paid an `allocateHeap` for it. Six of `Format`'s were exactly this.
         *
         * A **descriptor** still asks. Length is what tells them apart - zero is the address itself,
         * longer is a record holding it, and a record is the thing that can be moved.
         */
        if(!bareAddress(value)) {
            auto kept = opt.decl[call.callee]->summary.escapes().get(mir::ArgumentSet::all());

            Size index = 0;
            for(auto used: call.arguments.contents(base())) {
                if(used.isSet() && base()[used.value()] == &value && kept.has(index)) return false;
                index++;
            }
        }

        /*
         * What the call answers may have been computed from what it was handed - a `bitcast` of the
         * pointer is a call at this tier, and so is any accessor handing back a run pointer. So
         * every result carries, which costs nothing where it does not: a result nothing does
         * anything unsafe with is a value this walk clears.
         */
        for(Size i = 0; i < call.results.size(); i++) {
            taint(base()[call.results.get(base(), i)], nowhere());
        }
        return true;
    }

    // The successor argument each edge operand becomes. `forEachEdge` is the terminator's own walk,
    // so the position within an arm is the position within the target block's argument list.
    bool followEdges(Inst& terminator, Value& value) {
        auto ok = true;

        forEachEdge(base(), terminator, [&](Edge& edge) {
            auto target = base()[edge.target];
            Size position = 0;

            for(auto used: edge.arguments.contents(base())) {
                if(used.isSet() && base()[used.value()] == &value) {
                    if(position >= target->argumentCount()) ok = false;
                    else taint(base()[target->argumentAt(base(), position)], placeOf(value));
                }

                position++;
            }
        });

        return ok;
    }

    /*
     * Everything read back out of storage the buffer was written into.
     *
     * A walk of the allocation's **address tree** and not of its use list, because a body does not
     * load a descriptor out of a slot: it takes the address of the field it wants and loads that.
     * A use-list scan would have missed `.run.items` and answered that the pointer was in the frame
     * because it could not see the instruction that took it out - which is the direction this walk
     * may not fail in. It is `Contained`'s own recursion with the admission replaced by a taint.
     */
    void reread(InstStore& store) {
        auto written = base()[store.address.value()];
        auto root = allocationRoot(opt, written);
        if(!root) return;

        // Everything this reaches is a carrier by way of memory - see `reloaded`. Saved and
        // restored rather than set, because this recurses and its caller may be following a value
        // that came through storage already.
        auto saved = throughStorage;
        throughStorage = true;

        target.clear();
        here.clear();
        targeted = pathTo(*root, *written, target);

        // And where inside the value that was stored the pointer sits - see `Place`. Where that is
        // not known the chain stops at the slot, which is a prefix, and a prefix is an overlap.
        if(targeted) {
            auto inner = placeOf(*base()[store.value.value()]);
            if(inner.state == kPlaceKnown) {
                for(U8 i = 0; i < inner.length; i++) target.push(inner.steps[i]);
            }
        }

        rereadFrom(*root);
        throughStorage = saved;
    }

    /*
     * The chain of steps from `root` down to the address that was written, outermost first, and
     * whether the walk got there at all.
     *
     * Not `allocationRoot`'s walk: that one also steps through a `Guaranteed` load and a
     * `represent`, which name the same storage but are no step of the projection language the
     * comparison below speaks. So this stops where `addressStepOf` stops, and a stop short of the
     * root is reported as "no path", which switches the filter off and leaves the old answer.
     */
    bool pathTo(Value& root, Value& written, Array<AddressStep>& path) {
        auto address = &written;

        while(address && address != &root) {
            Value* above = nullptr;
            auto step = addressStepOf(opt, *address, above);

            if(step && above) {
                path.push(step.unwrap());
                address = above;
                continue;
            }

            // A `Guaranteed` load and a `represent` name the same storage and are no step of the
            // chain, which is `addressStep`'s own list and the reason it is a second walk.
            auto through = addressStep(opt, address);
            if(!through) return false;

            address = through;
        }

        if(address != &root) return false;

        // Pushed leaf-first by the walk and read root-first by `overlaps`.
        for(Size i = 0, j = path.size(); i + 1 < j; i++, j--) {
            auto swapped = path[i];
            path[i] = path[j - 1];
            path[j - 1] = swapped;
        }

        return true;
    }

    /*
     * Whether the storage the descent is standing on may be the storage the pointer was written to.
     *
     * The two chains are compared from the root down, which is the direction a separation is only
     * meaningful in - a step tells two addresses apart only if every step above it was the same
     * step. Where one chain runs out the other is inside it, and a whole-record read of an ancestor
     * does reach the field the pointer went into, so a prefix is an overlap. `opt/memory.cpp`'s
     * `mayAlias` is the same comparison asked of two finished chains rather than of one being
     * walked.
     */
    bool overlaps() {
        if(!targeted) return true;

        auto depth = min(here.size(), target.size());
        for(Size i = 0; i < depth; i++) {
            if(separateAddressStep(opt, here[i], target[i])) return false;
            if(!sameAddressComponent(opt, here[i], target[i])) return true;
        }

        return true;
    }

    void rereadFrom(Value& address) {
        // Everything below a separated sibling is inside it, so the whole subtree goes with it.
        if(!overlaps()) return;

        for(auto userPointer: address.uses(base())) {
            auto& user = *base()[userPointer];

            switch(user.kind) {
                case Inst::Load:
                case Inst::Copy: {
                    /*
                     * The half of the chain the descent has not walked yet is exactly where the
                     * pointer sits inside the value that just came out of storage - which is what
                     * lets a `String` copied from one slot to another keep its place rather than
                     * tainting the whole of the second slot. `Format.yana`'s `main` is six of these.
                     */
                    auto remaining = nowhere();
                    if(targeted && here.size() <= target.size() &&
                       target.size() - here.size() <= kPlaceDepth) {
                        remaining.state = kPlaceKnown;
                        remaining.length = (U8)(target.size() - here.size());
                        for(U8 i = 0; i < remaining.length; i++) {
                            remaining.steps[i] = target[here.size() + i];
                        }
                    }

                    for(Size i = 0; i < user.results.size(); i++) {
                        taint(base()[user.results.get(base(), i)], remaining);
                    }

                    // And the borrow a `Guaranteed` load is, which names the same storage - the half
                    // `Contained` recurses through for the same reason, and the same storage is the
                    // same place in the chain, so the walk keeps the position it is standing at.
                    for(Size i = 0; i < user.results.size(); i++) {
                        auto result = base()[user.results.get(base(), i)];
                        if(result->convention == Convention::Guaranteed) rereadFrom(*result);
                    }

                    break;
                }

                /*
                 * A projection, which is a step down the chain - so the position moves with it and
                 * comes back afterwards. A `Unary` is the third way an address names storage another
                 * value already names, and it is no step: `represent` restates the occurrence and a
                 * `bitcast` of an address rereads the same bytes at another type.
                 */
                case Inst::FieldAddress:
                case Inst::ElementAddress:
                    for(Size i = 0; i < user.results.size(); i++) {
                        auto result = base()[user.results.get(base(), i)];
                        Value* above = nullptr;
                        auto step = addressStepOf(opt, *result, above);
                        if(!step) continue;

                        here.push(step.unwrap());
                        rereadFrom(*result);
                        here.pop();
                    }

                    break;

                case Inst::Unary:
                    for(Size i = 0; i < user.results.size(); i++) {
                        rereadFrom(*base()[user.results.get(base(), i)]);
                    }

                    break;

                default:
                    break;
            }
        }
    }

    /*
     * Whether the bytes this request answers stay in the frame.
     *
     * The walk is a worklist and not the recursion `Contained` uses, because what is followed here
     * is a *value* graph with joins in it - a block argument has as many definitions as the block
     * has predecessors - rather than a tree of projections.
     */
    bool inFrame(Value& pointer) {
        carries.reset(opt.body->values.size());
        within.reset(opt.body->values.size());
        reloaded.reset(opt.body->values.size());
        collected.reset(U32(opt.body->insts.size()));

        // Every list the walk fills, emptied where the walk starts rather than at its caller: the
        // four below and the three sets above are one piece of state, and a clear that lived
        // somewhere else is a clear that can be forgotten.
        pending.clear();
        stores.clear();
        rows.clear();
        descriptors.clear();
        refusals.clear();
        holders.clear();

        throughStorage = false;
        stamp++;
        taint(&pointer, whole());

        /*
         * Two nested loops and the outer one is the fixpoint - see the `Store` arm.
         *
         * The inner loop is the walk as it was: one carrier's use list at a time, until nothing is
         * left to follow. The outer one re-reads every holder from the carrier set the inner loop
         * finished with, and stops when a whole pass over them taints nothing new. `carries` only
         * grows, so the last pass is the one that decided every narrowing, and a component that
         * turned out to carry after all widens the chain on the pass that found it.
         */
        for(;;) {
            while(pending.size()) {
                auto value = base()[pending.pop().unwrap()];

                // What this carrier is followed as - see `reloaded`. A value reached through storage
                // hands that on to everything derived from it.
                throughStorage = reloaded[value->id.index];

                for(auto userPointer: value->uses(base())) {
                    if(!admits(*base()[userPointer], *value)) return false;
                }
            }

            auto recorded = stores.size();
            for(Size i = 0; i < recorded; i++) reread((InstStore&)*base()[stores[i]]);

            // A store found while re-reading is one nothing has walked, so the rounds go on.
            if(!pending.size() && stores.size() == recorded) break;
        }

        throughStorage = false;

        return true;
    }

    /*
     * How many slots this request asks for, and whether the frame can state it as a type.
     *
     * **The count is a field now**, which is the difference J46 part 3 made here. It used to be an
     * argument that had to be folded - `constantOf` on operand one of a call, declining a dynamic
     * one - and the note that stood here argued why a dynamic count has no frame answer. That is
     * still true and is now stated one tier up: `mir::NodeRunSlots` carries a number because the
     * resolver only builds one where it has a number, and a run whose size is computed is still the
     * library's `newRun`. So what is left to check is the *bound*, which is what the type this
     * becomes can hold.
     */
    bool sized(InstRunSlots& slots) {
        return slots.capacity && slots.capacity <= mir::kMaxFixedArrayLength;
    }

    /*
     * A `runSlots` call whose count has folded, turned into the instruction a literal builds.
     *
     * **One shape for the walk below**, which is the whole reason this is a rewrite rather than a
     * second recognition path: the placement question, the loop test, the escape walk and the flag
     * rewrite are all written once, against `InstRunSlots`, and a call that means the same thing is
     * made into one before any of them is asked.
     *
     * It is also what a bounded string interpolation reaches. J46 §3.4 planned to emit that buffer
     * at the literal and could not: a hole's `formatBound(showBound(v))` is not a number until
     * `fold.cpp` has run, so `String.ofCapacity`'s argument is an expression in the resolver and a
     * constant here. This is the tier that has the number, so this is where the request is made.
     *
     * The count is read off the instruction rather than through `constantValueOf`, which puts every
     * answer through `foldableInt` and declines a target-answered width - and a capacity is a
     * `Size`, which is exactly one of those. What the width would be for is masking, and there is
     * nothing here to mask: the bound below is a number of *elements*, which no target makes
     * narrower.
     *
     * The element comes from the probe the request measures against - `runSlots`' first parameter,
     * whose whole purpose is that the element type reaches a function whose arguments would
     * otherwise not name one.
     */
    bool normalize(InstCall& call) {
        if(!request || call.through != mir::CallThrough::Declaration || !call.callee) return false;
        if(call.callee != request && opt.decl[call.callee]->specializes != request) return false;
        if(call.arguments.size() != 2 || !call.results.size()) return false;

        auto element = pointeeOf(opt, base()[call.arguments.get(base(), 0).value()]->occurrence.type);
        if(!element) return false;

        auto held = constantOf(opt, base()[call.arguments.get(base(), 1).value()]);
        if(!held || held->kind != mir::Constant::Int) return false;

        auto count = held->asInt();
        if(!count || count > mir::kMaxFixedArrayLength) return false;

        auto editor = opt.ir();
        auto pointer = base()[call.results.get(base(), 0)];

        editor.setInsertBefore(call);
        auto made = editor.addRunSlots(call.source, element, count);
        auto slots = editor.resultOf(*made);

        /*
         * The same bytes under the type the call answered. `runSlots` hands back `%a` and so does
         * this instruction, so the two usually agree - but a specialization's result carries its
         * own occurrence and convention, and this is where the two are made one value.
         */
        auto bytes = slots->occurrence.type == pointer->occurrence.type &&
                     slots->convention == pointer->convention
            ? slots
            : editor.resultOf(*editor.addUnary(call.source, mir::UnaryOp::Bitcast,
                                               editor.use(*slots, UseKind::Inspect),
                                               pointer->occurrence, pointer->convention));

        editor.replaceAllUses(*pointer, *bytes);
        editor.eraseInst(call);
        return true;
    }

    /*
     * The instructions that read what this allocation holds, and the ones that replace the whole of
     * it - the two events the liveness below is over.
     *
     * Collected over the allocation's **address tree** and not over its use list, for `reread`'s
     * reason: a body does not load a record out of a slot, it takes the address of the field it
     * wants and loads that.
     *
     * Everything that is not a whole-slot store is a read, which is the direction this may not be
     * wrong in: a write through a projection leaves the rest of the slot as the previous iteration
     * left it, so it ends nothing, and a `Load` counts even where what it produces is a borrow used
     * three blocks later - the load dominates every use of that borrow, so marking the load is
     * marking a point no later than the read it stands for.
     */
    void classifyStorage(Value& address, Value& allocation) {
        for(auto userPointer: address.uses(base())) {
            auto& user = *base()[userPointer];

            switch(user.kind) {
                case Inst::FieldAddress:
                case Inst::ElementAddress:
                case Inst::Unary:
                    for(Size i = 0; i < user.results.size(); i++) {
                        classifyStorage(*base()[user.results.get(base(), i)], allocation);
                    }

                    break;

                case Inst::Store:
                    if(base()[((InstStore&)user).address.value()] == &allocation) {
                        kills.set(user.id.index, true);
                        break;
                    }

                    reads.set(user.id.index, true);
                    break;

                default:
                    reads.set(user.id.index, true);
                    break;
            }
        }
    }

    /*
     * Whether what this allocation holds is still read when control comes back round to the header.
     *
     * A block answers for itself where it has an event: a read before a kill is live, a kill first
     * is dead. A block with neither is whatever its successors **inside the loop** say - an exit
     * successor contributes nothing, because the buffer is not written again once the loop is over
     * and one descriptor over bytes nobody rewrites is not the hazard.
     *
     * The fixpoint only ever turns a block on, so the walk is monotone and stops.
     */
    bool liveAtHeader(const Loop& loop) {
        auto count = U32(opt.body->blocks.size());
        live.reset(count);

        auto again = true;
        while(again) {
            again = false;

            for(U32 i = 0; i < count; i++) {
                if(!loop.contains[i] || live[i]) continue;

                auto block = opt.body->block(base(), BlockId(i));
                if(!block || block->deleted) continue;

                auto answer = false;
                auto settled = false;

                for(auto pointer: block->instructions(base())) {
                    auto id = base()[pointer]->id.index;
                    if(reads[id]) { answer = true; settled = true; break; }
                    if(kills[id]) { settled = true; break; }
                }

                if(!settled) {
                    forEachSuccessor(base(), *block, [&](U32 target) {
                        if(loop.contains[target] && live[target]) answer = true;
                    });
                }

                if(!answer) continue;

                live.set(i, true);
                again = true;
            }
        }

        return live[loop.header];
    }

    // Whether a value carrying the buffer travels one of this loop's back edges. A block argument at
    // the header holds the *previous* iteration's value at the top of the next one, which is the
    // same two-live-descriptors hazard said about a value instead of about a slot.
    bool carriedBack(const Loop& loop) {
        auto count = U32(opt.body->blocks.size());
        auto carried = false;

        for(U32 i = 0; i < count && !carried; i++) {
            if(!loop.contains[i]) continue;

            auto block = opt.body->block(base(), BlockId(i));
            if(!block || block->deleted) continue;

            auto last = block->terminator(base());
            if(!last) continue;

            forEachEdge(base(), *last, [&](Edge& edge) {
                if(!edge.target || base()[edge.target]->index.index != loop.header) return;

                for(auto used: edge.arguments.contents(base())) {
                    if(used.isSet() && carries[base()[used.value()]->id.index]) carried = true;
                }
            });
        }

        return carried;
    }

    /*
     * Whether at most one lifetime of these bytes is ever live at a time - the loop rule, argued in
     * the file comment, and the relaxation J46 step 5 is.
     *
     * A request outside every loop runs at most once per activation and the question does not
     * arise. A request **inside** one runs per iteration over a slot that is per activation, so what
     * has to hold is that the previous iteration's descriptor is gone before the next one is built:
     * nothing the walk tainted may travel the back edge as a value, and nothing it wrote into frame
     * storage may still be read when control arrives at the header.
     *
     * `test/bench/findings.md` §91.5 is what says the rule is load-bearing rather than cautious -
     * the shipping stage has no rule here and miscompiles `prev = cur` in a loop for it, reading the
     * new iteration's array back through the old descriptor. That program fails exactly one of the
     * two conditions: `prev`'s slot is read at the top of an iteration before anything stores to it,
     * so it is live at the header. `for x in xs: let &row = [1, 2, 3]` passes both - the row's slot
     * is stored before it is read and its drop runs before the back edge - and is the shape this is
     * for.
     *
     * Asked of every loop containing the request rather than of the innermost one, because an outer
     * iteration re-runs the inner loop and a read out in the outer body is one the inner walk never
     * looks at.
     */
    bool oneAtATime(Inst& request) {
        auto& loops = loopsOf(opt);
        auto block = base()[request.block]->index.index;

        for(auto& loop: loops) {
            if(!loop.contains[block]) continue;
            if(carriedBack(loop)) return false;

            for(auto pointer: holders) {
                reads.reset(U32(opt.body->insts.size()));
                kills.reset(U32(opt.body->insts.size()));

                auto allocation = base()[pointer];
                classifyStorage(*allocation, *allocation);

                if(liveAtHeader(loop)) return false;
            }
        }

        return true;
    }

    /*
     * The one descriptor the buffer reached, or null.
     *
     * Strict on both sides: none means the pass could not find the flag it would have to rewrite,
     * and two means the buffer is shared by descriptors that need not agree. Either way the answer
     * is to leave the request alone - a placement whose flag was not turned off is a frame address
     * handed to `freeHeap`.
     */
    Inst* soleDescriptor() {
        if(descriptors.size() != 1) return nullptr;

        auto& made = *base()[descriptors[0]];
        auto flag = base()[((InstAggregate&)made).components.get(base(), kOwnsHeap).value()];

        auto held = constantOf(opt, flag);
        if(!held || held->kind != mir::Constant::Int) return nullptr;

        return held->asInt() == kRunFromHeap ? &made : nullptr;
    }

    /*
     * The rewrite: the request becomes frame storage, and the descriptor over it stops claiming the
     * allocator's.
     *
     * **Two edits and one of them is a bit**, which is what the instruction bought. This used to
     * replace the `runSlots` call with an `Alloc` of `[a *n]` and a bitcast back to `%a` - three
     * instructions for one, because a call's result type is the pointer and an allocation's is the
     * pointer to the whole array. `InstRunSlots` already answers `%a` and already knows the element
     * and the count, so the placement is the answer to a question the instruction was asking, and
     * setting it is the whole of the storage half.
     *
     * The descriptor is still rebuilt rather than edited, because an instruction's operands are half
     * of a two-sided statement and `Editor` is the only thing allowed to write either half - so the
     * way to change one is to make the instruction that has it and retire the one that does not.
     *
     * The two halves are one rewrite and must stay that way: a placement whose flag was not turned
     * off is a frame address handed to `freeHeap`.
     */
    void place(InstRunSlots& slots, Inst& made) {
        auto editor = opt.ir();
        slots.placed = true;

        auto& aggregate = (InstAggregate&)made;
        auto result = base()[made.results.get(base(), 0)];

        mir::Constant borrowed;
        borrowed.type = base()[aggregate.components.get(base(), kOwnsHeap).value()]->occurrence.type;
        borrowed.kind = mir::Constant::Int;
        borrowed.bits = kRunBorrowed;

        editor.setInsertBefore(made);
        auto flag = editor.resultOf(*editor.addConst(made.source, borrowed));

        Use components[kFieldCount];
        for(Size i = 0; i < kFieldCount; i++) components[i] = aggregate.components.get(base(), i);
        components[kOwnsHeap] = editor.use(*flag, UseKind::Consume);

        auto placed = editor.addAggregate(made.source, aggregate.constructor, components,
                                          kFieldCount, result->occurrence, result->convention);

        editor.replaceAllUses(*result, *editor.resultOf(*placed));
        editor.eraseInst(made);

        discharge(editor);
    }

    /*
     * The refusal tests, answered - the third part of the rewrite.
     *
     * `false` for an `Eq` against zero and `true` for a `Ne`, which is the same statement made from
     * the two directions the language spells it in: this address is not null. What removes the
     * branch and the abort block behind it is the ordinary branch folding that follows, which is why
     * this only has to answer the comparison.
     */
    void discharge(Editor& editor) {
        for(auto pointer: refusals) {
            auto& test = (InstBinary&)*base()[pointer];
            auto result = base()[test.results.get(base(), 0)];

            editor.setInsertBefore(test);
            auto answer = editor.addConst(
                test.source, mir::truthConstant(opt.program, test.op != mir::BinaryOp::Eq));

            editor.replaceAllUses(*result, *editor.resultOf(*answer));
            editor.eraseInst(test);
        }
    }

    // One request, from the shape test to the rewrite. False where nothing was changed, which is
    // every request whose bytes this frame cannot keep.
    bool consider(InstRunSlots& slots) {
        if(slots.placed || !slots.results.size() || !slots.element) return false;
        if(!sized(slots)) return false;

        if(!inFrame(*base()[slots.results.get(base(), 0)])) return false;

        // After the walk and not before it, which is the difference J46 step 5 made: the loop rule
        // is now about what the walk found rather than about where the request stands.
        if(!oneAtATime(slots)) return false;

        auto made = soleDescriptor();
        if(!made) return false;

        place(slots, *made);
        return true;
    }
};

} // namespace

void placeRuns(OptContext& opt) {
    RunPlacement placement { opt };
    if(!placement.descriptor) return;

    /*
     * A pass over the blocks, restarted at the body whenever one request is placed.
     *
     * Restarted rather than continued, because the rewrite retires the descriptor and adds two
     * instructions in its place - so the walk it was in the middle of is over a list that has
     * moved. There are a handful of requests in a body that has any at all, which makes the restart
     * cheaper than the cursor repair, and the driver's own rounds are what settle what the
     * placement exposed.
     */
    auto again = true;
    while(again) {
        again = false;

        for(auto blockPointer: opt.body->blocks.contents(opt.base)) {
            auto block = opt.base[blockPointer];
            if(block->deleted) continue;

            for(auto instPointer: block->instructions(opt.base)) {
                auto& inst = *opt.base[instPointer];

                // The library's own request first, made into the instruction - see `normalize`.
                if(inst.kind == Inst::Call) {
                    if(!placement.normalize((InstCall&)inst)) continue;

                    again = true;
                    break;
                }

                if(inst.kind != Inst::RunSlots) continue;
                if(!placement.consider((InstRunSlots&)inst)) continue;

                again = true;
                break;
            }

            if(again) break;
        }
    }
}

} // namespace ssa
