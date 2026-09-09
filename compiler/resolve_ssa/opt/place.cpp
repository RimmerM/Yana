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
 *  - At a call argument, `Summary::retains` for a value and `Summary::escapes` for a raw pointer -
 *    the two rows the summary's own note keeps apart, and the pointer row is the one that follows
 *    "the incoming pointer of a `%T` parameter", which is exactly what the buffer is at such a
 *    position.
 *  - A comparison is a sink and not a carrier: `runOver` tests the pointer for null and a `Bool`
 *    is not a way back to anything.
 *  - `Return` is an escape, and so is every kind not named - a `FunValue` environment, a `Native`
 *    operand, an `Atomic`. Forgetting too much costs a placement and forgetting too little is a use
 *    after free, so the default declines.
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

    // The same allocations as a list, because the loop rule walks them and a set answers only
    // membership. Filled beside `within` and cleared with it.
    Array<SsaPtr<Value>> holders;

    Array<SsaPtr<Value>> pending;

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

    void taint(Value* value) {
        if(!value || carries[value->id.index]) return;

        carries.set(value->id.index, true);
        pending.push(value - base());
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
            case Inst::Aggregate:
                if(isDescriptor(user, value)) descriptors.push(&user - base());
                for(Size i = 0; i < user.results.size(); i++) {
                    taint(base()[user.results.get(base(), i)]);
                }

                return true;

            /*
             * A projection, a choice between two values and a restatement of one - each keeps the
             * address exactly as far as its own result does, which is the recursion this walk is.
             */
            case Inst::FieldAddress:
            case Inst::ElementAddress:
            case Inst::Select:
            case Inst::Unary:
                for(Size i = 0; i < user.results.size(); i++) {
                    taint(base()[user.results.get(base(), i)]);
                }

                return true;

            /*
             * A comparison, which is where `runOver` asks whether the allocator refused. What comes
             * out is a `Bool` and a `Bool` is not a way back, so this is a sink rather than a step -
             * and it is the one arithmetic use admitted, because pointer arithmetic that is not a
             * comparison produces another address and is caught by the projection arm above.
             */
            case Inst::Binary: {
                auto& binary = (InstBinary&)user;
                auto category = mir::binaryCategory(binary.op);
                if(category != mir::OpCategory::Equality && category != mir::OpCategory::Order) {
                    for(Size i = 0; i < user.results.size(); i++) {
                        taint(base()[user.results.get(base(), i)]);
                    }

                    return true;
                }

                // An equality against zero is the refusal test - see `refusals`. Order comparisons
                // and equalities against anything else are sinks like any other and say nothing.
                if(category == mir::OpCategory::Equality && isRefusalTest(binary, value)) {
                    refusals.push(&user - base());
                }

                return true;
            }

            /*
             * Into storage. The address has to root at an allocation of this body that nothing else
             * can name; then the allocation carries the pointer and every load out of it does too.
             *
             * A store *through* the tainted address is the other direction and says nothing: it is
             * a write into the buffer, which is what the buffer is for.
             */
            case Inst::Store: {
                auto& store = (InstStore&)user;
                if(!named(store.value)) return true;

                auto root = allocationRoot(opt, base()[store.address.value()]);
                if(!root || !containedOf(opt)[root]) return false;

                if(!within[root->id.index]) {
                    within.set(root->id.index, true);
                    holders.push(root - base());
                    reread(*root);
                }

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

        auto kept = opt.decl[call.callee]->summary.escapes().get(mir::ArgumentSet::all());

        Size index = 0;
        for(auto used: call.arguments.contents(base())) {
            if(used.isSet() && base()[used.value()] == &value && kept.has(index)) return false;
            index++;
        }

        /*
         * What the call answers may have been computed from what it was handed - a `bitcast` of the
         * pointer is a call at this tier, and so is any accessor handing back a run pointer. So
         * every result carries, which costs nothing where it does not: a result nothing does
         * anything unsafe with is a value this walk clears.
         */
        for(Size i = 0; i < call.results.size(); i++) taint(base()[call.results.get(base(), i)]);
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
                    else taint(base()[target->argumentAt(base(), position)]);
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
    void reread(Value& address) {
        for(auto userPointer: address.uses(base())) {
            auto& user = *base()[userPointer];

            switch(user.kind) {
                case Inst::Load:
                case Inst::Copy:
                    for(Size i = 0; i < user.results.size(); i++) {
                        taint(base()[user.results.get(base(), i)]);
                    }

                    // And the borrow a `Guaranteed` load is, which names the same storage - the half
                    // `Contained` recurses through for the same reason.
                    for(Size i = 0; i < user.results.size(); i++) {
                        auto result = base()[user.results.get(base(), i)];
                        if(result->convention == Convention::Guaranteed) reread(*result);
                    }

                    break;

                case Inst::FieldAddress:
                case Inst::ElementAddress:
                case Inst::Unary:
                    for(Size i = 0; i < user.results.size(); i++) {
                        reread(*base()[user.results.get(base(), i)]);
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
        pending.clear();

        taint(&pointer);

        while(pending.size()) {
            auto value = base()[pending.pop().unwrap()];

            for(auto userPointer: value->uses(base())) {
                if(!admits(*base()[userPointer], *value)) return false;
            }
        }

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

        descriptors.clear();
        refusals.clear();
        holders.clear();
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
