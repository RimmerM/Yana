#include "resolve_internal.h"
#include "own.h"
#include "specialize.h"
#include "select.h"

namespace mir {

/*
 * Iterator objects - the **state record**, `doc/inflight/Design-Iterator-Objects.md` §3.
 *
 * An `iter fn` whose call result is kept is not called at all. What the call site builds is a record
 * holding a resume index, the hand-over slot, the captured arguments and whatever the producer keeps
 * live across a `yield`; what steps it is one function that switches on the index, runs to the next
 * hand-over and returns. There is no stack, no allocation and no handle: `-> iter(T)` used to
 * declare one and is now an ordinary result *type* (§2.2), whose value is this same record erased -
 * `erasedIterTypeOf` and `adaptIterValue` are below, and the stack form is gone (2026-09-16).
 *
 * Several comments below compare a field or a loan against what the **stack handle** did with it -
 * `iterConstructor`, `constructIter`, `iterEntry`, `resolveExternalFor`, `LentIter.next` and
 * `LentIter.current`. Those names no longer resolve anywhere in the tree: they were
 * `resolve_mir/resolve_iter.cpp` and `lib/Fiber/Iterator.linux.x64.yana`, both deleted with the
 * handle, and `../../doc/progress/2026-09-16-J55-iterator-objects.md` is where the comparison can
 * still be read.
 *
 * Three sentences, and the third is where the work is:
 *
 *  - **the site type** `f.Iter` is a record minted here, one per (producer, substitution), by the one
 *    function `f.Iter` in type position calls too (`iterSiteTypeOf`, reached from `resolve_sig.cpp`
 *    since 2026-09-16), which is what makes a field written `lexer: lex.Iter` and a
 *    `let it = lex(src)` beside it one record;
 *  - **a call** does not call: it builds the record with the index at 0 and the captured arguments
 *    filled, which is `f$new` - a signature here and a body the conversion emits, because "a record
 *    whose payload is untouched" is a shape MIR has no write for;
 *  - **the step** `f$step` is the producer's **own MIR body** converted a second time, in
 *    `resolve_ssa/convert.cpp`. Nothing is re-resolved: the producer's blocks, its drop points and
 *    its loans are read exactly as the `for` form reads them, and three things are read differently -
 *    a parameter's storage is a field of `self`, a `yield` is "write the slot, write the index,
 *    return", and the value that `yield` produced is the step's own `$resume` argument.
 *
 * § A `lens fn` mints one too
 *
 * `f.Lens`, since 2026-09-16 (§6.6). A region a producer runs over part of its own body is a payload
 * member whatever its multiplicity - `for x in g(...)` holds `g.Iter` and `withLock(m):` holds
 * `withLock.Lens` - because a `yield` written inside either suspends the outer object with the callee
 * suspended inside it. Everything below reads the same three things of a lens that it reads of an
 * iterator (`Function::handover`, `continuationArg`, and the body those two are about), so this is a
 * row rather than a path; what differs is the record's name, the `Iterate` instance a lens does not
 * get, and the teardown a lens member always makes the outer record owe.
 *
 * A lens's record is minted only from `collectIterNests`. `.Iter` in type position is an `iter fn`'s
 * (`resolve_sig.cpp`), and no consumer holds a lens object: what steps one is the producer whose step
 * embeds the region.
 *
 * § The step's argument is the continuation's answer, unchanged
 *
 * J49 made an iterator's continuation answer the exit signal, or `Outcome(R, exit)` where the
 * declaration wrote `<- R`. **That value is what a step is handed**, at the same type, and it is what
 * lets one function serve three callers: `next()` passes "fall out", `resume(v)` passes `Proceed(v)`,
 * and the **teardown** passes "leave". So the producer's own departure path - `resolveYield`'s test,
 * `emitDeparture`'s walk out of the loops, the statements below them, and drop elaboration releasing
 * every local that is still live - is the teardown, and §3.3's "shared across indices" is not
 * something built here but what converting one body once already does.
 *
 * `$cancel` is the one thing the answer cannot carry: a record at index 0 has run *none* of the
 * producer's body (§2.6), so entering it to cancel must release the captured arguments and stop
 * rather than start. It is a parameter rather than a reserved index because the index says where the
 * producer is and this says what is being asked of it.
 *
 * § What a record owes
 *
 * Nothing, where every member is trivially copyable - which is the common producer, and is what makes
 * `copy(it)` two independent streams for free. Where a member is not, the record gets a compiler
 * generated `instance Drop(f.Iter)` whose member steps it with `$cancel`, and the teardown is
 * therefore keyed on the index rather than derived field-wise: a derived walk would release payload
 * members that were never written.
 *
 * A record that owes something is **not** `Copy` here. §3.3's "derived when every field is copyable"
 * is the trivial case only for now, because a derived `Copy` has the same problem the derived
 * teardown has and needs the same index-keyed shape (T3b).
 */

/*
 * The resume index's two fixed values.
 *
 * `0` is a record that has not started and is what `f$new` writes; `kIterFinished` is one that ran
 * out or was cancelled, and is deliberately a number no `yield` can be given - a `yield`'s index is
 * `1 + k`, so what is left over is the one value the dispatch's fallback means.
 */
static constexpr U64 kIterFinished = ~U64(0) & 0x7fffffff;

/*
 * "Give me the next element", as the exit signal spells it - `resolve_ssa/convert.cpp`'s
 * `kExitFallOut`, which is where the same number is written for the continuation-passing form.
 *
 * Repeated rather than shared because the two tiers do not include each other. The other row a step
 * is ever handed - "leave" - is written only where `f$drop`'s body is built, which is there.
 */
static constexpr I64 kIterFallOut = 0;

/*
 * Which of the two things a payload member holds - `RecordType::payloadLocals`, whose entries are a
 * local index or a **node** index with this bit set.
 *
 * One list rather than two, because what reads it is the step's storage walk and the question it
 * asks is the same for both: which field is this thing in. The two differ only in what the *other*
 * end is - a local has a place and a value has an SSA result - and that is a switch on one bit
 * rather than a second list that has to be kept parallel.
 */
static constexpr U32 kIterValueMember = 0x80000000;

/*
 * And whether the member is **placed** - `@heap` on the producer's binding, `RecordType::kHeapMember`.
 *
 * The field then holds the run's address rather than the value, which is the whole of §6.1's second
 * fix: a record moves with whatever holds it, so a borrow into one of its own members is a borrow of
 * where the value used to be - and a member in a run of its own does not move, so the borrow stays
 * good. `f$new` allocates the run, the step reaches the value through the pointer, and the record's
 * teardown frees it.
 */
static constexpr U32 kIterHeapMember = RecordType::kHeapMember;

/*
 * Whether this producer's `yield` **lends** what it hands over, which decides what the slot holds.
 *
 * The question the stack handle's own `yieldLends` asked, and for the same reason: a trivially
 * copyable value is copied into the slot, which no program can tell from the
 * loan, and anything else under a borrowing convention has to be reached where it is. A lending slot
 * therefore holds the value's **address**, valid for exactly as long as the record stays suspended -
 * which is the whole of §3.3's stepping rule, enforced on the consumer's side by the loan the read
 * of the slot takes against the record.
 */
static bool objectLends(DeclBase base, const Occurrence& handover) {
    if(handover.contract.isOwned()) return false;

    /*
     * A **window** is already a value, and that is the one row this question has to answer before it
     * looks at the type at all.
     *
     * `-> [a]` hands over two words naming somebody else's storage, exactly as a captured window
     * does (`IterCaptureKind::View`), so the slot holds the descriptor. Lending it would store the
     * *lender's* address - and for a producer that hands over a window it captured, that address is
     * the address of a descriptor field, which the consumer then reads as an owner.
     * `RecordType::slotWindow` is what the readers of the slot ask.
     */
    if(handover.contract.form() == AccessForm::View) return false;

    /*
     * And a hand-over with **interior positions** lends whatever its type is.
     *
     * `iter fn pairs(self: 'Map(k, v)) -> Entry('k, 'v)` is the shape: the occurrence carries a
     * shape the type does not have, so `manifestNative` lays the record's fields out as *references*
     * and the value is two addresses into the map rather than two words of its own. A slot holding
     * it by value would be laid out from the bare type - the same record with its fields inline -
     * and the store either side of the hand-over would be a store of addresses into space for
     * values. So what the slot holds is where the value is, which is what a borrowed interior asks
     * for anyway.
     */
    if(!Shapes::isEmpty(handover.shape)) return true;

    return !isTriviallyCopyable(base, handover.type);
}

/*
 * The root local a place is reached through, or `kNoLocal` - a global, a raw address, a loan.
 *
 * One walk up the parent chain, which is what a `Place` is: interned, so the chain is the path and
 * the root is the end of it.
 */
static constexpr U32 kNoLocal = ~U32(0);

static U32 rootLocalOf(MirBase mir, Body& body, PlaceId id) {
    auto place = body.place(mir, id);
    while(place->parent) place = mir[place->parent];
    return place->kind == Place::Local ? place->payload : kNoLocal;
}

/*
 * Whether one node **does** something a program can tell happened.
 *
 * Asked only of the departure walk below, and the answer is deliberately coarse in the direction
 * that adds a teardown: a call may print, a write through a borrow or into a global is the
 * producer reaching outside its own frame, and the machine operations are what they are. Everything
 * else here computes - a read, an arithmetic node, a write into the producer's own local - and a
 * record whose cleanup only computes is one nothing can tell was cancelled.
 *
 * A `yield` is **not** one, and that is what keeps `copy(it)` on the ordinary producer. A departed
 * pass does not enter the continuation again - `guardYields` is the test that stops it, and where
 * there is no test there is no second hand-over to reach - so a straight-line `yield 1; yield 2` has
 * nothing on its departure path and owes nothing, which is what `iter/Object.yana` is explicit about
 * wanting.
 */
static bool nodeHasEffect(MirBase mir, Body& body, Node& node, const U32* isYield) {
    switch(node.kind) {
        case Node::Call:
            return isYield[node.id.index] == kNoLocal;

        // A region the producer opens on its way out - a lens's acquisition, or a nested traversal.
        // Both run somebody else's body, which is the whole of what a call is here.
        case Node::LensCall:
        case Node::LensTry:
        case Node::Native:
        case Node::Atomic:
        case Node::RunSlots:
            return true;

        // And a host operation, which is the one kind here whose answer is a *row* rather than the
        // kind - `host.def`'s effect column, Analysis-IR-JS.md §3.10. `hostFixedCapacity` answers a
        // question about a type and is the only row that has no effect; everything else reads or
        // changes host state, which is something a consumer can observe.
        case Node::Host:
            return !hostIsPure(((NodeHost&)node).op);

        /*
         * A write, where what it writes is not the producer's own storage.
         *
         * A global is the plain case; a projection of a **borrowed** parameter is the same statement
         * about the caller's storage - `c.at = c.at + 1` at `&c: Counter` is something the consumer
         * can read afterwards. A write into an owned local is not: the local is a payload member of
         * a record that is being abandoned, so nothing survives to be read.
         */
        case Node::Write: {
            auto& destination = ((NodeWrite&)node).destination;
            if(destination.source() != SourceKind::Place) return true;

            auto place = body.place(mir, destination.placeId());
            while(place->parent) place = mir[place->parent];

            if(place->kind != Place::Local) return true;

            auto& local = *body.local(mir, LocalId(place->payload));
            return !local.occurrence.contract.isOwned();
        }

        default:
            return false;
    }
}

/*
 * Whether a **departure** does anything - J56 §4's hole, closed 2026-09-16.
 *
 * A consumer that leaves ends the iterator's loops and runs whatever is written below them, and the
 * internal form gets that for nothing. An object abandoned at the same point runs it only if the
 * record has a teardown, and a record owes one when its *members* do - so
 *
 *     iter fn noisy(n: I32) -> I32:
 *         let &i = 0 :: I32
 *         while i < n: { yield i; i = i + 1 }
 *         trace = trace * 10 + 9
 *
 * printed nothing when its object was dropped mid-iteration and printed on a `break` in the internal
 * form. The two forms have to be observationally identical, so the walk is the rule: forward from
 * each departure's own block (`Body::departures`), and a record owes a teardown where anything with
 * an effect is reachable.
 *
 * **Forward reachability and not the departure edges only**, which over-approximates where a
 * departure inside a nested region rejoins a block the ordinary path also reaches. That is the safe
 * direction - an extra teardown runs a cancel step that does nothing - and the alternative is
 * re-deriving `resumeDeparture`'s test out of the CFG, which is the emission written twice.
 *
 * The crude rule J56 declined is "any statement below a `yield`", and the difference between the two
 * is the whole point: a departure leaves the *loops*, so `i = i + 1` below the hand-over is not on
 * this path at all, and the producers `iter/Object.yana` copies stay copyable.
 */
static bool departureHasEffect(MirBase mir, Body& body, const U32* isYield) {
    auto entries = body.departures.contents(mir);
    if(!entries.size()) return false;

    Array<U8> seen;
    for(U32 i = 0; i < body.blocks.size(); i++) seen.push(0);

    Array<U32> stack;
    for(auto index: entries) {
        if(index < seen.size() && !seen[index]) { seen[index] = 1; stack.push(index); }
    }

    while(stack.size()) {
        auto index = stack[stack.size() - 1];
        stack.pop();

        auto block = body.block(mir, BlockId(index));
        auto count = block->nodes.size();

        for(Size i = 0; i < count; i++) {
            if(nodeHasEffect(mir, body, *mir[block->nodes.get(mir, i)], isYield)) return true;
        }

        if(!count) continue;

        forEachEdge(mir, *mir[block->nodes.get(mir, count - 1)], [&](Edge& edge) {
            if(!edge.target) return;
            auto target = mir[edge.target]->index.index;
            if(target >= seen.size() || seen[target]) return;
            seen[target] = 1;
            stack.push(target);
        });
    }

    return false;
}

/*
 * What a producer's body keeps alive across its hand-overs - §3.2's second step.
 *
 * The ordinary backward fixpoint, over **locals** rather than over move paths because the record has
 * a member per local: a partial move out of a payload member is still that member's storage, so
 * splitting the answer finer would produce a set this stage has nowhere to put. `analyze_live.cpp` is
 * the path-level walk and runs later, over an analysis this one cannot use - the payload is decided
 * while bodies are still being resolved, because the *type* is what a consumer's `let` binds.
 *
 * And over **node results** beside them, which is not the same question asked twice - see `localBit`
 * and `valueBit` below, where the counted `for`'s own block arguments are why there are any.
 *
 * CFG successors **and the `Continuation::Many` back edge**, which is what a nested traversal made
 * necessary: a `for x in g(...)` inside the producer is a region whose blocks fall out to say "give
 * me the next element", and the loop is a fact about the region rather than an edge (`Continuation`,
 * `Analysis::forEachSuccessor`). Without it a local written in a nested pass and read on the next one
 * would be dead across the `yield` that suspends the outer object inside that pass.
 *
 * What it answers is the live set **after** each `yield`, which is the set the record has to carry:
 * a value the producer reads below its own hand-over is a value the consumer's step must find again.
 *
 * And, out of the same `entryLive`, which members may **share** a field - `interference`, §3.2's
 * third step. That question is over the whole body rather than over the hand-overs, which is why it
 * is a second walk over this fixpoint's answer rather than a second thing this loop accumulates.
 */
struct LiveAcross {
    MirBase mir;
    Body& body;
    U32 locals;
    U32 values;
    U32 bits;
    U32 words;

    // One row per block: what is live on entry to it. The fixpoint's state, and its answer.
    Array<U64> entryLive;

    // The union over every `yield` of what is live after it - the payload, before the parameters and
    // the continuation are taken out of it.
    Array<U64> across;

    /*
     * The locals that **owe a teardown**, live at every exit whether or not anything reads them.
     *
     * The one thing a walk over reads and writes gets wrong about a producer, and it is not a
     * refinement: `let a = Note {...}` before a `yield` and nothing below it is dead by this walk's
     * reckoning and *alive* by the ownership analysis', which places its drop where the body ends.
     * A step-frame slot for it would be released on a later step than the one that filled it.
     *
     * Where the drop actually goes is `analyze_drop.cpp`'s answer and does not exist yet - the
     * payload decides a *type*, and a type is what a consumer's `let` binds long before any body is
     * analysed. So the conservative seed is the rule: an owner with something to run is live until
     * the producer returns, which is the latest the analysis can place it.
     */
    Array<U64> owing;

    // And the same per `yield`, which is what the inspection output prints - §7 stage 3's item 5.
    Array<U32> yields;
    Array<U64> perYield;

    /*
     * The local a **lending** hand-over names, per node, or `kNoLocal`.
     *
     * A lending slot holds the value's *address*, and an address is only worth holding while the
     * storage it names outlives the step that wrote it. A local the producer reads below its own
     * hand-over is a payload member by the fixpoint's ordinary answer; one it does not - the
     * temporary a `yield Entry {...}` is held in - is dead the moment the hand-over is made and its
     * step-frame slot is gone before the consumer reads the slot. So the value a lending `yield`
     * names is live *at* that hand-over whether or not anything below reads it, which puts it in the
     * record and makes the address one into storage the consumer holds.
     *
     * Empty where the hand-over does not lend, because then the slot holds the value itself and
     * there is nothing for the producer to keep.
     */
    Array<U32> lentAt;

    /*
     * One bit per local **and** one per node result, in one fixpoint.
     *
     * Both, and that is not a convenience: a producer's state is not only its named storage. A
     * counted `for` hands its bound and its step along its own back edge as block arguments, so the
     * `yield` inside the loop has *values* live across it that no local names - and a split that
     * left them where they were would produce a resume block naming a value nothing on the way in
     * defines. They are spilled into the record beside the locals, which is the same answer for the
     * same reason and is what a hand-written state machine does with them.
     *
     * A value's uses are node operands **and edge arguments**; `checkValueScope` holds the first to
     * one block and says nothing about the second, which is exactly why a value can cross at all.
     */
    U32 localBit(U32 index) const { return index; }
    U32 valueBit(U32 index) const { return locals + index; }

    LiveAcross(MirBase mir, Body& body):
        mir(mir), body(body), locals(U32(body.locals.size())), values(U32(body.nodes.size())),
        bits(locals + values), words((locals + values + 63) / 64) {}

    static bool get(const U64* row, U32 bit) { return (row[bit / 64] >> (bit % 64)) & 1; }
    static void set(U64* row, U32 bit) { row[bit / 64] |= U64(1) << (bit % 64); }
    static void clear(U64* row, U32 bit) { row[bit / 64] &= ~(U64(1) << (bit % 64)); }

    bool liveLocal(U32 index) const { return get(across.pointer(), localBit(index)); }
    bool liveValue(U32 index) const { return get(across.pointer(), valueBit(index)); }

    /*
     * One node, backwards - `Analysis::applyBackward`'s order, kept because the reason for it is the
     * same one: a write whose value came out of the place it writes reads the old contents and then
     * replaces them, so the kill has to happen before the uses are marked.
     *
     * A kill only where the write names the local **whole**. A write into a field says nothing about
     * the record, exactly as the path walk says, and the coarse answer here is the conservative one.
     */
    void applyBackward(U64* live, Node& node) {
        clear(live, valueBit(node.id.index));

        forEachOperand(mir, node, [&](Operand& operand, OperandRole role) {
            if(role != OperandRole::WriteTarget) return;
            if(operand.source() != SourceKind::Place) return;

            auto place = body.place(mir, operand.placeId());
            if(place->kind != Place::Local) return;

            clear(live, localBit(place->payload));
        });

        auto use = [&](Operand& operand) {
            if(operand.source() == SourceKind::Result) {
                set(live, valueBit(operand.nodeId().index));
                return;
            }

            if(operand.source() != SourceKind::Place) return;

            auto root = rootLocalOf(mir, body, operand.placeId());
            if(root != kNoLocal) set(live, localBit(root));

            // A place is reached *through* the values its projections name - an index, and the raw
            // address a pointer place is rooted in. Both are node results and both are uses.
            auto place = body.place(mir, operand.placeId());
            for(auto step = place; step; step = step->parent ? mir[step->parent] : nullptr) {
                if(step->kind == Place::Index || step->kind == Place::Address) {
                    set(live, valueBit(step->payload));
                }
            }
        };

        forEachOperand(mir, node, [&](Operand& operand, OperandRole role) {
            /*
             * A write of the **whole** local is a definition and not a use of what was there.
             *
             * The kill above already said so, and walking every operand as a use afterwards put it
             * straight back - which made every local live from the block that first writes it, and
             * therefore made `let &b = 0` below a `yield` a member kept across that hand-over. It
             * cost nothing while the record was the sum of what survives a hand-over and costs the
             * whole of §3.2's third step now: two locals in two arms of a producer never share a
             * field if each is live wherever the other is written.
             *
             * A write into a **field** is a use, which is the same sentence the kill makes from the
             * other side: what the record holds elsewhere survives it, so the root stays live.
             */
            if(role == OperandRole::WriteTarget && operand.source() == SourceKind::Place &&
               body.place(mir, operand.placeId())->kind == Place::Local) {
                return;
            }

            use(operand);
        });
        forEachEdge(mir, node, [&](Edge& edge) {
            auto items = edge.arguments.contents(mir);
            for(Size i = 0; i < items.size(); i++) use(*items.pointerAt(i));
        });
    }

    /*
     * What is live on the way **out** of a block - its successors' entry sets, the exit seed, and the
     * back edge a `Many` region has that its CFG does not.
     *
     * One function for the two walks below, because the two have to agree: an edge the fixpoint
     * follows and the interference replay does not would colour two members disjoint that a nested
     * pass keeps alive together. `Analysis::forEachSuccessor` is the same statement one stage over,
     * and the *skip* edge it draws beside this one is left out here because a nested traversal that
     * can fail is refused at the mint.
     */
    void meetSuccessors(U32 index, U64* live) {
        auto block = body.block(mir, BlockId(index));
        auto count = block->nodes.size();
        if(!count) return;

        auto& last = *mir[block->nodes.get(mir, count - 1)];

        forEachEdge(mir, last, [&](Edge& edge) {
            if(!edge.target) return;
            auto target = mir[edge.target]->index.index;
            for(U32 w = 0; w < words; w++) live[w] |= entryLive[Size(target) * words + w];
        });

        // The exit, where every owner that owes something is still live - see `owing`.
        if(last.kind == Node::Return) {
            for(U32 w = 0; w < words; w++) live[w] |= owing[w];
        }

        /*
         * And the pass after this one, for a block that falls out of a nested traversal.
         *
         * Only a `Jump`: a `break` leaves the same block for the same target and does not come back,
         * which is the whole of what the kind is for - the same reading `forEachSuccessor` makes.
         */
        if(last.kind != Node::Jump) return;

        auto region = body.region(mir, block->region);
        if(!region || region->multiplicity != Continuation::Many) return;

        auto leaves = false;
        forEachEdge(mir, last, [&](Edge& edge) {
            if(edge.target && mir[edge.target]->region != block->region) leaves = true;
        });

        if(!leaves) return;

        auto entry = body.block(mir, region->entry)->index.index;
        for(U32 w = 0; w < words; w++) live[w] |= entryLive[Size(entry) * words + w];
    }

    /*
     * Which pairs of the record's candidate members are live at one point at once - §3.2's third
     * step, the spill-slot interference problem.
     *
     * Over the **whole body** and not only at the hand-overs, which is the one thing §3.2's sentence
     * does not say and the thing that decides whether the answer is sound: a payload member is the
     * local's storage for its whole life, not a spill written at a `yield`, so two members share a
     * field only where neither is so much as *written* while the other is live. "Disjoint at every
     * `yield`" would let `let a = 1` in one arm clobber a `b` the other arm is still reading.
     *
     * A second walk rather than a second fixpoint: `entryLive` is already the answer per block, so
     * this replays each block backwards from it and records what is live together at every point.
     * The definition set is recorded beside the live one, because a write into a shared field is a
     * clobber whether or not anything reads what it wrote.
     *
     * `bits` is the candidates' bit numbers and `into` a `count` by `count` matrix, which is a byte
     * per pair over a handful of members - the payload of the largest producer in the corpus is six.
     */
    void interference(const U32* isYield, const U32* bits, U32 count, U8* into) {
        SmallArray<U64, 8> live;
        SmallArray<U64, 8> defs;
        for(U32 i = 0; i < words; i++) { live.push(0); defs.push(0); }

        auto meet = [&](const U64* row, const U64* extra) {
            for(U32 a = 0; a < count; a++) {
                if(!get(row, bits[a]) && !(extra && get(extra, bits[a]))) continue;

                for(U32 b = 0; b < count; b++) {
                    if(b == a) continue;
                    if(!get(row, bits[b]) && !(extra && get(extra, bits[b]))) continue;

                    into[Size(a) * count + b] = 1;
                    into[Size(b) * count + a] = 1;
                }
            }
        };

        for(U32 index = 0; index < body.blocks.size(); index++) {
            auto block = body.block(mir, BlockId(index));
            for(U32 w = 0; w < words; w++) live[w] = 0;

            auto nodes = block->nodes.size();
            meetSuccessors(index, live.pointer());

            for(U32 n = nodes; n-- > 0;) {
                auto& node = *mir[block->nodes.get(mir, n)];

                // The same promotion `run` makes: what a lending hand-over names is live here
                // however little reads it, so it occupies its field here too - see `lentAt`.
                if(isYield[node.id.index] != kNoLocal && lentAt.size() &&
                   lentAt[node.id.index] != kNoLocal) {
                    set(live.pointer(), localBit(lentAt[node.id.index]));
                }

                for(U32 w = 0; w < words; w++) defs[w] = 0;
                set(defs.pointer(), valueBit(node.id.index));

                forEachOperand(mir, node, [&](Operand& operand, OperandRole role) {
                    if(role != OperandRole::WriteTarget) return;
                    if(operand.source() != SourceKind::Place) return;

                    auto root = rootLocalOf(mir, body, operand.placeId());
                    if(root != kNoLocal) set(defs.pointer(), localBit(root));
                });

                meet(live.pointer(), defs.pointer());
                applyBackward(live.pointer(), node);
            }

            meet(live.pointer(), nullptr);
        }
    }

    void run(DeclBase decl, const U32* isYield) {
        owing.reserve(words);
        for(U32 i = 0; i < words; i++) owing.push(0);

        for(U32 i = 0; i < locals; i++) {
            auto local = body.local(mir, LocalId(i));
            if(local->occurrence.contract.isOwned() && isLinearAt(decl, local->occurrence)) {
                set(owing.pointer(), localBit(i));
            }
        }

        entryLive.reserve(Size(words) * body.blocks.size());
        for(Size i = 0; i < Size(words) * body.blocks.size(); i++) entryLive.push(0);

        across.reserve(words);
        for(U32 i = 0; i < words; i++) across.push(0);

        perYield.reserve(Size(words) * yields.size());
        for(Size i = 0; i < Size(words) * yields.size(); i++) perYield.push(0);

        SmallArray<U64, 8> live;
        for(U32 i = 0; i < words; i++) live.push(0);

        /*
         * Reverse block order, to a fixpoint. The builder emits blocks roughly in forward order, so
         * a backward walk over the reversed list settles a loop-free body in one pass and a loop in
         * two - which is the same reason `analyze_live.cpp` walks its worklist the way it does.
         */
        auto changed = true;
        while(changed) {
            changed = false;

            for(U32 index = U32(body.blocks.size()); index-- > 0;) {
                auto block = body.block(mir, BlockId(index));
                for(U32 w = 0; w < words; w++) live[w] = 0;

                auto count = block->nodes.size();
                meetSuccessors(index, live.pointer());

                for(U32 n = count; n-- > 0;) {
                    auto& node = *mir[block->nodes.get(mir, n)];

                    // The set *after* the hand-over, which is what has to survive in the record.
                    if(isYield[node.id.index] != kNoLocal) {
                        auto slot = isYield[node.id.index];

                        // What a lending hand-over names is live here however little reads it - see
                        // `lentAt`, where the temporary a `yield Entry {...}` builds is the case.
                        if(lentAt.size() && lentAt[node.id.index] != kNoLocal) {
                            set(live.pointer(), localBit(lentAt[node.id.index]));
                        }

                        for(U32 w = 0; w < words; w++) {
                            across[w] |= live[w];
                            perYield[Size(slot) * words + w] |= live[w];
                        }
                    }

                    applyBackward(live.pointer(), node);
                }

                // The block's own parameters are defined here, so they are not live on the way in.
                for(auto argument: block->arguments.contents(mir)) {
                    clear(live.pointer(), valueBit(mir[argument]->id.index));
                }

                for(U32 w = 0; w < words; w++) {
                    auto& into = entryLive[Size(index) * words + w];
                    if((into | live[w]) == into) continue;

                    into |= live[w];
                    changed = true;
                }
            }
        }
    }
};

/*
 * Whether this producer's body can be a state record at all, and the message where it cannot.
 *
 * At the **mint** and not at the call site, because what it reads is the producer's blocks: the
 * question is about the declaration and the answer is the same at every call, so reporting it here
 * gives one diagnostic per producer rather than one per use. §6.1's two shapes, §3.2's nested
 * traversal, and the recursion §6.3 boxes.
 */
bool Resolver::checkObjectBody(Function& producer, Body& body, const LiveAcross& live,
                               const U32* isYield, LocationId source) {
    auto mir = *program.mir;
    auto ok = true;

    /*
     * A **lending** hand-over of something that is not a name.
     *
     * The slot holds where the value is, so the value has to be somewhere the record reaches - which
     * is a local of the producer, promoted to a payload member by `LiveAcross::lentAt`. An
     * expression handed over directly has no local to promote, so what the slot would hold is an
     * address in a step frame that returns before the consumer reads it. `lentAt` is empty where the
     * hand-over does not lend, so this is a question only a lending producer is asked.
     */
    for(U32 i = 0; i < live.lentAt.size(); i++) {
        if(isYield[i] == kNoLocal || live.lentAt[i] != kNoLocal) continue;

        context.diagnostics.error(
            "%@ lends what it hands over, and this one is not a name - an object lends the value out of its own record, so bind it before the `yield`"_v,
            body.node(mir, NodeId(i))->source, context.findName(producer.name));
        ok = false;
    }

    /*
     * Which local each `read` reaches, and which local each binding **borrows** - one pass each, so
     * that the two refusals below are lookups rather than searches.
     *
     * `borrows` is the `Init` that establishes a binding whose value is a borrowing read: that is
     * the only shape a self-borrow can arrive in here, because a loan is a value produced by a read
     * of a place and a binding is the write that installs one.
     */
    Array<U32> readRoot;
    for(U32 i = 0; i < body.nodes.size(); i++) {
        auto& node = *body.node(mir, NodeId(i));
        readRoot.push(node.kind == Node::Read ? rootLocalOf(mir, body, ((NodeRead&)node).place.placeId())
                                              : kNoLocal);
    }

    Array<U32> borrows;
    for(U32 i = 0; i < body.locals.size(); i++) borrows.push(kNoLocal);

    for(U32 i = 0; i < body.nodes.size(); i++) {
        auto& node = *body.node(mir, NodeId(i));
        if(node.kind != Node::Write) continue;

        auto& write = (NodeWrite&)node;
        if(write.value.source() != SourceKind::Result) continue;

        auto target = body.place(mir, write.destination.placeId());
        if(target->kind != Place::Local) continue;

        auto from = readRoot[write.value.nodeId().index];
        if(from != kNoLocal && from != target->payload) borrows[target->payload] = from;
    }

    /*
     * `addressOf` a local the producer keeps across a `yield` - §6.1's second shape.
     *
     * A record moves with whatever holds it, because every move here is bytewise, so an address of
     * its own storage taken before a hand-over is an address of the bytes it used to be at. The loan
     * checker sees where the address is *taken* and nothing at all once it is an integer, which is
     * why this is refused rather than checked.
     */
    for(U32 i = 0; i < body.nodes.size(); i++) {
        auto& node = *body.node(mir, NodeId(i));
        if(node.kind != Node::Address) continue;

        auto& taken = (NodeAddress&)node;
        if(taken.of.source() != SourceKind::Result) continue;

        auto root = readRoot[taken.of.nodeId().index];
        if(root == kNoLocal || !live.liveLocal(root)) continue;
        if(body.local(mir, LocalId(root))->demandsHeap) continue;

        context.diagnostics.error(
            "%@ takes the address of %@, which it keeps across a `yield` - an iterator object moves with whatever holds it, so the address would not stay valid; write `@heap` on the binding, or hold an index instead"_v,
            node.source, context.findName(producer.name),
            context.findName(body.local(mir, LocalId(root))->name));
        ok = false;
    }

    /*
     * A **borrow of the record's own inline storage** held across a `yield` - §6.1's first shape.
     *
     * The line is representation and not type, exactly as the design draws it: a view of heap-owned
     * contents - an `Array(T)`'s elements, a `String`'s bytes - points at storage that does not move
     * with the record, and only a borrow into the record's own bytes is self-referential. So what is
     * refused is a borrowing binding, live across a `yield`, of a *payload* local. A borrow of a
     * **parameter** is not one: a lent parameter travels as its address, so what it names is outside
     * the record; and `@heap` on the lender is the way out §6.1 names, because the bytes then live
     * in a run that does not move.
     */
    for(U32 i = 0; i < body.locals.size(); i++) {
        auto local = body.local(mir, LocalId(i));
        if(local->isParameter() || local->occurrence.contract.isOwned()) continue;
        if(!live.liveLocal(i)) continue;

        auto lender = borrows[i];
        if(lender == kNoLocal) continue;

        auto from = body.local(mir, LocalId(lender));
        if(from->isParameter() || from->demandsHeap) continue;
        if(!live.liveLocal(lender)) continue;

        context.diagnostics.error(
            "%@ borrows %@ and keeps the borrow across a `yield`, and both live in the object - a record moves with whatever holds it, so the borrow would point at where the value used to be; write `@heap` on %@, or hold an index instead"_v,
            local->source, context.findName(producer.name), context.findName(from->name),
            context.findName(from->name));
        ok = false;
    }

    /*
     * The **values** the record has to spill, and the two things this stage asks of one.
     *
     * A value live across a `yield` is spilled into a field at the hand-over and read back where the
     * next step resumes - see `LiveAcross`, where the counted `for`'s own block arguments are why
     * there are any. One limit, stated here rather than met:
     *
     *  - it must be **trivially copyable**. A store and a load either side of a suspension are a
     *    move through memory for anything else, and the drop elaboration the producer's body carries
     *    knows nothing about a field this transform invented - so an owner spilled here would be one
     *    nothing releases. Nothing in the language produces one today: what crosses is a loop's
     *    bound, its step and its counter.
     *
     * There used to be a second: every block that **names** it had to be converted after the
     * `yield`'s own, because the reload replaced the value for everything converted from that point
     * on, in index order. That refused `yield a; yield a` in a loop body and missed `yield a; if c:
     * yield a`, which reached the verifier as "jump names N, whose definition does not dominate it".
     * The step conversion now works out which definition reaches each block and reads the field
     * where more than one does (`resolve_ssa/convert.cpp`'s `planSpillReach`, J164), so there is
     * nothing left to refuse.
     */
    Array<U32> definedIn;
    for(U32 i = 0; i < body.nodes.size(); i++) definedIn.push(kNoLocal);

    for(U32 b = 0; b < body.blocks.size(); b++) {
        auto block = body.block(mir, BlockId(b));
        for(auto argument: block->arguments.contents(mir)) definedIn[mir[argument]->id.index] = b;
        for(auto node: block->nodes.contents(mir)) definedIn[mir[node]->id.index] = b;
    }

    for(U32 b = 0; b < body.blocks.size(); b++) {
        auto block = body.block(mir, BlockId(b));

        for(U32 n = 0; n < block->nodes.size(); n++) {
            auto& node = *mir[block->nodes.get(mir, n)];
            if(isYield[node.id.index] == kNoLocal) continue;

            for(U32 v = 0; v < body.nodes.size(); v++) {
                if(!live.liveValue(v)) continue;
                if(v == node.id.index || definedIn[v] == kNoLocal) continue;
                if(!LiveAcross::get(live.perYield.pointer() +
                                    Size(isYield[node.id.index]) * live.words, live.valueBit(v))) {
                    continue;
                }

                auto& defined = *body.node(mir, NodeId(v));
                if(!isTriviallyCopyable(decl(), valueOccurrence(defined).type)) {
                    context.diagnostics.error(
                        "%@ keeps a value that is not trivially copyable across a `yield` without naming it - an iterator object spills what crosses a hand-over into its record, and only a copy may be spilled; bind it to a name"_v,
                        defined.source, context.findName(producer.name));
                    ok = false;
                }
            }
        }
    }

    return ok;
}

/*
 * Which local of the producer a nested traversal's argument really comes from.
 *
 * `rootLocalOf` answers a place, and what a call site hands a traversal is not always a read of one:
 * a shared position at a type with a view is the *view*, so `items(xs)` passes what `deriveView`
 * built out of `xs` rather than `xs`. Following the derivation is what makes the question about
 * storage rather than about the spelling.
 *
 * Only the two steps that keep the answer the same storage - a derivation of one subject, and a
 * restatement that moves no bytes. Anything else answers `kNoLocal` and is refused by the caller,
 * which is the conservative direction: a lender this cannot name is one nothing vouches for.
 */
static U32 argumentRootLocal(Body& body, NodeId id, DeclBase decl, MirBase mir) {
    for(U32 step = 0; step < 8; step++) {
        auto& node = *body.node(mir, id);

        if(node.kind == Node::Read) {
            return rootLocalOf(mir, body, ((NodeRead&)node).place.placeId());
        }

        Operand next;

        if(node.kind == Node::Unary) {
            next = ((NodeUnary&)node).value;
        } else if(node.kind == Node::Call) {
            auto& call = (NodeCall&)node;
            if(call.callsValue() || !call.callee) return kNoLocal;
            if(!decl[call.callee]->derivation) return kNoLocal;

            auto arguments = call.arguments.contents(mir);
            if(arguments.size() != 1) return kNoLocal;

            next = *arguments.pointerAt(0);
        } else {
            return kNoLocal;
        }

        if(next.source() != SourceKind::Result) return kNoLocal;
        id = next.nodeId();
    }

    return kNoLocal;
}

/*
 * The **regions** a producer runs inside its own body - §3.2's first step, and the one shape a step
 * takes that the continuation-passing form does not.
 *
 * `for x in g(...)` written inside an `iter fn` is a `lens.call` opening a `Many` region whose blocks
 * are the loop body, and `withLock(m):` is the same node opening a `Once` one. In the CPS form those
 * blocks become a function of their own and the callee calls it; in a **step** there is nowhere to
 * call from - the outer producer is suspended, not running - so the child becomes a payload member
 * holding the callee's site type and the region's blocks are embedded in the step, with the member
 * stepped where the callee used to call. `resolve_ssa/convert.cpp` is where that is emitted; this is
 * where the members are decided.
 *
 * **A lens is the same member and a different edge**, which is the whole of §6.6 - built 2026-09-16.
 * The multiplicity is what the *conversion* reads: falling out of a `Many` region asks the member for
 * its next element, and falling out of a `Once` one steps it once more so that the code after the
 * lens's `yield` runs. Nothing here has to tell them apart beyond minting for a `lens fn` as well as
 * for an `iter fn`, because everything a site type is made of - the hand-over, the continuation
 * parameter, the liveness, the payload - is written on a lens too.
 *
 * The site type of each child is minted **recursively**, at the substitution the call binds, which is
 * `iterSiteTypeOf` called from inside itself - and a child that reaches *this* producer again is
 * §6.3's recursion, which is **boxed** rather than refused since 2026-09-16: the member holds the
 * erased `iter(Y)` instead of the child's record, so the outer record has a finite size and the
 * child lives in a heap block built where the region is entered. `iterMinting` is the cycle, and it
 * catches a mutual one as readily as a direct one - `walkA` is on the stack while `walkB` is being
 * minted inside it.
 *
 * Four shapes are still refused by name, each because the step would need something it has not got.
 *
 * Answers false having reported. The list is in body order, which is the order `mintIterSite` gives
 * the members and therefore the order the fields are laid out in.
 */
/*
 * How one captured argument travels into the record - the same four-way question `constructIter`
 * asks of the stack handle, asked of the **parameter's declaration** rather than of the argument.
 *
 * Of the declaration, because the record is a property of the producer and of its substitution: a
 * field list decided per call site would give one `iter fn` two site types, and `let it = f(x)` and
 * `let jt = f(y)` would be two types nothing could hold together.
 */
enum class IterCaptureKind: U8 {
    Value,    // the field holds the value - an owned parameter, or a shared trivially copyable one
    Address,  // the field holds the lender's address - every other borrow
    View,     // the field holds the view record - a window into somebody else's storage
};

// The one row `resolve_ssa` names, kept in step with the enumeration here - see `kCaptureAddress`.
static_assert(U8(IterCaptureKind::Address) == RecordType::kCaptureAddress,
              "the step conversion reads `Address` by number");
static_assert(U8(IterCaptureKind::View) == RecordType::kCaptureView,
              "the dumps read `View` by number");

static IterCaptureKind captureKindOf(DeclBase base, const Occurrence& parameter) {
    if(parameter.contract.isOwned()) return IterCaptureKind::Value;

    /*
     * A **window** is already a value: two words naming somebody else's storage, which travel into
     * the record exactly as they travel into a closure's environment or into `constructIter`'s
     * captured tuple. What the loan stands for is published at the constructor rather than held in
     * the field, which is why this is its own kind and not `Address` - the address of a window is
     * the address of the two words, and those are the caller's.
     */
    if(parameter.contract.form() == AccessForm::View) return IterCaptureKind::View;

    /*
     * A **shared** loan of a trivially copyable whole value is physically a copy, which no program
     * can tell from the loan - the same sentence `readMode` makes, and what lets a counter's bound
     * live in the record rather than behind a pointer into a frame that is not there any more.
     *
     * Shared and not any access: a `&` parameter is written *through*, and a copy cannot satisfy
     * writing through - the same exemption `captureKindOf` in resolve_lambda.cpp declines at `&`.
     *
     * And only where the parameter publishes **no origin**. A `'T` loan is a reference whatever its
     * type's layout says - `isReferenceValue` reads the origin before it reads the manifestation -
     * so a field holding the value would be filled from an address, which is the mismatch
     * `iter fn f(x: 'Big)` used to report as "call consumes a guaranteed value".
     */
    if(parameter.contract.access() == Access::Shared &&
       parameter.contract.form() == AccessForm::WholeValue &&
       parameter.contract.origin() != Origin::Slot &&
       isTriviallyCopyable(base, parameter.type)) {
        return IterCaptureKind::Value;
    }

    return IterCaptureKind::Address;
}

/*
 * How a **payload local** that is a loan travels into the record - `captureKindOf`, less the one
 * row that exists for a parameter.
 *
 * A `'T` parameter is held by address because `f$new` is filled from the argument as it arrives,
 * and a slot-rooted loan arrives as an address whatever its type. A local is filled by the step's
 * own `Init`, which adapts: where the member holds bytes and the loan is an address, it stores what
 * the loan names (`Conversion`'s `Init` arm). So a shared loan of a trivially copyable value is
 * carried as its bytes whichever origin it names - a copy no program can tell from the loan, and
 * one that stays good when the object moves.
 *
 * Which is the point (J131). `for e in each(xs)` inside a producer, with `each` handing over `'I32`,
 * binds `e` to the nested member's own slot - a copying child's item is where it keeps it - so a
 * member holding `e`'s *address* held an address inside this record, and a `let moved = it` taken
 * between two `yield`s left it naming the old bytes. The bytes themselves need no lender.
 */
static IterCaptureKind payloadKindOf(DeclBase base, const Occurrence& held) {
    if(!held.contract.isOwned() && held.contract.access() == Access::Shared &&
       held.contract.form() == AccessForm::WholeValue && Shapes::isEmpty(held.shape) &&
       isTriviallyCopyable(base, held.type)) {
        return IterCaptureKind::Value;
    }

    return captureKindOf(base, held);
}

/*
 * The producer a nest really reaches, at the arguments the call bound.
 *
 * `iterSiteTypeOf`'s own two lines, split out because the **cycle** has to be asked about before the
 * mint is entered: what says a child is recursive is whether its specialization is already on
 * `iterMinting`, and a specialization is what the mint would have made. Interning is what makes
 * asking twice free.
 */
static DeclPtr<Function> iterProducerAt(Context& context, Program& program, Function& declared,
                                        const Substitution& with, LocationId source) {
    auto producer = DeclPtr<Function>(&declared - *program.decl);
    if(!with.count) return producer;

    SmallArray<DeclPtr<Type>, 4> arguments;
    for(Size i = 0; i < with.count; i++) arguments.push(with.arguments[i]);

    return specializeFunction(context, program, producer, arguments.pointer(), arguments.size(),
                              source);
}

bool Resolver::collectIterNests(Function& producer, Body& body, Array<IterNest>& into,
                                LocationId source) {
    auto mir = *program.mir;
    auto ok = true;

    for(U32 i = 0; i < body.nodes.size(); i++) {
        auto& node = *body.node(mir, NodeId(i));
        if(!isLensCall(node.kind)) continue;

        auto& call = (NodeLensCall&)node;
        auto region = body.region(mir, call.region);
        if(!region) continue;

        // Which of the two a region is - a `for` over a traversal, or a lens over part of the
        // producer's own body. The only thing decided from it here is which words a refusal uses;
        // the edge the multiplicity really changes is the conversion's.
        auto lens = region->multiplicity != Continuation::Many;

        /*
         * A region that can **skip** - `| E` one level in, whether it is an iterator's failure or a
         * lens's.
         *
         * The alternative is a block of the outer producer reached from inside the callee, and what
         * the step would have to read is a terminal failure on the child's own step: the step
         * answers a `Bool`, so "it handed nothing over" and "it failed with `e`" are one answer.
         * Telling them apart is a second field and a second row of what the step returns, which is
         * exactly the thing §6.6 lists as `| E` on an object - so this is the same refusal the outer
         * producer gets, made where the child is.
         */
        if(node.kind != Node::LensCall || region->otherwise.valid()) {
            if(lens) {
                context.diagnostics.error(
                    "%@ runs %@ over part of its own body and %@ can fail, and an object's step answers only whether it handed over - a skipping lens region is not built yet; `for x in %@(...)` still compiles it the other way"_v,
                    node.source, context.findName(producer.name),
                    context.findName(decl()[call.callee]->name),
                    context.findName(decl()[call.callee]->name), context.findName(producer.name));
            } else {
                context.diagnostics.error(
                    "%@ runs a nested traversal that can fail, and an object's step has no terminal failure to hand it - `| E` on an object is not built yet; `for x in %@(...)` still compiles it the other way"_v,
                    node.source, context.findName(producer.name), context.findName(producer.name));
            }

            ok = false;
            continue;
        }

        // And one whose passes hand a value back - the `continue v` a nested pass would owe. The
        // step embeds the region's blocks and steps the member with "fall out"; carrying a value
        // into a nested pass is a second edge and is not built.
        if(region->resume.type && decl()[region->resume.type]->kind != Type::Unit) {
            context.diagnostics.error(
                "%@ runs a nested traversal whose passes hand a value back, and an object's step does not carry one into a nested pass yet"_v,
                node.source, context.findName(producer.name));
            ok = false;
            continue;
        }

        /*
         * Which body the call reaches, resolved the way every other reader resolves it.
         *
         * `callTarget` and not `call.callee`: `items` nests `chunks(self)`, which is a **class
         * member**, and at the concrete `c` this record is minted for the instance is decided. So
         * what the member holds is the site type of the *selected* body - which is also why §6.4's
         * refusal does not stand in the way here, since the declaration the refusal is about is the
         * class's and the one minted is the instance's.
         */
        CallSite site;
        callTarget(context, program, producer, callRequestOf(call), site);

        if(!site.target || site.through != CallThrough::Declaration || site.siteCall ||
           !bodyOwnerOf(decl(), *decl()[site.target]).lens) {
            context.diagnostics.error(
                lens ? "%@ runs a lens this stage cannot name a body for - an object of it is not built"_v
                     : "%@ runs a nested traversal this stage cannot name a body for - an object of it is not built"_v,
                node.source, context.findName(producer.name));
            ok = false;
            continue;
        }

        /*
         * Minted from the **generic** and the arguments the target bound, which is what
         * `iterSiteTypeOf` is written to take: a specialization carries its parameters and its
         * result and not the declaration flags a producer is recognized by, so handing it one
         * directly would be handing it something that does not say it is a traversal at all. The
         * specialization interns, so this reaches the same body `callTarget` named.
         *
         * A **lens**'s substitution carries one argument no written argument decided - `$r`, which
         * `handoverCall` binds to the exit signal. So `withLock` is generic here even where its
         * header is not, and the site type is one per binding of it exactly as it is one per
         * element type of a generic `iter fn`.
         */
        auto& declaring = bodyOwnerOf(decl(), *decl()[site.target]);
        auto childWith = substitutionOf(decl(), *decl()[site.target]);

        /*
         * The **cycle** - §6.3, and the one nest whose member is not the child's record.
         *
         * A child already on `iterMinting` is one this mint is inside: `walk` nesting itself is the
         * direct case and `walkA` -> `walkB` -> `walkA` is the mutual one, and neither has a finite
         * record if the member holds the child inline. So the member holds the **erased** `iter(Y)`
         * - two words over a heap block - and everything else about the nest is unchanged: the
         * region's blocks are still embedded, the exits are still the same three, and what differs
         * is where the child is built (at the call, not in `f$new`) and how it is ended (the box is
         * dropped, and the drop is what runs the child's own teardown).
         */
        auto reached = iterProducerAt(context, program, declaring, childWith, node.source);
        if(!reached) { ok = false; continue; }

        auto boxed = false;
        for(U32 m = 0; m < iterMinting.size(); m++) {
            if(iterMinting[m] != reached) continue;
            boxed = true;

            /*
             * And the cycle the back edge just closed, which is every producer from the one being
             * re-entered up to the one being minted - `iterCyclic`, and §6.3's delegation is what
             * reads it. A box lands on one edge of a cycle and a *level* is every producer round
             * it, so a region that wants to be skippable has to ask about the cycle rather than
             * about the box.
             */
            for(U32 k = m; k < iterMinting.size(); k++) {
                auto known = false;
                for(auto cyclic: iterCyclic) if(cyclic == iterMinting[k]) { known = true; break; }
                if(!known) iterCyclic.push(iterMinting[k]);
            }

            break;
        }

        DeclPtr<Type> child = nullptr;

        if(!boxed) {
            child = iterSiteTypeOf(declaring, childWith, node.source);
            if(!child) { ok = false; continue; }
        }

        /*
         * Whether this level **delegates** - §6.3's `yield from`, and open question 3's answer.
         *
         * Two conditions and both are structural. The pass is nothing but a re-yield
         * (`Continuation::reyield`), so there is nothing of this level between the child's item and
         * the consumer's and a trampoline may skip past it; and the child is in this producer's own
         * recursion cycle, so the chain of levels a walk builds is unbounded and skipping it is what
         * turns O(depth) per element into O(1). A re-yield over something that is not recursive -
         * `lines` re-yielding `splitOn` - is one level and stays exactly as J53 built it.
         *
         * A delegating level is **boxed** whether or not it was the back edge: what a trampoline
         * steps is a pair, so a level it may reach has to be one. That is the one allocation §6.3
         * says delegation keeps.
         */
        auto delegate = region->reyield;

        if(delegate) {
            auto cyclic = false;
            for(auto known: iterCyclic) if(known == reached) { cyclic = true; break; }
            delegate = cyclic;
        }

        if(!boxed && !delegate) {
            into.push(IterNest { i, call.region, child, nullptr });
            continue;
        }

        /*
         * A box this mint chose rather than one the cycle forced, which is the one case a refusal
         * below is not a refusal: `evens`' region over `odds` compiles today as an inline member, so
         * a delegating form of it that cannot be built falls back to that rather than reporting
         * something the author could not have caused. `fail` is what tells the two apart.
         */
        auto fallback = false;

        auto fail = [&](StringView message) {
            if(!boxed) { fallback = true; return; }

            context.diagnostics.error(message, node.source, context.findName(producer.name));
            ok = false;
        };

        auto& childOwner = bodyOwnerOf(decl(), *decl()[reached]);
        auto reachedWith = substitutionOf(decl(), *decl()[reached]);
        auto handover = substituteOccurrence(context, program, childOwner.handover, reachedWith);

        /*
         * A **window** hand-over through the box, which is the one item shape the embedding cannot
         * read back.
         *
         * A concrete child's slot holds the descriptor and the step reads the field; an erased one
         * answers an address, and what is at it is the descriptor rather than the owner - so the
         * region's entry would take two words derived from something that is already two words.
         * `answerItem` makes that distinction for a consumer holding the pair and the embedding
         * does not, so it is refused here rather than mis-read there.
         */
        if(handover.contract.form() == AccessForm::View) {
            fail("%@ runs a recursive traversal that hands over a window, and a boxed level answers where its item is rather than a field to read it out of - which is not built yet"_v);

            if(!fallback) continue;

            into.push(IterNest { i, call.region, child, nullptr });
            continue;
        }

        /*
         * What the child **borrows**, checked here because the box has nowhere to say it.
         *
         * A concrete nest's borrows are renumbered onto the outer record's own origin sets
         * (`mintIterSite`), and that walk is what refuses a child pointed at storage the outer
         * record holds inline - a record moves with whatever holds it, so the borrow would name
         * where the value used to be. An erased member carries no positions at all, so the same
         * question is asked of the **declarations**: a child position that travels as an address or
         * as a window has to be filled from the outer producer's own parameter, from an `@heap`
         * binding, or from a local the outer record itself holds by address. Everything else is
         * §6.1's self-borrow with one more indirection over it.
         */
        auto with = substitutionOf(decl(), producer);
        auto operands = call.arguments.contents(mir);
        auto held = true;

        for(U32 a = 0; a < childOwner.writtenArity() && a < operands.size(); a++) {
            auto& parameter = *decl()[childOwner.args.get(decl(), a)];
            auto position = substituteOccurrence(context, program, parameter.occurrence,
                                                 reachedWith);

            if(captureKindOf(decl(), position) == IterCaptureKind::Value) continue;

            auto lender = kNoLocal;
            auto& operand = *operands.pointerAt(a);

            if(operand.source() == SourceKind::Result) {
                lender = argumentRootLocal(body, operand.nodeId(), decl(), mir);
            }

            if(lender != kNoLocal) {
                auto local = body.local(mir, LocalId(lender));
                if(local->isParameter() || local->demandsHeap) continue;

                auto carried = substituteOccurrence(context, program, local->occurrence, with);
                if(payloadKindOf(decl(), carried) != IterCaptureKind::Value) continue;
            }

            fail("%@ runs a recursive traversal over storage the object itself holds, and a boxed level outlives the step that built it - so the child's borrow would point into a record that moves; write `@heap` on the binding, or traverse a parameter instead"_v);

            held = false;
            break;
        }

        if(!held) {
            if(!fallback) continue;

            into.push(IterNest { i, call.region, child, nullptr });
            continue;
        }

        auto resume = childOwner.resume.type
            ? substituteType(context, program, childOwner.resume.type, reachedWith) : nullptr;

        auto erased = erasedIterTypeOf(handover, resume, node.source);
        if(!erased) { ok = false; continue; }

        into.push(IterNest { i, call.region, erased, reached, delegate });
    }

    return ok;
}

/*
 * A callee's body, built now - see the declaration.
 */
bool Resolver::demandBody(Function& callee) {
    if(callee.bodyState != Function::BodyState::Unread) return true;

    for(U32 i = 0; i < writtenFunctions.size(); i++) {
        if(writtenFunctions[i] != DeclPtr<Function>(&callee - *program.decl)) continue;
        if(!writtenFunctionDecls[i]) break;

        // The position the callee was written at, and not the caller's - `requireResult`'s rule,
        // and for its reason: an import is scoped to the file that wrote it.
        ModuleScope scope = scopeOf(callee.module, callee.source);

        // And not the entry sequence's set of globals that have not been initialized yet, which is
        // the other half of what `requireResult` saves around a demanded body.
        Array<DeclPtr<Global>> pending(move(uninitialized));
        resolveBody(callee, *writtenFunctionDecls[i]);
        uninitialized = move(pending);
        break;
    }

    return callee.bodyState == Function::BodyState::Read;
}

/*
 * `f.Iter` - the site type, minted once per producer and substitution. See the file header.
 *
 * And `f.Lens` for a **`lens fn`**, which is the same record with one hand-over in it and is minted
 * by the same function - §6.6, built 2026-09-16. A lens is written `lens fn f(a) -> P` and declares
 * `fn ($r) f(a, $body: (P) -> $r) -> $r`, so everything below reads the same three things it reads
 * of an `iter fn`: `Function::handover`, `Function::continuationArg` and the body those two are
 * about. The row that is not the same is what `collectIterNests` reaches this through - a lens's
 * site type is minted only for a region a producer runs over part of its own body, because `.Iter`
 * in type position is an `iter fn`'s and a consumer never holds a lens object (`resolve_sig.cpp`).
 */
DeclPtr<Type> Resolver::iterSiteTypeOf(Function& declared, const Substitution& with,
                                       LocationId source) {
    assertTrue(declared.lens);

    /*
     * The declaration-level refusals, before anything is built - §6.6's gaps and §6.4's erased
     * producers, each by name.
     *
     * Here rather than at the call site so that a producer nothing can make an object of says so
     * once, against its own declaration, however many places try.
     */
    /*
     * The class's own declaration of a member, at a subject nothing has decided - so there is
     * nothing to mint a record out of.
     *
     * Not reachable from a **call** any more: `constructIterObject` selects the instance first and
     * mints that body's record, which is §6.4's concrete half (`iter/Witness.yana`). What still
     * arrives here is `walk.Iter` written in *type* position at a class member, where no subject has
     * been decided and the answer would have to be a record per instance.
     *
     * A member the class gave a **default** body arrives with its arguments decided, and is minted:
     * what an instance that left the member out runs is that body specialized at its own type, which
     * is an ordinary generic producer read under an ordinary substitution. The test is therefore
     * about the *substitution* rather than about the declaration - §6.4's witness entry is what
     * reaches it, since a default is what `instanceMember` hands back for a member nobody wrote.
     */
    if(declared.klass) {
        auto decided = with.count != 0;

        for(Size i = 0; i < with.count; i++) {
            if(with.arguments[i] && isConcreteType(decl(), with.arguments[i])) continue;
            decided = false;
            break;
        }

        if(!decided) {
            context.diagnostics.error(
                "%@ is a class member, so which record an object of it is, is the instance's - there is one per instance and this path names none"_v,
                source, context.findName(declared.name));
            return nullptr;
        }
    }

    if(declared.failure) {
        context.diagnostics.error(
            "%@ can fail, and an iterator object needs a terminal failure on its step - `| E` on an object is not built yet; `for x in %@(...)` still compiles it"_v,
            source, context.findName(declared.name), context.findName(declared.name));
        return nullptr;
    }

    for(U32 i = 0; i < declared.writtenArity(); i++) {
        auto& parameter = *decl()[declared.args.get(decl(), i)];
        if(!parameter.lazy) continue;

        context.diagnostics.error(
            "%@ takes %@ as a deferred argument, and a `@lazy` position is a thunk over the caller's frame - an object outlives that frame, so it is not built yet"_v,
            source, context.findName(declared.name), context.findName(parameter.name));
        return nullptr;
    }

    /*
     * `<- R` at a type the step's own parameter would **owe** something for.
     *
     * The step is handed the continuation's answer, and for a resuming producer that answer is
     * `Outcome(R, exit)` - an owned record parameter of a function whose blocks are the producer's,
     * so there is no drop elaboration of this stage's to release it on the paths that never reach a
     * `yield`: index zero runs the producer from the start and never reads it, the finished index
     * returns without reading it, and a `yield` written without a binding drops the answer on the
     * floor. A trivially copyable `R` owes nothing on any of them.
     *
     * Nor would a **lent** one: a channel that hands the pass's value over as a loan leaves the
     * wrapper's payload somebody else's, so the step's parameter is one word with nothing to
     * release. The test is written that way rather than left out, and it is **not reachable today**:
     * `<- &R` is refused at the declaration by `resolve_sig.cpp` ("an exclusive `<- &R` resume value
     * yet") and an unqualified `<- R` resolves owned. It is here so that the day the declaration
     * side admits one, the object does not have to be taught about it separately.
     *
     * What would remove the rest is a drop of `$resume` at each of the three paths that do not read
     * it, which needs "does this `yield`'s result have a use" asked of the producer's MIR and a drop
     * emitted against a block argument. It is a walk and an instruction rather than a shape, and it
     * is left out because nothing in the corpus reaches it.
     */
    if(declared.resume.type && declared.resume.contract.isOwned() &&
       !isTriviallyCopyable(decl(), declared.resume.type)) {
        context.diagnostics.error(
            "%@ takes a resume value that is not trivially copyable, and an object's step is handed that value as a parameter - which is not built yet"_v,
            source, context.findName(declared.name));
        return nullptr;
    }

    /*
     * The body, built now if nothing has - and the one thing a demand can find instead of a body: a
     * producer whose body **holds** an object of itself, met while that body is still being read.
     *
     * Not §6.3's recursion, which is a `for` over a child and is boxed (`collectIterNests`). This is
     * a record inside a record: a held object is a member at the child's own site type, and no
     * indirection the mint could add would be one the author asked for. `iter/Reject.Object.yana`
     * holds it.
     */
    if(declared.bodyState == Function::BodyState::Reading) {
        context.diagnostics.error(
            "%@ holds an object of itself, and a record cannot contain itself - a recursive `for x in %@(...)` is boxed one `iter(T)` per level, and a held object is not"_v,
            source, context.findName(declared.name), context.findName(declared.name));
        return nullptr;
    }

    if(!demandBody(declared)) {
        context.diagnostics.error("%@ has no body to step"_v, source,
                                  context.findName(declared.name));
        return nullptr;
    }

    // The producer at these arguments. Concrete by the call site's own refusal - a construction
    // inside a generic body is refused there, because a site type has no parameters of its own.
    auto producer = iterProducerAt(context, program, declared, with, source);
    if(!producer) return nullptr;

    for(auto& known: iterSites) {
        if(known.producer == producer) return known.record;
    }

    auto& owner = bodyOwnerOf(decl(), *decl()[producer]);
    if(!owner.mir) {
        context.diagnostics.error("%@ has no body to step"_v, source,
                                  context.findName(declared.name));
        return nullptr;
    }

    auto mir = *program.mir;
    auto& body = *mir[owner.mir];

    /*
     * The hand-overs, in body order, and the liveness across them - §3.2's second step.
     *
     * A `yield` is a call of the continuation parameter and nothing else (`resolveYield`), so what
     * marks one is a `call` through a value read out of that parameter's place. The read itself is
     * the one node with no storage at all in the step.
     */
    auto continuationArg = owner.continuationArg;
    assertTrue(continuationArg != Function::kNoContinuation);

    Array<U32> readsContinuation;
    for(U32 i = 0; i < body.nodes.size(); i++) {
        auto& node = *body.node(mir, NodeId(i));
        auto reads = false;

        if(node.kind == Node::Read) {
            auto place = body.place(mir, ((NodeRead&)node).place.placeId());
            reads = place->kind == Place::Local &&
                    body.local(mir, LocalId(place->payload))->parameter == continuationArg;
        }

        readsContinuation.push(reads ? 1 : 0);
    }

    LiveAcross live(mir, body);

    Array<U32> yieldSlot;
    for(U32 i = 0; i < body.nodes.size(); i++) {
        auto& node = *body.node(mir, NodeId(i));
        auto isYield = false;

        if(node.kind == Node::Call) {
            auto& call = (NodeCall&)node;
            isYield = call.callsValue() && call.target.source() == SourceKind::Result &&
                      readsContinuation[call.target.nodeId().index];
        }

        yieldSlot.push(isYield ? U32(live.yields.size()) : kNoLocal);
        if(isYield) live.yields.push(i);
    }

    /*
     * Which local each **lending** hand-over names - see `LiveAcross::lentAt`.
     *
     * The hand-over is read off the body's owner rather than off the specialization: a clone carries
     * its parameters and its result, and `yield`'s outgoing occurrence is written on the declaration
     * `bodyOwnerOf` answers.
     */
    auto handover = substituteOccurrence(context, program, owner.handover,
                                         substitutionOf(decl(), *decl()[producer]));

    if(objectLends(decl(), handover)) {
        for(U32 i = 0; i < body.nodes.size(); i++) live.lentAt.push(kNoLocal);

        for(U32 i = 0; i < body.nodes.size(); i++) {
            if(yieldSlot[i] == kNoLocal) continue;

            auto& call = (NodeCall&)*body.node(mir, NodeId(i));
            auto items = call.arguments.contents(mir);
            if(!items.size()) continue;

            auto& argument = *items.pointerAt(0);
            if(argument.source() != SourceKind::Result) continue;

            auto& handed = *body.node(mir, argument.nodeId());
            if(handed.kind != Node::Read) continue;

            live.lentAt[i] = rootLocalOf(mir, body, ((NodeRead&)handed).place.placeId());
        }
    }

    live.run(decl(), yieldSlot.pointer());

    if(!checkObjectBody(*decl()[producer], body, live, yieldSlot.pointer(), source)) return nullptr;

    /*
     * The nested traversals, minted before the record because each is a **member** of it - §3.2's
     * first step, `collectIterNests`.
     *
     * `iterMinting` is what §6.3's recursion is read off - `collectIterNests` boxes a child already
     * on the stack rather than minting it, so what reaches the guard below is the *other* shape a
     * cycle takes: a producer whose body **holds** an object of one that is minting it, which is a
     * record inside a record however many levels round the cycle it is.
     */
    for(auto minting: iterMinting) {
        if(minting != producer) continue;

        context.diagnostics.error(
            "%@ holds an object of itself, and a record cannot contain itself - a recursive `for x in %@(...)` is boxed one `iter(T)` per level, and a held object is not"_v,
            source, context.findName(declared.name), context.findName(declared.name));
        return nullptr;
    }

    iterMinting.push(producer);
    PopOnExit<DeclPtr<Function>, 8> minting { iterMinting };

    Array<IterNest> nests;
    if(!collectIterNests(*decl()[producer], body, nests, source)) return nullptr;

    for(auto refused: iterNestRefused) {
        if(refused == producer) return nullptr;
    }

    Array<U8> fromItem;
    if(!refuseHeldNestItems(*decl()[producer], body, live, nests, fromItem)) {
        iterNestRefused.push(producer);
        return nullptr;
    }

    auto made = mintIterSite(*decl()[producer], owner, body, live, yieldSlot.pointer(), nests,
                             source);
    if(!made) return nullptr;

    /*
     * And whether the item this record lends may be inside it - `RecordType::itemInRecord`, which
     * the producer nesting this one reads. A `yield` names a local (`lentAt`); the bytes are the
     * record's where that local is a payload member holding its value, or a borrow bound out of a
     * nested item that is itself the record's. A parameter, an `@heap` binding and a window name
     * storage outside it.
     */
    auto& record = *(RecordType*)decl()[made];

    for(U32 i = 0; i < live.lentAt.size(); i++) {
        auto root = live.lentAt[i];
        if(root == kNoLocal) continue;

        auto local = body.local(mir, LocalId(root));
        if(local->isParameter() || local->demandsHeap) continue;

        auto held = substituteOccurrence(context, program, local->occurrence, with);
        if(payloadKindOf(decl(), held) == IterCaptureKind::Value ||
           (root < fromItem.size() && fromItem[root])) {
            record.itemInRecord = true;
        }
    }

    return made;
}

/*
 * A borrow of a nested traversal's item **kept across this producer's own `yield`**, where the item
 * lives inside the child's record - and so inside this one (J131).
 *
 * The child lends out of its own payload (`RecordType::itemInRecord`), so the loop variable is an
 * address inside the record, and a local that holds it across a hand-over is a member holding that
 * address. A record moves with whatever holds it, so after `let moved = relocate(it)` the member
 * named where the child used to be - the same self-borrow `checkObjectBody` refuses between two
 * payload locals, one indirection further away, where that check cannot see the lender because it is
 * a block argument rather than a local. A copying child's scalar item is not one of these: the
 * member holds the value (`payloadKindOf`). Neither is a child that lends the caller's storage.
 *
 * `fromItem` answers, per local, whether it is bound out of such an item - directly, or by a `let`
 * that borrows one that is - which is also what says whether this record's own item is inside it.
 */
bool Resolver::refuseHeldNestItems(Function& producer, Body& body, const LiveAcross& live,
                                   const Array<IterNest>& nests, Array<U8>& fromItem) {
    auto mir = *program.mir;
    for(U32 i = 0; i < body.locals.size(); i++) fromItem.push(0);

    // The entry arguments of the regions whose child lends out of its own record.
    Array<U32> items;
    for(auto& nest: nests) {
        if(!nest.record || nest.boxed) continue;
        if(decl()[nest.record]->kind != Type::Record) continue;
        if(!((RecordType*)decl()[nest.record])->itemInRecord) continue;

        auto region = body.region(mir, nest.region);
        if(!region) continue;

        auto entry = body.block(mir, region->entry);
        for(auto argument: entry->arguments.contents(mir)) items.push(mir[argument]->id.index);
    }

    if(!items.size()) return true;

    // Bindings in node order, which is the order a `let` over a `let` is written in.
    for(U32 b = 0; b < body.blocks.size(); b++) {
        auto block = body.block(mir, BlockId(b));

        for(auto pointer: block->nodes.contents(mir)) {
            auto& node = *mir[pointer];
            if(node.kind != Node::Write) continue;

            auto& write = (NodeWrite&)node;
            if(write.mode != Destination::Init || write.value.source() != SourceKind::Result) continue;

            auto target = body.place(mir, write.destination.placeId());
            if(target->kind != Place::Local) continue;

            auto value = write.value.nodeId().index;
            auto bound = false;

            for(auto item: items) bound = bound || item == value;

            if(!bound) {
                auto& read = *body.node(mir, write.value.nodeId());
                if(read.kind == Node::Read) {
                    auto root = rootLocalOf(mir, body, ((NodeRead&)read).place.placeId());
                    bound = root != kNoLocal && fromItem[root];
                }
            }

            if(bound) fromItem[target->payload] = 1;
        }
    }

    auto ok = true;
    auto with = substitutionOf(decl(), producer);

    for(U32 i = 0; i < body.locals.size(); i++) {
        if(!fromItem[i] || !live.liveLocal(i)) continue;

        auto local = body.local(mir, LocalId(i));
        if(local->isParameter() || local->occurrence.contract.isOwned()) continue;

        auto held = substituteOccurrence(context, program, local->occurrence, with);
        if(payloadKindOf(decl(), held) == IterCaptureKind::Value) continue;

        context.diagnostics.error(
            "%@ keeps %@ across a `yield`, and %@ is an item a nested traversal lends out of its own object - which lives inside this one, and a record moves with whatever holds it, so the borrow would point at where the item used to be; copy what is needed out of %@ before the `yield`"_v,
            local->source, context.findName(producer.name), context.findName(local->name),
            context.findName(local->name), context.findName(local->name));
        ok = false;
    }

    return ok;
}

/*
 * The record, its constructor, its step and its teardown - §3.1's field order, built once the
 * payload is known.
 *
 * `buildEnvironment` in resolve_lambda.cpp is the shape this follows: a record shell, a field list,
 * one constructor over it, the schema, the classification. What differs is that the field list is
 * complete before the type exists rather than accumulated by a body walk, so the constructor is
 * stated here and not at the end.
 */
DeclPtr<Type> Resolver::mintIterSite(Function& producer, Function& owner, Body& body,
                                     LiveAcross& live, const U32* isYield, Array<IterNest>& nests,
                                     LocationId source) {
    auto mir = *program.mir;
    auto with = substitutionOf(decl(), producer);
    auto module = producer.module;

    auto handover = substituteOccurrence(context, program, owner.handover, with);

    /*
     * An **owning** hand-over of something that owes a teardown - `-> ->T` at a `T` with a `Drop`.
     *
     * The slot then holds a value the record owes, and what says whether it is occupied is not the
     * index: a consumer that moves out of the slot leaves the record at a suspend index with nothing
     * in it, and one that steps again without taking leaves the previous item there. Telling the two
     * apart is `$held`, a flag the step sets as it writes, every consumer clears as it takes, and
     * the teardown reads - `RecordType::heldField`, the last field of the record.
     */
    auto flagged = handover.contract.isOwned() && hasTeardown(decl(), handover.type);

    /*
     * `.Iter` for an `iter fn` and `.Lens` for a `lens fn` - the two are one record and are named
     * apart because the name is what every dump, descriptor and message shows.
     *
     * Read off the **declaration** and not off `producer`, which is the specialization:
     * `specializeFunction` copies `lens` and not `iterator` (J53 §1.5), so the clone says it is a
     * traversal and does not say which kind. `RecordType::lensSite` is where the answer is kept for
     * the three readers below this that need it again.
     */
    auto lensSite = !bodyOwnerOf(decl(), producer).iterator;

    auto text = Tritium::format("%@%@", context.findName(producer.symbol),
                                lensSite ? ".Lens"_v : ".Iter"_v);
    auto name = program.uniqueSymbol(module, context, StringView(text.text(), text.size()));

    auto record = program.types.recordType(name, module, producer.source);
    auto recordType = asBase<Type>(DeclPtr<RecordType>(record - *program.decl));
    record->lensSite = lensSite;

    /*
     * `pub` on the **producer**, which is what a site type's visibility is - §2.4's "across a module
     * boundary a site type is a declaration reference plus its layout".
     *
     * Nothing writes `pub` on a minted record, and it is not a name a program can look up: what an
     * importer writes is `Lex.lex.Iter`, which is a path through the *function*. So the marker the
     * export check reads has to be the one the author wrote, or a `pub fn` answering an object would
     * leak a type that is private only because nobody declared it.
     *
     * The declaration's and not the specialization's, for the reason `bodyOwnerOf` exists: a clone
     * carries its parameters and its result, and `pub` is written on the generic.
     */
    record->exported = bodyOwnerOf(decl(), producer).exported;

    auto field = [&](StringId fieldName, Occurrence occurrence) {
        RecordField made;
        made.name = fieldName;
        made.occurrence = occurrence;
        made.source = producer.source;
        made.index = U16(record->fields.size());
        record->fields.push(program.decl, made);
    };

    // Field 0 - the resume index. An `I32`, which is the exit signal's width and is what the step's
    // `switch` selects on.
    field(context.addUnqualifiedName("$index", 6), owned(program.types.exitCode()));

    /*
     * Field 1 - the hand-over slot.
     *
     * The value where the convention owns or copies, and the lent value's **address** where it
     * lends: what the consumer holds between two steps is a borrow of storage the producer still
     * owns, and the record has nowhere to put that storage. §2.6's "a lent value lives until the
     * next step" is the extent, and what enforces it is the loan the *read of the slot* takes
     * against the record - stepping needs the record whole.
     */
    auto lends = objectLends(decl(), handover);
    auto slotType = lends ? program.types.pointerType(handover.type) : handover.type;

    /*
     * And the **window**'s own shape, where the hand-over is one: the descriptor rather than the
     * owner the type names - see `objectLends` and `RecordType::slotWindow`.
     */
    if(!lends && handover.contract.form() == AccessForm::View) {
        if(auto descriptor = viewRecordOf(context, program, handover)) {
            slotType = descriptor;
            record->slotWindow = true;
        }
    }

    /*
     * Owned and **shapeless**, which is `objectLends`' doing rather than an omission: a hand-over
     * whose occurrence carries interior positions lends, by that function's third test, so what the
     * slot holds is an address and there is no interior of its own for a shape to describe. The two
     * rows that reach here with the type itself - a window, and a trivially copyable value - have an
     * empty shape already.
     */
    field(context.addUnqualifiedName("$slot", 5), owned(slotType));

    // Whether any payload member is `@heap`-placed, which the record owes a free for whatever else
    // its members are - see the payload loop below.
    auto placed = false;

    /*
     * Which interior positions the lent value really has - the **member's**, not the declaration's.
     *
     * A hand-over declared `Ent('Int)` at `Int` publishes an origin on a scalar the source copies, so
     * the value the producer builds has its field inline and the declaration's shape describes
     * nothing physical. One declared `Entry('k, 'v)` at `(String, I32)` has a field that really is a
     * window. The record is where the value lives, so what says which of the two this is is the
     * local the `yield` names - and `f$item` hands the value back at that reading, since it is the
     * storage it reads from.
     */
    if(lends) {
        for(U32 i = 0; i < live.lentAt.size(); i++) {
            if(live.lentAt[i] == kNoLocal) continue;

            auto lent = body.local(mir, LocalId(live.lentAt[i]));
            handover.shape = substituteOccurrence(context, program, lent->occurrence, with).shape;
            break;
        }
    }

    record->payloadFirst = U16(2 + owner.writtenArity());
    record->handovers = U16(live.yields.size());
    record->payloadYieldWords = U16((live.yields.size() + 63) / 64);

    /*
     * Where each node stands in the order the conversion emits them - blocks by index, nodes within
     * a block in order.
     *
     * One reader, and it is the value spills: the conversion holds one entry per value and fills it
     * as it walks, so whether a `yield` is emitted before or after a value's definition is what
     * decides whether the store into that member happens at all. See the spill loop below.
     */
    Array<U32> nodeOrder;
    for(U32 i = 0; i < body.nodes.size(); i++) nodeOrder.push(0);

    // And which region each node stands in, which is what says whether a hand-over is written
    // inside a nested traversal - see the nest members below.
    Array<ContinuationId> nodeRegion;
    for(U32 i = 0; i < body.nodes.size(); i++) nodeRegion.push(ContinuationId(0));

    {
        U32 position = 0;
        for(U32 b = 0; b < body.blocks.size(); b++) {
            auto block = body.block(mir, BlockId(b));
            for(auto argument: block->arguments.contents(mir)) {
                nodeOrder[mir[argument]->id.index] = position++;
                nodeRegion[mir[argument]->id.index] = block->region;
            }

            for(auto node: block->nodes.contents(mir)) {
                nodeOrder[mir[node]->id.index] = position++;
                nodeRegion[mir[node]->id.index] = block->region;
            }
        }
    }

    /*
     * The captured arguments - one field each, in the producer's own parameter order.
     *
     * The record **borrows what it was lent**, exactly as a closure borrows what it captured: a lent
     * parameter's field holds an address and the field's occurrence publishes an origin, so `f$new`'s
     * signature binds the record to each argument's loan and the lent locals stay borrowed for as
     * long as the object is live. `iterConstructor` makes the same statement about the stack handle
     * with one slot; this is a slot per capture, because §2.2 is explicit that grouping does not
     * claim the members are one object.
     */
    SmallArray<U16, 4> captureSlot;

    for(U32 i = 0; i < owner.writtenArity(); i++) {
        auto& parameter = *decl()[owner.args.get(decl(), i)];
        auto substituted = substituteOccurrence(context, program, parameter.occurrence, with);
        auto contract = substitutedContract(decl(), program, parameter.occurrence.type,
                                            substituted.type, substituted.contract);

        auto kind = captureKindOf(decl(), Occurrence { substituted.type, contract,
                                                       substituted.shape });

        record->captureKinds.push(program.decl, U8(kind));

        if(kind == IterCaptureKind::Value) {
            captureSlot.push(kNoOriginSlot);
            field(parameter.name, owned(substituted.type));
            continue;
        }

        // Which of the record's own origin sets this capture publishes, kept so that a **nested**
        // traversal borrowing the same parameter can say it borrows the same storage - see the nest
        // members below, where the child's slots are renumbered onto these.
        captureSlot.push(record->originSlots);

        /*
         * A **window**, held as the **view record** - the two words of a descriptor, owned outright.
         *
         * `constructIter`'s `IterCapture::View` makes the same field for the stack handle, and for
         * the same reason: a window is already a value, so what the record holds is its bytes and
         * what the loan is about is on the far side of them. The step reads the field as the view
         * the parameter declared, which is the same bytes under the occurrence that says what they
         * mean - `iterEntry`'s `addRepresent` one representation over.
         *
         * The field carries **no origin**, which is the one thing tried and withdrawn: a field whose
         * contract publishes a slot is a `isReferenceValue` in the lowering *and* a view record in
         * `manifestNative`, and the two disagree by one indirection - the reader loads the
         * descriptor's first word and treats it as the descriptor. So the loan is stated where
         * `iterConstructor` states it, on `f$new`'s result, and the field is a plain member.
         */
        if(kind == IterCaptureKind::View) {
            Occurrence window { substituted.type, contract, substituted.shape };
            auto descriptor = viewRecordOf(context, program, window);

            if(!descriptor) {
                context.diagnostics.error(
                    "internal: %@ captures %@ as a window with no view record"_v, source,
                    context.findName(producer.name), context.findName(parameter.name));
                return nullptr;
            }

            /*
             * The loan, at the descriptor's own **pointer member** - the one position inside it that
             * names the lender's storage, which is `iterConstructor`'s `field(0)/pointee` on the
             * handle said about a window instead. A path of two steps rather than one, so that it is
             * provenance rather than a member taken away: `qualifiesInterior` reads a single field
             * step, and this is deliberately not one.
             */
            auto& shape = *(RecordType*)decl()[descriptor];
            auto reaches = U16(0xffff);

            for(auto member: shape.fields.contents(decl())) {
                if(decl()[member.occurrence.type]->kind != Type::Pointer) continue;
                reaches = member.index;
                break;
            }

            /*
             * A view record with no pointer in it, which no row of `kViewRows` is today: both
             * `ArrayView(a)` and `StringView` lead with one. The fallback is here because a third row
             * is a library's to add and this walk must not silently point an origin at a field it
             * did not find.
             *
             * The field is then a plain member publishing **nothing**, so the slot recorded above has
             * to be taken back: `captureSlot` is what a nested traversal borrowing this parameter is
             * attributed to, and leaving the number there without spending it would give the *next*
             * publishing capture the same slot and file the child's borrow under the wrong lender.
             */
            if(reaches == 0xffff) {
                captureSlot[captureSlot.size() - 1] = kNoOriginSlot;
                field(parameter.name, owned(descriptor));
                continue;
            }

            ExternalSlot lent;
            lent.path.push(program.decl, PathStep::field(reaches));
            lent.path.push(program.decl, PathStep::pointee());
            lent.contract = slotContract(contract.access(), record->originSlots);

            field(parameter.name, Occurrence { descriptor, ownedContract(),
                                               program.shapes.intern(&lent, 1) });
            record->originSlots++;
            continue;
        }

        /*
         * A pointer field carrying the origin at its **pointee**, which is `iterConstructor`'s own
         * shape and is here for the same reason: a qualifier on the field itself would take the
         * member away, and what the loan keeps alive is the storage the pointer reaches.
         */
        ExternalSlot slot;
        slot.path.push(program.decl, PathStep::pointee());
        slot.contract = slotContract(contract.access(), record->originSlots);

        field(parameter.name, Occurrence { program.types.pointerType(substituted.type),
                                           ownedContract(), program.shapes.intern(&slot, 1) });
        record->originSlots++;
    }

    /*
     * The payload - §3.2's second step and its third, in that order.
     *
     * One **entry** per local live across some `yield`, in `Body::locals` order, then one per value
     * that crosses one without a name; and then a field per entry, or a field shared by several of
     * them where their live ranges never meet. Every member is owned here whatever the local's own
     * contract said, because the record holds the storage - a local that is a borrow is one whose
     * lender `checkObjectBody` has already vouched for.
     *
     * The entries are gathered before any field is made, which is what the sharing needs: the
     * interference is over the whole set, and a field's *name* is the first member that landed in
     * it.
     */
    SmallArray<U32, 16> entryCode;
    SmallArray<U32, 16> entryBit;

    // The hand-over sets, flat: `yieldWords` words per entry, which is `RecordType::payloadYields`'
    // own shape - see it, where the bitmap rather than a mask is argued.
    auto yieldWords = U32((live.yields.size() + 63) / 64);
    SmallArray<U64, 16> entryKeeps;
    SmallArray<Occurrence, 16> entryHeld;
    SmallArray<StringId, 16> entryName;

    // Whether the entry may share a field at all - see the assignment below, where the two rows
    // that may not are argued.
    SmallArray<U8, 16> entryShares;

    /*
     * Live across a hand-over, **or placed** - and the second is not an optimization the first
     * subsumes.
     *
     * `@heap` says the value lives in a run of its own for the object's whole life, which is a
     * statement about *where* it is rather than about when it is read: the address is handed out, and
     * what §6.1 promises is that it stays good while the object does. So the member exists whether or
     * not anything reads the local after a `yield`.
     *
     * Skipping it made a placed local an ordinary frame slot of the step - and a step's frame is
     * entered once per element. `convert.cpp`'s `kNoField` arm allocated it in the dispatch block, so
     * `let @heap base = Cell {...}` read only before the first `yield` was one heap allocation *per
     * step call* with no `free` anywhere in the module, which `iter/Heap.yana` did not catch because
     * every producer in it authors a `Drop` - and a teardown is the other thing that forces a local
     * into the record.
     */
    for(U32 i = 0; i < body.locals.size(); i++) {
        auto local = body.local(mir, LocalId(i));
        if(local->isParameter()) continue;
        if(!live.liveLocal(i) && !local->demandsHeap) continue;

        /*
         * Owned, and carrying the local's own **interior positions**.
         *
         * The contract is the record's - the storage is here, whatever the local's own contract
         * said - but the shape is not a contract: `Ent('Int)` lays its field out as a reference and
         * `Ent(Int)` lays it out as a member, so a shape dropped here would be a field of the wrong
         * width filled by a store of the right one. `manifestNative`'s "a shape the type does not
         * have" is the case, and a qualified type argument is how it arrives.
         */
        auto held = substituteOccurrence(context, program, local->occurrence, with);

        auto keeps = entryKeeps.size();
        for(U32 w = 0; w < yieldWords; w++) entryKeeps.push(0);

        for(U32 y = 0; y < live.yields.size(); y++) {
            if(LiveAcross::get(live.perYield.pointer() + Size(y) * live.words, live.localBit(i))) {
                entryKeeps[keeps + y / 64] |= U64(1) << (y % 64);
            }
        }

        /*
         * `@heap` on the binding - the member in a run of its own, and the field holding its
         * address. §6.1's second fix, and the thing that makes the self-borrow refusal's advice
         * work rather than merely name a spelling: the run does not move with the record, so a
         * borrow into it survives the object being moved.
         *
         * Allocated by `f$new` rather than at the step that first reaches the binding, which is one
         * allocation for the object's life instead of a flag saying whether there has been one. The
         * record then owes a free whatever its members are, which is why `owes` is forced below.
         *
         * **A placed member never shares.** The field is a run's address written once at
         * construction and freed once at the exits, so two members in one field would be one
         * allocation whose lifetime is neither member's - and what it would save is a pointer,
         * against a `f$new` that has to know which of the sharers it is allocating for.
         */
        if(local->demandsHeap) {
            entryCode.push(kIterHeapMember | i);
            entryHeld.push(owned(program.types.pointerType(held.type)));
            entryShares.push(0);
            placed = true;
        } else {
            /*
             * And otherwise **how the loan travels**, which is the three-way question
             * `captureKindOf` asks of a parameter, asked through `payloadKindOf` - the same
             * function less its slot-origin row, which a local's `Init` adapts where a
             * constructor's argument cannot.
             *
             * It has to be one question, because what the step does with the member is decided
             * by `aliasesLender` one tier down: a member whose occurrence *is* an address needs a
             * pointer field and no storage of its own, and one whose occurrence is bytes needs a
             * field those bytes fit in and the field's address as its storage - filled with what
             * the loan names where the loan is an address (J131). Asking it twice with two rules
             * is how `for chunk in chunks(self)` came to store a two-word descriptor into a
             * pointer field and read it back as one; the two readers below that ask whether the
             * record holds a local by address ask `payloadKindOf` too.
             *
             * An **owned** local answers `Value` and is the ordinary member it always was.
             */
            auto travels = payloadKindOf(decl(), held);
            auto descriptor = travels == IterCaptureKind::View
                ? viewRecordOf(context, program, held) : nullptr;

            if(travels == IterCaptureKind::View && descriptor) {
                /*
                 * A **window** the producer holds across a hand-over - a `for` variable over a
                 * traversal that hands over `[a]`, and the same field `IterCaptureKind::View` makes
                 * for a captured one: the descriptor's own two words, owned outright.
                 *
                 * No origin on the field, for the reason the capture's carries one and this does
                 * not: a captured window is lent by the *caller* and `f$new` binds the loan, and
                 * this one is built by the producer out of storage the object already keeps
                 * borrowed. `iterConstruction` has no argument to bind it to and there is nothing
                 * for it to say.
                 */
                entryCode.push(i);
                entryHeld.push(owned(descriptor));
                entryShares.push(1);
            } else if(travels == IterCaptureKind::Address) {
                /*
                 * A local that is a **loan** reached through an address - a second name for storage
                 * rather than a second object.
                 *
                 * The field holds where the storage is, because a field holding the value would be a
                 * copy of somebody else's, and a borrow that copies is not a borrow. Which storage is
                 * decided in the producer's body, so the field is written where the binding is
                 * established and read at every use - see `addressOf`'s `Local` case.
                 *
                 * That the address survives the object moving is what `checkObjectBody` vouches for:
                 * a borrow of another payload member is refused unless the lender is `@heap`, and a
                 * borrow of a parameter names storage outside the record.
                 */
                entryCode.push(RecordType::kBorrowMember | i);
                entryHeld.push(owned(program.types.pointerType(held.type)));
                entryShares.push(1);
            } else {
                entryCode.push(i);
                entryHeld.push(Occurrence { held.type, ownedContract(), held.shape });
                entryShares.push(1);
            }
        }

        entryBit.push(live.localBit(i));
        entryName.push(local->name);
    }

    /*
     * And the **values** - what crosses a hand-over without a name, which is a counted `for`'s own
     * block arguments and the like (`LiveAcross`). An entry each, recorded as a node index with the
     * high bit set, so that `payloadLocals` is one list of what the step has to find storage for.
     */
    for(U32 i = 0; i < body.nodes.size(); i++) {
        if(!live.liveValue(i)) continue;

        auto& node = *body.node(mir, NodeId(i));
        if(!hasResult(node.kind)) continue;

        auto crosses = false;
        for(U32 y = 0; y < live.yields.size(); y++) {
            if(live.yields[y] == i) continue;
            if(LiveAcross::get(live.perYield.pointer() + Size(y) * live.words, live.valueBit(i))) {
                crosses = true;
                break;
            }
        }

        if(!crosses) continue;

        /*
         * And **not** where every one of those hand-overs is emitted before the value exists.
         *
         * The conversion holds one entry per value and fills it as it walks, in block order, so a
         * `yield` in an earlier block than the definition finds nothing to store - which is J51's
         * "the value spill can name a member that is never written", a field no instruction
         * mentions. Asking the question here rather than leaving the field is the same walk the
         * conversion makes, made once and against the record instead of against the emission.
         */
        auto reached = false;
        for(U32 y = 0; y < live.yields.size(); y++) {
            if(live.yields[y] == i) continue;
            if(!LiveAcross::get(live.perYield.pointer() + Size(y) * live.words, live.valueBit(i))) {
                continue;
            }

            if(nodeOrder[live.yields[y]] > nodeOrder[i]) { reached = true; break; }
        }

        if(!reached) continue;

        // The interior positions too - see the payload loop above, where the reason is written.
        auto held = substituteOccurrence(context, program, valueOccurrence(node), with);

        entryCode.push(kIterValueMember | i);
        entryHeld.push(Occurrence { held.type, ownedContract(), held.shape });
        entryShares.push(1);
        entryBit.push(live.valueBit(i));
        entryName.push(StringId());

        auto keeps = entryKeeps.size();
        for(U32 w = 0; w < yieldWords; w++) entryKeeps.push(0);

        for(U32 y = 0; y < live.yields.size(); y++) {
            if(live.yields[y] == i) continue;
            if(LiveAcross::get(live.perYield.pointer() + Size(y) * live.words, live.valueBit(i))) {
                entryKeeps[keeps + y / 64] |= U64(1) << (y % 64);
            }
        }
    }

    /*
     * And the **regions** - `for x in g(...)` and `withLock(m):` written inside the producer, §3.2's
     * first step, one member each holding the child's whole state.
     *
     * A member and not a call, because a `yield` written inside the region suspends the *outer*
     * object with the child suspended inside it: what the next step has to find again is where the
     * child got to, and a record is the only place the outer object has to keep it. That is as true
     * of a lens as of a traversal - `withLock(m): yield x` suspends with the lock held, and the
     * member is what the lens's own state lives in until the cleanup runs. The entry's payload is
     * the `lens.call`'s **node** index, which is what the step conversion matches the call against -
     * a region is not something the producer's body names, so there is no local for it the way there
     * is for everything else in the payload.
     *
     * **It never shares a field**, for a placed member's reason said about state rather than about
     * an allocation: the child is live from the call that initializes it to the exit that cancels
     * it, and those are not points this liveness has bits for.
     *
     * The `across` list is every hand-over written inside the region or inside one nested in it,
     * which is exactly the set of suspensions the child survives - and is what the inspection output
     * prints beside the word `nested`.
     */
    /*
     * The **link** every delegating member holds, minted once for the record - §6.3 and
     * `RecordType::linkType`, where the adjacency it is for is argued.
     *
     * One shape for the whole chain, so a producer whose two delegating regions somehow disagreed
     * about the item type keeps the first and steps the second the ordinary way. Nothing writes
     * that - a level that re-yields hands over exactly what its child handed it - and a record with
     * two link shapes would be a trampoline that could not read its own chain.
     */
    DeclPtr<Type> linkOver = nullptr;

    /*
     * Two producers a delegating level is not built for, and neither is a refusal - the object keeps
     * the form J59 built and costs one indirect call per level, which is what §6.3 states.
     *
     *  - an **owning** hand-over of something with a teardown (`flagged`). `f$take` moves the value
     *    out of *this* record's slot and clears *this* record's `$held`, and what a delegating object
     *    hands over lives in the innermost level's slot - so the consumer would be clearing a flag
     *    one record over from the value it took.
     *  - a **resuming** producer. The trampoline hands what the consumer answered straight to the
     *    innermost level, which is the forwarding the re-yield would have done - but it does so in a
     *    loop, and `Outcome(R, exit)` is a value the consumer owns and may hand over once.
     */
    /*
     * Read off the **declaration** and not out of the substituted continuation result. `<- R` is what
     * turns that result into `Outcome(R, exit)` and `Function::resume` is where it is recorded, so
     * asking whether the wrapper arrived is asking the same question through its own consequence -
     * one substitution and a constructor count to learn what the occurrence already says, and a test
     * that would answer the wrong way the day the bare exit signal stops being an `I32`.
     */
    auto resumes = owner.resume.type != nullptr;

    if(flagged || resumes) {
        for(auto& nest: nests) nest.delegate = false;
    }

    for(auto& nest: nests) {
        if(!nest.delegate) continue;

        if(!linkOver) {
            linkOver = nest.record;
            record->linkType = iterLinkTypeOf(nest.record, source);
            if(!record->linkType) return nullptr;
        }

        if(linkOver != nest.record) nest.delegate = false;
    }

    for(auto& nest: nests) {
        auto& call = (NodeLensCall&)*body.node(mir, NodeId(nest.node));

        auto text = Tritium::format("$%@", context.findName(decl()[call.callee]->name));
        entryName.push(program.internName(context, StringView(text.text(), text.size())));

        /*
         * The child's record, or - where the nest is **boxed** - the erased `iter(Y)` standing in
         * for it. §6.3: a recursive child has no finite record inside its parent's, so the member is
         * the protocol and the child lives in a heap block built where the region is entered.
         */
        entryCode.push(RecordType::kNestMember | nest.node);
        entryHeld.push(owned(nest.delegate ? record->linkType : nest.record));
        entryShares.push(0);
        entryBit.push(live.valueBit(nest.node));

        auto keeps = entryKeeps.size();
        for(U32 w = 0; w < yieldWords; w++) entryKeeps.push(0);

        for(U32 y = 0; y < live.yields.size(); y++) {
            if(!body.encloses(mir, nest.region, nodeRegion[live.yields[y]])) continue;

            entryKeeps[keeps + y / 64] |= U64(1) << (y % 64);
        }
    }

    /*
     * §3.2's third step - two members whose live ranges never meet are **one field**.
     *
     * Greedy over the entries in body order, against the interference `LiveAcross` computes: the
     * first field whose every occupant is disjoint from this entry takes it. Greedy and not an
     * optimal colouring, because the graph here is a handful of nodes and the ordering is the
     * body's - which is the same argument the register allocator's linear scan makes and is what
     * §5's "fifty `yield`s over three locals is four words" needs.
     *
     * **Of one type**, and exactly one: the field is laid out once and the two members are stored
     * and loaded through it under their own occurrences, so a shared field whose occupants disagree
     * about width, about interior positions, or about whether the bytes are the value or a pointer
     * to it would be a store of one shape through a field of another. "Compatible layout" was
     * considered and declined - whether two types happen to manifest alike is the *target's* answer
     * and this is decided once per program, which is the refusal `opt/fold.cpp` makes about a width.
     */
    SmallArray<U8, 64> clash;
    if(entryCode.size() > 1) {
        for(Size i = 0; i < entryCode.size() * entryCode.size(); i++) clash.push(0);
        live.interference(isYield, entryBit.pointer(), U32(entryCode.size()), clash.pointer());
    }

    SmallArray<U16, 16> entryField;
    SmallArray<U32, 16> fieldFirst;

    for(U32 e = 0; e < entryCode.size(); e++) {
        auto into = U16(0xffff);

        for(U32 f = 0; entryShares[e] && f < fieldFirst.size(); f++) {
            auto& owner = entryHeld[fieldFirst[f]];
            if(!entryShares[fieldFirst[f]]) continue;
            if(owner.type != entryHeld[e].type || owner.shape != entryHeld[e].shape) continue;

            // Against every member already in the field, and not only the first: sharing is
            // transitive in the field and the interference is not.
            auto meets = false;
            for(U32 o = 0; o < e && !meets; o++) {
                if(entryField[o] != f) continue;
                meets = clash.size() && clash[Size(o) * entryCode.size() + e];
            }

            if(meets) continue;

            into = U16(f);
            break;
        }

        if(into == 0xffff) {
            into = U16(fieldFirst.size());
            fieldFirst.push(e);
            field(entryName[e], entryHeld[e]);
        }

        entryField.push(into);
        record->payloadLocals.push(program.decl, entryCode[e]);
        record->payloadFields.push(program.decl, U16(record->payloadFirst + into));

        for(U32 w = 0; w < yieldWords; w++) {
            record->payloadYields.push(program.decl, entryKeeps[Size(e) * yieldWords + w]);
        }
    }

    /*
     * What a nested member **borrows**, renumbered onto this record's own origin sets.
     *
     * A child's record borrows what the outer producer lent it - `spansOn.Iter` holds the address of
     * `lines`' own `text` - so the outer record inherits that origin requirement the moment it holds
     * one as a field. `foldRecordSchema` refuses a field whose *type* carries a schema the record
     * never named and accepts one whose *occurrence* names it, which is exactly the statement to
     * make here: the child's positions, prefixed with the field's own index, in the outer record's
     * slot scope.
     *
     * Which outer set is the one the argument came from. A nested traversal is handed the producer's
     * own parameter, and that parameter's capture already published a set (`captureSlot`), so the
     * child's loan of it is a loan of the same storage and belongs in the same set. Anything else -
     * a traversal over the producer's own payload - is §6.1's self-borrow and is refused by name
     * here rather than silently given a set nothing binds.
     *
     * After the field assignment because a member's *field* is not its position since slot sharing;
     * a nested member never shares, so the field is its own, and this is where its index is known.
     */
    /*
     * And which of the nest members are **boxed**, in the order the payload holds them - §6.3, and
     * `RecordType::nestBoxed` is where the step reads it back.
     *
     * One entry per nest whatever its kind, so the list is indexed by "the k-th nest member the
     * payload walk meets" and needs no key of its own. The entry is filled by `completeBoxedNests`
     * rather than here, because the child's record may not exist yet: a mutually recursive pair
     * mints the second inside the first.
     */
    for(U32 n = 0; n < nests.size(); n++) {
        record->nestBoxed.push(program.decl, nullptr);
        record->nestDelegates.push(program.decl, nests[n].delegate ? U8(1) : U8(0));
        if(!nests[n].boxed) continue;

        boxedNests.push(BoxedNest { DeclPtr<RecordType>(record - *program.decl), n, nests[n].boxed,
                                    nests[n].delegate ? linkOver : nests[n].record, source,
                                    nests[n].delegate });
    }

    for(U32 e = 0; e < entryCode.size(); e++) {
        if(!(entryCode[e] & RecordType::kNestMember)) continue;

        auto inside = schemaOf(decl(), entryHeld[e].type);
        if(!inside) continue;

        auto& call = (NodeLensCall&)*body.node(mir, NodeId(entryCode[e] & ~RecordType::kNestMember));
        auto& child = *(RecordType*)decl()[entryHeld[e].type];
        auto& make = *decl()[child.producerNew];
        auto at = U16(record->payloadFirst + entryField[e]);

        SmallArray<ExternalSlot, 4> positions;
        auto held = true;

        for(auto slotPointer: decl()[inside]->slots.contents(decl())) {
            auto& position = *decl()[slotPointer];
            auto wanted = position.contract.originSlot();
            auto lender = kNoOriginSlot;
            auto descriptor = false;

            /*
             * Which argument of the child's constructor publishes the set, and which local of this
             * producer filled it - one walk, because both lists are the same list: `g$new`'s
             * parameters are `g`'s, and the `lens.call`'s operands are what the source wrote.
             */
            auto items = call.arguments.contents(mir);

            for(U32 a = 0; a < make.args.size() && a < items.size(); a++) {
                if(decl()[make.args.get(decl(), a)]->occurrence.contract.originSlot() != wanted) {
                    continue;
                }

                auto& operand = *items.pointerAt(a);
                if(operand.source() != SourceKind::Result) break;

                auto root = argumentRootLocal(body, operand.nodeId(), decl(), mir);
                if(root == kNoLocal) break;

                auto local = body.local(mir, LocalId(root));
                descriptor = local->inlineDescriptor;

                if(local->isParameter() && local->parameter < captureSlot.size()) {
                    lender = captureSlot[local->parameter];
                    break;
                }

                /*
                 * Or the producer's **own** storage, placed in a run of its own - §6.1's `@heap`,
                 * and the one lender that is not a parameter and still does not move.
                 *
                 * The child's borrow is then into an allocation the record points at rather than
                 * into the record, so a move of the object leaves it good. It gets an origin set of
                 * its own, which `f$new` binds to nothing because there is nothing outside the
                 * object for it to bind to: what the set says is that the position refers to
                 * storage, and whose it is, is this record's own answer.
                 */
                if(!local->isParameter() && local->demandsHeap) {
                    lender = record->originSlots++;
                    break;
                }

                /*
                 * Or a local the record itself holds **by address** - a `kBorrowMember`, or a
                 * window whose two words name somebody else's storage.
                 *
                 * The same sentence `@heap` above makes, read off the member's representation
                 * instead of off the binding: what the field holds is where the value is, so the
                 * child's borrow of it is a borrow of storage outside this record and a move of the
                 * object leaves it good. Which storage that is, is what `checkObjectBody` has
                 * already vouched for - a borrow member's lender is a parameter or an `@heap`
                 * binding - so the set here says only that the position refers to storage, exactly
                 * as the `@heap` row above does.
                 *
                 * It is what lets a **mutually recursive** traversal nest its partner at all
                 * (§6.3): `evens` walks `odds(left)` where `left` is a subtree bound out of its own
                 * parameter, and the record holds `left` as a pointer because a `Tree` is not
                 * trivially copyable.
                 */
                if(!local->isParameter()) {
                    auto carried = substituteOccurrence(context, program, local->occurrence, with);

                    if(payloadKindOf(decl(), carried) != IterCaptureKind::Value) {
                        lender = record->originSlots++;
                        break;
                    }
                }

                break;
            }

            if(lender == kNoOriginSlot) {
                /*
                 * Once per written call, though every mint of the producer's object reaches it: the
                 * refusal is about the producer's body, and `objectDropped` and `objectFull` each
                 * minting `outer` printed it twice (J162).
                 */
                auto reported = false;
                for(auto at: objectRefusals) reported |= at == call.source;
                if(!reported) objectRefusals.push(call.source);

                // The same refusal in the two spellings a region has, because what an author has to
                // change differs: a nested traversal walks the storage and a lens is handed it. And
                // a third where the storage is an `@inline` array's descriptor: the author did
                // traverse a parameter, and what the object would hold is the compiler's local.
                if(reported) {
                } else if(descriptor) {
                    context.diagnostics.error(
                        "%@ lends %@ an `@inline(n) @capacity(n)` array as a growable array, and the descriptor that stands for it is a local of the object's step that cannot be held across a `yield` - copy the array into a `[T]` first, or call %@ outside an object"_v,
                        call.source, context.findName(producer.name),
                        context.findName(decl()[call.callee]->name),
                        context.findName(producer.name));
                } else if(child.lensSite) {
                    context.diagnostics.error(
                        "%@ runs %@ over part of its own body and lends it storage the object itself holds, and a record moves with whatever holds it - so the lens's borrow would point at where the value used to be; write `@heap` on the binding, or lend it a parameter instead"_v,
                        call.source, context.findName(producer.name),
                        context.findName(decl()[call.callee]->name));
                } else {
                    context.diagnostics.error(
                        "%@ runs a nested traversal over storage the object itself holds, and a record moves with whatever holds it - so the child's borrow would point at where the value used to be; write `@heap` on the binding, or traverse a parameter instead"_v,
                        call.source, context.findName(producer.name));
                }

                held = false;
                break;
            }

            ExternalSlot moved;
            moved.path.push(program.decl, PathStep::field(at));
            for(auto step: position.path.contents(decl())) moved.path.push(program.decl, step);

            moved.contract = slotContract(position.contract.access(), lender,
                                          position.contract.form());
            positions.push(moved);
        }

        if(!held) return nullptr;

        record->fields.pointer(decl())[at].occurrence.shape =
            program.shapes.intern(positions.pointer(), positions.size());
    }

    /*
     * And a **shared** field is named after all of its occupants - `i+j`, with `#` for a value the
     * producer never named.
     *
     * In the name rather than beside it in the dump, because the name is what every reader already
     * has: the `.ssa` block, `yana explain`'s `object:` row and the MIR record all print a field's
     * name and none of them can reach the producer's locals to look a second one up. §7 stage 3's
     * item 5 asks that the inspection stay correct after sharing, and this is what keeps it so - the
     * `across` list beside it is the union, which the printers compute from `payloadFields`.
     */
    for(U32 f = 0; f < fieldFirst.size(); f++) {
        Tritium::StringBuilder text;
        U32 members = 0;

        for(U32 e = 0; e < entryField.size(); e++) {
            if(entryField[e] != f) continue;

            if(members) text.append("+", 1);
            if(entryName[e] == StringId()) text.append("#", 1);
            else text.append(context.findName(entryName[e]));
            members++;
        }

        if(members < 2) continue;

        record->fields.pointer(decl())[record->payloadFirst + f].name =
            program.internName(context, StringView(text.text(), text.size()));
    }

    // And `$held`, last, where an owning hand-over owes a teardown - see `RecordType::heldField`.
    if(flagged) {
        record->heldField = U16(record->fields.size());
        field(context.addUnqualifiedName("$held", 5), owned(booleanType(program)));
    }

    /*
     * And **delegation's** two words, after it and for its reason: §3.1's field order is `$index`,
     * `$slot`, the captures and the payload, in five walks across two tiers, and a word threaded
     * through the middle of that would renumber all of them to say something only the trampoline
     * reads. `RecordType::downField` and `innerField` are where each one is.
     */
    if(record->linkType) {
        record->downField = U16(record->fields.size());
        field(context.addUnqualifiedName("$down", 5), owned(program.types.words[0]));

        record->innerField = U16(record->fields.size());
        field(context.addUnqualifiedName("$inner", 6), owned(program.types.words[0]));
    }

    RecordCon only;
    only.name = name;
    only.source = producer.source;
    only.count = U16(record->fields.size());
    record->cons.push(program.decl, only);
    record->layout = RecordLayout::Single;

    record->producer = DeclPtr<Function>(&producer - *program.decl);
    decl()[module]->records.push(program.decl, DeclPtr<RecordType>(record - *program.decl));

    /*
     * What the record owes, decided from its members **before** it is classified, because the answer
     * is what decides whether there is an instance for the classification to find.
     *
     * `isLinear` and not `needsTeardown`: an `@explicitDrop` member is an obligation the container
     * inherits, and the step is what discharges it.
     */
    auto owes = false;
    for(auto member: record->fields.contents(decl())) {
        if(!ownsField(member)) continue;
        if(ownershipOf(decl(), member.occurrence.type).isLinear()) owes = true;
    }

    /*
     * And a **run** the record has of its own, which no member's type says anything about: a
     * `@heap`-placed payload member is a pointer field, and a pointer owes nothing by its type while
     * what it reaches is this record's allocation. Forcing the obligation here is also what stops
     * `copy(it)` - two records sharing one run would free it twice - and is the same statement §6.1
     * makes about the cost of the fix: one visible allocation per object.
     */
    if(placed) owes = true;

    /*
     * And a **lens** region the object may be suspended inside, which owes nothing by any member's
     * type and still has to be ended.
     *
     * A lens's cleanup is the construct: `withLock(m): yield x` acquires on the way in and releases
     * on the way out, and what the record holds between the two is a member of three trivially
     * copyable words. Nothing in `ownershipOf` sees a lock in that, so without this an object
     * abandoned inside the region would be dropped by not being dropped at all - no `Drop` instance,
     * no cancellation, and the release never run. Forcing the obligation is also what stops
     * `copy(it)`, which is the right answer for the same reason `@heap` above is: two records with
     * one open scope between them would release it twice.
     *
     * Only a **lens** member is decided here. A nested traversal's cancellation is observable for the
     * same reason and is not a fact about the fields at all, so it is the walk below - one question
     * the lens's row is the constant-true answer to.
     */
    for(auto& nest: nests) {
        /*
         * And a **boxed** child, which is a heap block this record is the only owner of - §6.3.
         *
         * Forced here rather than left to the field's own answer for the reason `@heap` above is
         * forced: the allocation is the record's, so the record owes the free whatever its other
         * members are, and `copy(it)` of two records sharing one block would hand it back twice.
         */
        if(nest.boxed) { owes = true; continue; }
        if(nest.record && decl()[nest.record]->kind != Type::Record) continue;
        if(nest.record && ((RecordType*)decl()[nest.record])->lensSite) owes = true;
    }

    /*
     * And whether the producer's own **departure path** does anything - `departureHasEffect`, and
     * J56 §4's hole closed.
     *
     * "A consumer that leaves ends the iterator's loops, so the statements below them are cleanup
     * and run on both paths" is the internal form's contract, and an object owing nothing had no
     * teardown to run them from. A walk rather than a field, because what it asks is about the body:
     * the members of `noisy.Iter` are two integers and its cleanup writes a global.
     *
     * It is asked last because it is the only one of the four that can be false where a member says
     * true, and asking it at all costs a forward walk over the blocks a departure reaches - nothing,
     * on a producer with no departure to take.
     */
    if(!owes && departureHasEffect(mir, body, isYield)) owes = true;

    auto make = iterConstruction(producer, *record);
    auto step = iterStep(producer, *record);
    if(!make || !step) return nullptr;

    record->producerNew = make;
    record->producerStep = step;

    // And the trampoline, where any of the regions delegates - `iterWalk`, which is what every
    // consumer of this record steps through instead of `f$step`.
    if(record->linkType) {
        record->producerWalk = iterWalk(producer, *record, handover);
        if(!record->producerWalk) return nullptr;
    }

    if(owes && !iterTeardown(producer, *record, source)) return nullptr;
    if((lends || record->linkType) && !iterItem(producer, *record, handover, source)) return nullptr;
    if(flagged && !iterTake(producer, *record, handover)) return nullptr;

    /*
     * `Iterate`, for an iterator and not for a **lens**.
     *
     * The class is what a body generic in its traversal steps one through, and nothing iterates a
     * lens: a lens's record is minted only for a region a producer runs over part of its own body,
     * where the one reader is that producer's own step. An instance here would be `next`/`resume`
     * over a record whose second step is a cleanup, which is a reading of the class no consumer
     * should be able to reach.
     */
    if(!lensSite && !iterIterateInstance(producer, *record, handover, source)) return nullptr;

    breakRecordLayout(context, program, *record);
    if(!foldRecordSchema(context, program, *record)) return nullptr;
    if(!classifyRecord(context, program, *record)) return nullptr;

    iterSites.push(IterSite { DeclPtr<Function>(&producer - *program.decl), recordType, make, step });
    return recordType;
}

/*
 * The three functions a site type needs, none of which has a body here.
 *
 * `f$new` is the construction: a record with its captured arguments in it and its payload untouched,
 * which is a shape MIR has no write for - every write it has initializes something, and a record of
 * half-initialized members is what the move paths would then have to describe. `f$step` is the
 * producer's own body read a second time, so there is no body to build. `f$drop` is one call of the
 * step. All three are `Function::Generated`, and `resolve_ssa/convert.cpp` is where they are filled.
 */
DeclPtr<Function> Resolver::iterGenerated(Function& producer, RecordType& record, StringView suffix,
                                          Function::Generated kind) {
    return generatedFor(producer.module, producer.symbol, suffix, kind,
                        asBase<Type>(DeclPtr<RecordType>(&record - *program.decl)),
                        DeclPtr<Function>(&producer - *program.decl), producer.source);
}

/*
 * One generated function, named after whatever it was made out of.
 *
 * Split out of `iterGenerated` because the **erased** form has no producer to be named after: one
 * `iter(I32)` serves every `iter fn` in the program, so its two steppers are named after the type
 * and hang off whichever module minted it first. Everything else about the two is the same three
 * lines, which is why they are one function rather than two that have to be kept in step.
 */
DeclPtr<Function> Resolver::generatedFor(DeclPtr<Module> module, StringId base, StringView suffix,
                                         Function::Generated kind, DeclPtr<Type> record,
                                         DeclPtr<Function> from, LocationId source) {
    auto text = Tritium::format("%@%@", context.findName(base), suffix);
    auto symbol = program.uniqueSymbol(module, context, StringView(text.text(), text.size()));

    auto made = new (program.decl) Function(symbol, module);
    made->source = source;
    made->summary.bodyKnown = true;
    made->bodyState = Function::BodyState::Read;
    made->generated = kind;

    /*
     * **`f$new` is inlinable again**, and the rule that made it look otherwise was somebody else's.
     *
     * J51 marked it `@noinline` after `test/resolve/iter/Capture.yana`'s window capture came out
     * with the producer's own counter never initialized, and read the cause as a half-initialized
     * record being forwarded field-wise across an inlined call boundary. That reading was wrong.
     * The record *is* deliberately half-initialized - the captured arguments are stored and the
     * payload is left as it was, because §2.6 says a call runs none of the producer - and nothing in
     * the memory passes ever answered a read of an unwritten member: `forwardMemory` forwards only
     * out of a store it can see, and a member with none is a load that stays a load.
     *
     * What actually miscompiled was `placeRuns`: its carrier walk taints, through storage, every
     * value a read of a slot the run pointer was stored into can answer with - and a record store of
     * a captured window taints the record whole, so `$index` became a "carrier" and the step's
     * `eq($index, 0)` read as the allocator's refusal test and folded to `False`. The fresh arm of
     * the dispatch then became unreachable, which is why the answer was the accumulator's initial
     * value and why a `print` inside the loop made it go away. `opt/place.cpp`'s `reloaded` is the
     * fix and `iter/Inline.yana` is the fixture; J52 §3 is the account.
     *
     * So the constructor is an ordinary body: the call per construction J51 paid is gone, and
     * `Inline.yana.ssa.opt.expect` is what says the optimizer expands it.
     */
    made->generatedRecord = record;
    made->generatedFrom = from;

    auto pointer = DeclPtr<Function>(made - *program.decl);
    decl()[module]->functions.push(program.decl, pointer);
    return pointer;
}

DeclPtr<Function> Resolver::iterConstruction(Function& producer, RecordType& record) {
    auto& owner = bodyOwnerOf(decl(), producer);
    auto with = substitutionOf(decl(), producer);
    auto pointer = iterGenerated(producer, record, "$new"_v, Function::Generated::IterNew);
    auto make = decl()[pointer];

    /*
     * The parameters are the producer's, under the conventions the *record* holds them by - which is
     * the whole of the borrowing. A lent one publishes an origin slot and the result binds it, so the
     * loan analysis roots the record in each argument's loan by the rule it applies to any `'T`
     * result, and the lent locals stay borrowed for as long as the object is live.
     */
    SmallArray<ExternalSlot, 4> published;

    for(U32 i = 0; i < owner.writtenArity(); i++) {
        auto& parameter = *decl()[owner.args.get(decl(), i)];
        auto substituted = substituteOccurrence(context, program, parameter.occurrence, with);
        auto contract = substitutedContract(decl(), program, parameter.occurrence.type,
                                            substituted.type, substituted.contract);

        auto declared = new (program.decl) Arg();
        declared->name = parameter.name;
        declared->source = parameter.source;
        declared->index = i;

        auto kind = IterCaptureKind(record.captureKinds.get(decl(), i));

        if(kind == IterCaptureKind::Value) {
            declared->occurrence = owned(substituted.type);
        } else {
            auto slot = U16(published.size());
            declared->occurrence = Occurrence { substituted.type,
                                                slotContract(contract.access(), slot, contract.form()),
                                                substituted.shape };

            /*
             * Where in the record the loan sits, read off **the field the mint made** rather than
             * restated: an address capture publishes its pointee and a window publishes the pointer
             * inside its descriptor, and the two have to agree to the step with the record's own
             * schema or `foldRecordSchema` reports the disagreement. Prefixing the field's own
             * positions with its index is that agreement rather than a second reading of it.
             */
            auto captured = record.field(decl(), U16(2 + i));

            if(kind == IterCaptureKind::View) {
                for(auto slotPointer: decl()[captured.occurrence.shape]->slots.contents(decl())) {
                    auto& inside = *decl()[slotPointer];

                    ExternalSlot position;
                    position.path.push(program.decl, PathStep::field(U16(2 + i)));
                    for(Size step = 0; step < inside.path.size(); step++) {
                        position.path.push(program.decl, inside.path.get(decl(), step));
                    }

                    position.contract = slotContract(contract.access(), slot);
                    published.push(position);
                }
            } else {
                ExternalSlot position;
                position.path.push(program.decl, PathStep::field(U16(2 + i)));
                position.path.push(program.decl, PathStep::pointee());
                position.contract = slotContract(contract.access(), slot);
                published.push(position);
            }
        }

        make->args.push(program.decl, declared - *program.decl);
    }

    DeclPtr<OwnershipSchema> shape = nullptr;
    if(published.size()) shape = program.shapes.intern(published.pointer(), published.size());

    make->result = Occurrence { asBase<Type>(DeclPtr<RecordType>(&record - *program.decl)),
                                ownedContract(), shape };
    make->originSlots = U16(published.size());
    return pointer;
}

/*
 * `{$pair: iter(Y), $up: %Word}` - what a **delegating** nest member holds, Design-Iterator-Objects.md
 * §6.3, built 2026-09-16.
 *
 * Two words of pair and one of chain, **adjacent and in that order**, because that adjacency is the
 * whole of the data structure delegation needs. A trampoline that has just been told "this level
 * entered a child" is told it as the address of one of these, and from that one address it has to
 * find both the pair it will step next and the level it goes back to when that pair runs out.
 *
 * A stack the root grows was the alternative and was declined for the reason the levels make
 * unavoidable: a cycle's levels are several record types of several sizes (`evens.Iter` and
 * `odds.Iter` are not the same record), so a root that kept the chain itself would be keeping an
 * array of pairs it has to grow - an allocation and a library role inside a generated body - where
 * the link word is one word in storage the level above it already owns and already frees.
 *
 * Interned as a tuple, which is what makes one shape serve every level: the item type is the same
 * all the way down a delegating chain, because a level that delegates re-yields what its child
 * handed it.
 */
DeclPtr<Type> Resolver::iterLinkTypeOf(DeclPtr<Type> erased, LocationId source) {
    RecordField fields[2];

    fields[0].name = context.addUnqualifiedName("$pair", 5);
    fields[0].occurrence = owned(erased);
    fields[0].source = source;
    fields[0].index = 0;

    fields[1].name = context.addUnqualifiedName("$up", 3);
    fields[1].occurrence = owned(program.types.words[0]);
    fields[1].source = source;
    fields[1].index = 1;

    return tupleType(context, program, fields, 2, source);
}

/*
 * `f$walk` - the **trampoline** every consumer of a delegating object steps through, §6.3.
 *
 *      fn f$walk($self: &f.Iter, $resume: S, $cancel: Bool) -> %Y
 *
 * The step's own three arguments, and an answer that is the *erased* step's rather than the
 * concrete one's: what comes back is where the item is, because the item a delegating object hands
 * over belongs to whichever level produced it and is not in this record's slot at all. That is also
 * what makes `f$erased` over a delegating record one call rather than a second adaptation - the two
 * signatures already agree.
 *
 * A function of its own rather than a branch inside `f$step`, and the reason is that a level is both
 * a root and a child: `f$step` is what a *parent* enters, and a parent that ran the trampoline would
 * keep a second chain of its own. One function answers "advance this level", the other answers
 * "advance this object", and only the consumer ever asks the second.
 */
DeclPtr<Function> Resolver::iterWalk(Function& producer, RecordType& record,
                                     const Occurrence& handover) {
    auto pointer = iterGenerated(producer, record, "$walk"_v, Function::Generated::IterWalk);
    auto walk = decl()[pointer];
    auto& step = *decl()[record.producerStep];
    auto recordType = asBase<Type>(DeclPtr<RecordType>(&record - *program.decl));

    for(U32 i = 0; i < 3; i++) {
        auto& taken = *decl()[step.args.get(decl(), i)];

        auto declared = new (program.decl) Arg();
        declared->name = taken.name;
        declared->source = taken.source;
        declared->index = i;
        declared->occurrence = i == 0 ? borrowed(recordType, Access::Unique) : taken.occurrence;
        walk->args.push(program.decl, declared - *program.decl);
    }

    walk->result = owned(program.types.pointerType(handover.type));
    return pointer;
}

DeclPtr<Function> Resolver::iterStep(Function& producer, RecordType& record) {
    auto& owner = bodyOwnerOf(decl(), producer);
    auto with = substitutionOf(decl(), producer);
    auto pointer = iterGenerated(producer, record, "$step"_v, Function::Generated::IterStep);
    auto step = decl()[pointer];
    auto recordType = asBase<Type>(DeclPtr<RecordType>(&record - *program.decl));

    auto self = new (program.decl) Arg();
    self->name = context.addUnqualifiedName("$self", 5);
    self->source = producer.source;
    self->index = 0;
    self->occurrence = borrowed(recordType, Access::Unique);
    step->args.push(program.decl, self - *program.decl);

    /*
     * The step's second argument is **what the continuation answered**, at the type J49 gave it -
     * the exit signal, or `Outcome(R, exit)` where the producer declared `<- R`.
     *
     * The same value one tier over, which is what lets one function serve `next()`, `resume(v)` and
     * the teardown: the producer's own `yield` reads it exactly as it reads a continuation's return,
     * so nothing in the body has a second reading for the object form.
     */
    auto resume = new (program.decl) Arg();
    resume->name = context.addUnqualifiedName("$resume", 7);
    resume->source = producer.source;
    resume->index = 1;
    resume->occurrence = owned(substituteType(context, program, owner.continuationResult.type, with));
    step->args.push(program.decl, resume - *program.decl);

    /*
     * And whether this is a **cancellation**, which the answer cannot carry.
     *
     * A record at index 0 has run none of the producer's body (§2.6), so entering it to cancel has
     * to release the captured arguments and stop rather than start. A parameter rather than a
     * reserved index, because the index says where the producer is and this says what is being
     * asked of it.
     */
    auto cancel = new (program.decl) Arg();
    cancel->name = context.addUnqualifiedName("$cancel", 7);
    cancel->source = producer.source;
    cancel->index = 2;
    cancel->occurrence = owned(booleanType(program));
    step->args.push(program.decl, cancel - *program.decl);

    /*
     * And the answer is whether the slot now holds something - **or**, for a delegating producer,
     * whether it has entered a child, which is the same `True` with `$down` set beside it (§6.3).
     * Two rows in this channel and a third one over, which is what made delegation free for every
     * producer that does not use it.
     */
    step->result = owned(booleanType(program));
    return pointer;
}

/*
 * `instance Drop(f.Iter)`, where the record owes anything - §3.3's teardown by index.
 *
 * The member steps the record with `$cancel`, and that is the whole of it: at a suspend index the
 * producer's own departure path runs - `emitDeparture` out of the loops, the statements below them,
 * and drop elaboration releasing every local that is still live - and at index 0 the step releases
 * the captured arguments and stops. So the design's "the teardown is shared across indices" is not
 * something built here; it is what converting one body once already does.
 *
 * An instance rather than a field on the record, so that every walk that already asks what a type
 * owes - `classifyRecord`, `analyze_drop.cpp`, `reachTeardown`, `Lowering::emitTeardown` - gets the
 * answer through the one question it already asks.
 */
bool Resolver::iterTeardown(Function& producer, RecordType& record, LocationId source) {
    auto klass = program.roles.klass(intrinsic::Known::drop);
    if(!klass) {
        reportMissingRole(intrinsic::Known::drop, "an iterator object"_v, source);
        return false;
    }

    auto recordType = asBase<Type>(DeclPtr<RecordType>(&record - *program.decl));
    auto pointer = iterGenerated(producer, record, "$drop"_v, Function::Generated::IterDrop);
    auto drop = decl()[pointer];

    auto self = new (program.decl) Arg();
    self->name = context.addUnqualifiedName("$self", 5);
    self->source = producer.source;
    self->index = 0;
    self->occurrence = owned(recordType);
    drop->args.push(program.decl, self - *program.decl);

    drop->result = owned(program.types.unit);
    drop->teardown = true;

    auto instance = new (program.decl) Instance();
    instance->klass = klass;
    instance->args.push(program.decl, recordType);
    instance->members.push(program.decl, pointer);
    instance->module = producer.module;
    instance->source = producer.source;

    auto made = DeclPtr<Instance>(instance - *program.decl);
    decl()[producer.module]->instances.push(program.decl, made);

    if(!program.addInstance(decl(), made)) {
        context.diagnostics.error("internal: an iterator object's teardown collided with an instance"_v,
                                  source);
        return false;
    }

    return true;
}

/*
 * `let it = f(...)` - the object, built where a call would have been.
 *
 * The arguments arrived exactly as they would have for a call of the producer: resolved against its
 * declared parameters, prepared under the modes their contracts asked for, with the type arguments
 * bound by what was passed. What happens to them is this - one call of `f$new`, whose signature is
 * where the borrowing is stated, and that is the whole of the call site.
 */
Node* Resolver::constructIterObject(Builder& builder, Function& callee,
                                    SmallArray<Node*, 8>& arguments, TypeArguments& typeArguments,
                                    LocationId source) {
    typeArguments.fillDetermined(source);

    if(typeArguments.reportUndecided(source, "argument"_v,
                                     Tritium::format("call to %@", context.findName(callee.name)))) {
        return nullptr;
    }

    if(!dischargeRequirements(callee, typeArguments, source)) return nullptr;

    auto with = typeArguments.with();

    /*
     * A construction inside a **generic body** - §6.5's neighbour, and refused for the same reason.
     *
     * A site type has no parameters: it is minted from one producer read under one substitution, and
     * a body compiled once has no substitution to mint it under. Specializing is always available
     * and is what `Auto` chooses, so nothing that compiled stops compiling.
     */
    for(Size i = 0; i < with.count; i++) {
        if(with.arguments[i] && isConcreteType(decl(), with.arguments[i])) continue;

        /*
         * A **class member** at an open subject - §6.4's witness entry, built 2026-09-16 (J61).
         *
         * What a call to an `iter fn` produces is a record minted per instance, and a body with an
         * open subject has no instance and therefore no record. What it gets instead is the erased
         * form: the call goes to the member's **"make an object" entry** - one more cell on the
         * class's witness, whose contents are a function over the selected instance's own record -
         * and what comes back is the two words every erased object is.
         *
         * The refusal that stood here named the entry as not built and advised `-> iter(T)` or
         * `@specialize`. Both still work and neither is needed.
         *
         * **The protocol still has to be closed.** `iter(T)` is interned on what it hands over and
         * the instance that steps one is generated at that protocol, so a member whose item type is
         * still a variable after this substitution has nothing to step it - `Chunked(c, a)` at an
         * open `a` is that case and `Chunked(c, Int)` is not. Refused by name below, on the same
         * terms `resolveErasedIterType` refuses a written one.
         */
        if(callee.klass && callee.iterator) {
            auto handover = substituteOccurrence(context, program, callee.handover, with);
            auto resume = callee.resume.type
                ? substituteType(context, program, callee.resume.type, with) : nullptr;

            if(genContext && (mentionsContext(decl(), handover.type, genContext) ||
                              mentionsContext(decl(), resume, genContext))) {
                context.diagnostics.error(
                    "%@ hands over %@ here, and an iterator object's type is interned on the protocol it steps - so a declaration cannot hold one whose item is a type variable it binds. Bind the class's item parameter to a type, as in `Chunked(c, Int)`"_v,
                    source, context.findName(callee.name),
                    typeText(context, decl(), handover.type));
                return nullptr;
            }

            if(!erasedIterTypeOf(handover, resume, source)) return nullptr;

            auto entry = iterObjectMember(callee);
            if(!entry) return nullptr;

            return builder.addCall(source, *decl()[entry], arguments.pointer(), arguments.size(),
                                   with);
        }

        context.diagnostics.error(
            "%@'s object type is minted per set of type arguments, so one cannot be built inside a body compiled at a type variable - write `@specialize` on this function"_v,
            source, context.findName(callee.name));
        return nullptr;
    }

    /*
     * A **class member** at a concrete subject, selected before anything is minted - §6.4.
     *
     * The declaration the call names is the class's, which is a signature and not a body; what an
     * object of it would step is the instance's. So selection happens here, exactly as
     * `collectIterNests` already does it for a nested `for x in chunks(self)`, and the record minted
     * is the *instance's* - which is why a concrete `let it = chunks(xs)` keeps the instance
     * function's own site type rather than an erased pair. The refusal that used to stand here is
     * now `iterSiteTypeOf`'s, reached only where the subject is a variable and the body is compiled
     * once.
     */
    auto producer = DeclPtr<Function>(&callee - *program.decl);
    auto producerWith = with;

    if(callee.klass) {
        SmallArray<DeclPtr<Type>, 4> classArguments;
        for(Size i = 0; i < with.count; i++) classArguments.push(with.arguments[i]);

        InstanceMatch found;
        if(!selectInstance(context, program, callee.klass, classArguments.pointer(),
                           classArguments.size(), source, true, found)) {
            return nullptr;
        }

        auto selected = instanceMember(context, program, found, callee.classMember, source);
        if(!selected) return nullptr;

        producer = DeclPtr<Function>(&bodyOwnerOf(decl(), *decl()[selected]) - *program.decl);
        producerWith = substitutionOf(decl(), *decl()[selected]);
    }

    auto site = iterSiteTypeOf(*decl()[producer], producerWith, source);
    if(!site) return nullptr;

    auto& record = *(RecordType*)decl()[site];

    DeclPtr<Function> make = nullptr;
    for(auto& known: iterSites) {
        if(known.record == site) make = known.make;
    }

    if(!make) return nullptr;

    /*
     * A **temporary** at a borrowing parameter, which an object cannot take.
     *
     * A normal call lends what the caller made for the duration of the statement; an object outlives
     * it, so the record would hold the address of storage that is already gone. The stack handle
     * keeps such a value instead (`constructIter`'s `producerArguments`), and a record could too -
     * a capture that owns where the parameter borrows is a second `IterCaptureKind`, and it is not
     * built.
     *
     * **A backstop rather than a reachable message today**, and the reason is worth keeping: the only
     * capture held by address is an exclusive one, and `&` at a call refuses a value that is not a
     * writable name long before this. It is here because that is a property of the *capture kinds*,
     * and the day a shared loan of an addressed value becomes one is the day a temporary reaches it.
     */
    for(U32 i = 0; i < arguments.size(); i++) {
        if(!arguments[i]) return nullptr;
        if(IterCaptureKind(record.captureKinds.get(decl(), i)) != IterCaptureKind::Address) continue;
        if(!valueOccurrence(*arguments[i]).contract.isOwned()) continue;

        auto& parameter = *decl()[callee.args.get(decl(), i)];
        context.diagnostics.error(
            "%@ borrows %@, and an object outlives the statement that built it - bind this argument to a name first"_v,
            source, context.findName(callee.name), context.findName(parameter.name));
        return nullptr;
    }

    Substitution none;
    return builder.addCall(source, *decl()[make], arguments.pointer(), arguments.size(), none);
}

/*
 * One step of a record the consumer holds - the call `next()`, `resume(v)` and a `for` all make.
 *
 * `$resume` is what the continuation answered, `$cancel` says whether the producer is being asked to
 * stop rather than to carry on, and the answer is whether the slot now holds something.
 *
 * Through `f$walk` where the record **delegates** (§6.3), which changes both halves of that last
 * clause: a delegating object's step may answer "I have entered a child", which only the trampoline
 * knows what to do with, and the item it hands back belongs to whichever level produced it - so what
 * comes back is *where the item is* rather than a `Bool`, which is the erased form's own answer.
 */
Node* Resolver::stepIterObject(Builder& builder, Place& object, RecordType& site, Node* resume,
                               bool cancel, LocationId source) {
    /*
     * The record itself, read as the step's own `&self` position asks - and `requestOf` rather than
     * `bindingRequest`, which is the whole of the difference: a `let`'s request publishes nothing, so
     * a record whose members are all trivially copyable would be **copied** into the call and the
     * step would advance a duplicate. `Origin::CallBounded` is what refuses that copy, and it is
     * what the parameter's own declaration carries.
     */
    /*
     * And through the **trampoline** where the record has one - §6.3. A delegating object's step may
     * answer "I entered a child", which only `f$walk` knows what to do with; every consumer of one
     * therefore goes through it, and what comes back is where the item is rather than a `Bool`.
     */
    auto target = site.producerWalk ? site.producerWalk : site.producerStep;

    auto& declared = *decl()[decl()[target]->args.get(decl(), 0)];
    auto self = readPlace(builder, source, object, requestOf(declared.occurrence));
    if(!self) return nullptr;

    if(!resume) {
        resume = builder.addConst(source, intConstant(program.types.exitCode(), U64(kIterFallOut)));
    }

    auto stop = builder.addConst(source, truthConstant(program, cancel));
    if(!resume || !stop) return nullptr;

    Node* arguments[3] = { self, resume, stop };

    Substitution none;
    return builder.addCall(source, *decl()[target], arguments, 3, none);
}

/*
 * Whether this expression is a **name, or a path of fields over one** - the two shapes a `for` may
 * ask about without building anything.
 *
 * Syntactic on purpose. `resolveObjectFor` has to answer "is this a held object" before the ordinary
 * traversal walk gets its turn, and the only honest way to ask is to resolve the source - which for
 * a subscript is a call of `get`, emitted into a block this walk may then decline to build. A name
 * and a field path over one reach `resolvePlace`'s two cheap branches and emit nothing, and they are
 * the two positions §2.1 names: a local, a parameter, and a field of either.
 */
static bool namesStorage(ast::ParseBase parse, ast::Expr& expr) {
    if(expr.kind == ast::Expr::Nested) return namesStorage(parse, *parse[expr.nested]);
    if(expr.kind == ast::Expr::Field) return namesStorage(parse, parse[expr.field]->target);

    return expr.kind == ast::Expr::Var;
}

/*
 * `for x in it` over a record the author holds - §2.1's fourth row.
 *
 *    header:  if !f$step(&it, fallOut, False) -> exit
 *             let x = <the slot, under the producer's own hand-over convention>
 *             ...body...          continue -> header, break -> exit
 *             jump header
 *    exit:
 *
 * An ordinary loop in the consumer's own region: the producer's frame is the record, which is here,
 * so nothing is a continuation and `break` and `continue` are the edges `while` gives them.
 *
 * **Leaving does not finish the object.** The record is the author's, so a `break` leaves it
 * suspended exactly where it was and a second `for` carries on from the index it left - which is
 * what stepping by hand means, and is the same statement `resolveExternalFor` makes about a handle
 * the author holds.
 *
 * Built directly rather than through `Iterate`, because a concrete site type knows the producer's
 * declared convention: a generic body always borrows the item out of the slot, and here the loop
 * variable **moves out of it** where the producer wrote `-> ->T`.
 *
 * Answers false, having emitted nothing, where the source is not such a record.
 */
bool Resolver::resolveObjectFor(Builder& builder, ast::Expr& expr, ast::ForExpr& loop) {
    if(!namesStorage(parse, loop.from)) return false;

    /*
     * The object, wherever it is held - a local, a parameter, or a field of either.
     *
     * `resolvePlace` and not `findLocal`, which is what J54 left for this stage: `for x in p.lexer`
     * reported "no rule finds one from the type of a value" and the fixture wrote a `let` first.
     * What decides which loan the loop takes is the same thing that decides it for a `let`-bound
     * one - `stepIterObject` reads the step's own `&self` position - so the place is asked for
     * exclusively and a field of a shared record is refused there rather than here.
     *
     * `namesStorage` above is what keeps the widening from costing anything: a subscript accessor is
     * a *call*, and resolving one to find out whether its element happens to be a site type would
     * emit the call into a block this walk is about to decline to build. A name and a path of fields
     * over one are the two shapes that cost nothing to ask about, and they are the two the design
     * names - `for x in <field>` and `for x in <argument>`.
     */
    auto held = resolvePlace(builder, loop.from, false, Access::Unique);
    if(!held) return false;

    // `for x in it` over an **erased** object - the same loop through the pair rather than through a
    // record the consumer can see into. Its own function because every line below reads a site type.
    if(held->occurrence.type && decl()[held->occurrence.type]->kind == Type::Iter) {
        return resolveErasedFor(builder, expr, loop, *held,
                                *(IterType*)decl()[held->occurrence.type]);
    }

    auto record = recordDeclarationOf(decl(), held->occurrence.type);
    if(!record) return false;

    auto& site = *(RecordType*)decl()[record];
    if(!site.producer || !site.producerStep) return false;

    auto source = expr.source;
    auto& producer = *decl()[site.producer];

    /*
     * The declaration the hand-over and the resume channel are written on, which is the **generic**
     * where the producer is a specialization - `bodyOwnerOf`, the same reading `mintIterSite` makes.
     *
     * A clone carries its parameters and its result and nothing else, so reading `handover` off the
     * specialization answered an empty occurrence and every question below it - does this lend, is
     * there a value to carry - was answered about nothing.
     */
    auto& declaring = bodyOwnerOf(decl(), producer);

    if(loop.alts.size()) {
        context.diagnostics.error(
            "an iterator object cannot fail, so there is nothing for an alternative to handle"_v,
            loop.pat.source);
        return true;
    }

    auto place = held;

    auto with = substitutionOf(decl(), producer);
    auto handover = substituteOccurrence(context, program, declaring.handover, with);
    auto lends = objectLends(decl(), handover);

    /*
     * A **resuming** producer, whose passes owe it a value - §2.5's last row.
     *
     * The step's own `$resume` parameter is what travels: `Outcome(R, exit)` where the producer
     * declared `<- R`, and the bare exit signal everywhere else. So the header takes it as its
     * first block argument, the entry edge supplies the **fall-out** row - which is what "the first
     * pass calls `next`" means - and each `continue v` supplies `Proceed(v)` on the back edge.
     * `break` supplies nothing, because it goes to the exit and leaves the object suspended.
     *
     * A block argument rather than a slot in the record, because it is the *consumer's* answer and
     * the record is the producer's: two `for`s over one object answer it independently, and a value
     * held across the step would be a second thing a `break` had to unwind.
     */
    auto sent = decl()[decl()[site.producerStep]->args.get(decl(), 1)]->occurrence;

    auto resumes = decl()[sent.type]->kind == Type::Record &&
                   ((RecordType*)decl()[sent.type])->cons.size() > 1;

    auto resume = resumes ? substituteOccurrence(context, program, declaring.resume, with)
                          : Occurrence();

    // Which constructor of `Outcome(R, exit)` carries what - by what each one holds rather than by
    // its position, which is the reading `emitIterStepper` makes of the same wrapper.
    U16 proceedCon = 0;
    U16 exitCon = 0;

    if(resumes) {
        auto& wrapper = *(RecordType*)decl()[sent.type];
        proceedCon = wrapper.cons.get(decl(), 0).index;
        exitCon = wrapper.cons.get(decl(), 1).index;
    }

    // `Outcome.Exit(0)`, which is the row a pass that asked for an element arrives on - the same
    // constant `stepIterObject` passes a non-resuming producer directly.
    auto fallOut = [&]() -> Node* {
        auto row = builder.addConst(source, intConstant(program.types.exitCode(), U64(kIterFallOut)));
        if(!row) return nullptr;

        Node* only = row;
        return builder.addAggregate(source, sent.type, exitCon, &only, 1);
    };

    /*
     * Entering a `for` over an object that is **already suspended at a `yield` asking for a value**,
     * which is the one thing the first pass cannot do.
     *
     * The first pass has run no body, so it has nothing to answer with - which is exactly what
     * `it.next()` is refused for, and this is the same refusal at the spelling that cannot be
     * decided at compile time: the index is a value. The test is `emitIterStepper`'s, written out
     * here for the same three rows - index zero is a fresh object, the finished index is one that
     * ran out, and everything between them is a producer waiting for an answer.
     *
     * Silently skipped where the library declares no condition function, which is the role's own
     * `Optional`: a build with no checks has no place to put this one either.
     */
    if(resumes && program.roles.function(intrinsic::Known::checkCondition)) {
        auto counter = program.types.exitCode();
        auto index = readPlace(builder, source, *builder.fieldPlace(*place, 0), inspectRequest());
        auto fresh = builder.addConst(source, intConstant(counter, U64(0)));
        auto done = builder.addConst(source, intConstant(counter, kIterFinished));

        if(index && fresh && done) {
            auto started = builder.addBinary(source, BinaryOp::Ne, *index, *fresh);
            auto running = builder.addBinary(source, BinaryOp::Ne, *index, *done);
            auto no = builder.addConst(source, truthConstant(program, false));

            // A `select` rather than a bitwise `and`, for `emitIterStepper`'s reason: the
            // conjunction is over two truths and `OpCategory::Bits` is about integers.
            if(started && running && no) {
                if(auto suspended = builder.addSelect(source, *started, *running, *no)) {
                    callKnown(builder, intrinsic::Known::checkCondition, &suspended, 1, source);
                }
            }
        }
    }

    Carry carry(*this, source);

    auto header = builder.addBlock(StringId());
    auto exit = builder.addBlock(StringId());

    if(resumes) builder.addBlockArgument(*header, owned(sent.type), source);

    carry.declare(builder, *header);
    carry.declare(builder, *exit);

    SmallArray<Operand, 8> entry;

    if(resumes) {
        auto first = fallOut();
        if(!first) return true;
        entry.push(builder.resultOf(*first, argumentMode(ownedContract(), SourceKind::Result)));
    }

    carry.edge(builder, entry);
    builder.addJump(source, *header, entry.pointer(), entry.size());

    builder.setBlock(*header);

    Node* answered = nullptr;
    if(resumes) answered = builder.base()[header->arguments.get(builder.base(), 0)];

    carry.arrive(builder, *header, resumes ? 1 : 0);

    auto scope = bindings.size();

    auto step = [&]() -> bool {
        auto more = stepIterObject(builder, *place, site, answered, false, source);
        if(!more) return false;

        /*
         * A **delegating** object answers where the item is - §6.3, and this loop is then
         * `resolveErasedFor`'s with a record in hand instead of a pair: the address crosses the
         * branch in a frame slot, and `f$item` roots the loan in the object exactly as it does for a
         * lending producer, with the address passed in rather than read out of a field.
         */
        if(site.producerWalk) {
            auto number = builder.addBitcast(source, *more, program.types.words[0]);
            auto none = builder.addConst(source, intConstant(program.types.words[0], 0));
            if(!number || !none) return false;

            auto answeredItem = builder.addBinary(source, BinaryOp::Ne, *number, *none);
            if(!answeredItem) return false;

            auto address = hold(builder, *more, source);
            if(!address) return false;

            failIfNot(builder, *answeredItem, *exit, carry, source);

            auto found = readPlace(builder, source, *address, inspectRequest());
            if(!found) return false;

            auto& borrow = *decl()[site.producerItem];
            auto& itemSelf = *decl()[borrow.args.get(decl(), 0)];

            Node* held[2] = { readPlace(builder, source, *place, requestOf(itemSelf.occurrence)),
                              found };
            if(!held[0]) return false;

            auto value = callFunction(builder, borrow, held, 2, source);
            if(!value) return false;

            auto delegated = loop.pat.kind == ast::Pat::Var;
            auto bound = builder.addLocal(delegated ? loop.pat.var : StringId(),
                                          valueOccurrence(*value), loop.pat.source, false);
            builder.addWrite(loop.pat.source, *builder.localPlace(*bound), *value,
                             Destination::Init);

            if(delegated) {
                bindLocal(loop.pat.var, bound);
            } else if(loop.pat.kind != ast::Pat::Any &&
                      !bindIrrefutable(builder, loop.pat, *builder.localPlace(*bound),
                                       "a `for` binding"_v)) {
                return false;
            }

            return true;
        }

        failIfNot(builder, *more, *exit, carry, source);

        /*
         * The item, out of the slot under the producer's own convention.
         *
         * A **lending** producer's slot holds the value's address, so the loop variable is a borrow
         * through it - rooted in the record by the pointer's own origin, which is the whole of §3.3's
         * stepping rule: the next pass needs the record whole and is refused while the value is live.
         * Everything else is read as the hand-over declared it, so `-> ->T` moves out of the slot and
         * a plain one borrows it.
         */
        Node* item = nullptr;

        if(lends) {
            /*
             * `f$item`, whose *signature* roots the value in the record - see it. Not a read of the
             * slot: a pointer read out of a field names no origin by itself, and the loan is what
             * stops the next pass while the value is still live.
             */
            auto& borrow = *decl()[site.producerItem];
            auto& declared = *decl()[borrow.args.get(decl(), 0)];

            Node* held[1] = { readPlace(builder, source, *place, requestOf(declared.occurrence)) };
            if(!held[0]) return false;

            item = callFunction(builder, borrow, held, 1, source);
        } else if(site.producerTake) {
            /*
             * `f$take`, which moves the value out of the slot and clears `$held` - see it. Not a
             * read of the field: a move out of `it.$slot` is a move out of a path the back edge
             * uses again, and what refills it is the step rather than anything MIR can see.
             */
            auto& taking = *decl()[site.producerTake];
            auto& declared = *decl()[taking.args.get(decl(), 0)];

            Node* held[1] = { readPlace(builder, source, *place, requestOf(declared.occurrence)) };
            if(!held[0]) return false;

            item = callFunction(builder, taking, held, 1, source);
        } else if(site.slotWindow) {
            /*
             * A **window** in the slot - the descriptor's own two words, restated as the view they
             * are (`RecordType::slotWindow`). Read as a copy and represented rather than read at the
             * hand-over's position directly, because the field's *type* is the view record and a
             * read is held to it - `addRepresent` is the pair that moves no bytes.
             */
            auto copy = readPlace(builder, source, *builder.fieldPlace(*place, 1), inspectRequest());
            if(copy) item = builder.addRepresent(source, *copy, handover);
        } else {
            item = readPlace(builder, source, *builder.fieldPlace(*place, 1),
                             requestOf(handover));
        }

        if(!item) return false;

        auto named = loop.pat.kind == ast::Pat::Var;
        auto bound = builder.addLocal(named ? loop.pat.var : StringId(), valueOccurrence(*item),
                                      loop.pat.source, false);
        builder.addWrite(loop.pat.source, *builder.localPlace(*bound), *item, Destination::Init);

        if(named) {
            bindLocal(loop.pat.var, bound);
        } else if(loop.pat.kind != ast::Pat::Any &&
                  !bindIrrefutable(builder, loop.pat, *builder.localPlace(*bound),
                                   "a `for` binding"_v)) {
            return false;
        }

        return true;
    };

    if(!step()) {
        bindings.resize(U32(scope));
        if(!builder.isTerminated()) builder.addUnreachable(source);
        builder.setBlock(*exit);
        carry.arrive(builder, *exit, 0);
        return true;
    }

    {
        Loop frame(*this, &carry, *exit, *header, builder.region(), false);

        if(resumes) {
            frame.resume = resume;
            frame.sent = sent.type;
            frame.proceedCon = proceedCon;
        }

        resolveStatement(builder, loop.body);
    }

    bindings.resize(U32(scope));

    /*
     * Falling off the end of a pass - the row §2.5 refuses unless `R` is `{}`.
     *
     * The statements below the loop are the consumer's own, so what is missing is an answer rather
     * than a path: the producer is suspended at a `yield` waiting for one, and a back edge that
     * supplied the fall-out row would be `next()` on a producer that asked for a value, which is
     * the thing `refuseNextOnResuming` refuses one spelling over.
     */
    if(!builder.isTerminated()) {
        SmallArray<Operand, 8> back;

        if(resumes) {
            if(decl()[resume.type]->kind != Type::Unit) {
                context.diagnostics.error(
                    "this loop's iterator hands back %@ on each pass, so every path through the body has to end with `continue v`"_v,
                    loop.body.source, typeText(context, decl(), resume.type));

                builder.addUnreachable(source);
            } else {
                auto unit = builder.addConst(source, unitConstant(program.types.unit));
                Node* only = unit;
                auto wrapped = unit ? builder.addAggregate(source, sent.type, proceedCon, &only, 1)
                                    : nullptr;

                if(wrapped) {
                    back.push(builder.resultOf(*wrapped,
                                               argumentMode(ownedContract(), SourceKind::Result)));
                } else {
                    builder.addUnreachable(source);
                }
            }
        }

        if(!builder.isTerminated()) {
            carry.edge(builder, back);
            builder.addJump(source, *header, back.pointer(), back.size());
        }
    }

    builder.setBlock(*exit);
    carry.arrive(builder, *exit, 0);
    return true;
}

/*
 * `for x in it` over an **erased** object - §3.4, and the same loop one indirection out.
 *
 *    header(sent):  %at = <the pair>(sent, False)
 *                   if %at == null -> exit
 *                   let x = iter(T)$item(it, %at)
 *                   ...body...       continue -> header, break -> exit
 *    exit:
 *
 * Built against the *type's* own step signature rather than through `Iterate`, for the reason the
 * concrete loop is built against the record's: a `for` honours the producer's declared hand-over,
 * and the class always borrows. The difference from the concrete one is one line - where the step
 * comes from - which is the whole of what erasure costs a consumer.
 *
 * **Leaving does not finish the object**, exactly as it does not for a held record: the block is the
 * author's and a `break` leaves it suspended where it was.
 */
bool Resolver::resolveErasedFor(Builder& builder, ast::Expr& expr, ast::ForExpr& loop, Place& place,
                                IterType& iterated) {
    auto source = expr.source;

    if(loop.alts.size()) {
        context.diagnostics.error(
            "an iterator object cannot fail, so there is nothing for an alternative to handle"_v,
            loop.pat.source);
        return true;
    }

    auto item = iterErasedItem(DeclPtr<Type>(&iterated - *program.decl), source);
    if(!item) return true;

    auto& reading = *decl()[item];
    auto& itemSelf = *decl()[reading.args.get(decl(), 0)];

    /*
     * A **resuming** producer, whose passes owe it a value - §2.5's last row, and the same carry the
     * concrete loop makes. The wrapper is the step's own first parameter, which the type holds.
     */
    auto resumes = decl()[iterated.sent]->kind == Type::Record &&
                   ((RecordType*)decl()[iterated.sent])->cons.size() > 1;

    U16 proceedCon = 0;
    U16 exitCon = 0;

    if(resumes) {
        auto& wrapper = *(RecordType*)decl()[iterated.sent];
        proceedCon = wrapper.cons.get(decl(), 0).index;
        exitCon = wrapper.cons.get(decl(), 1).index;
    }

    auto fallOut = [&]() -> Node* {
        auto row = builder.addConst(source, intConstant(program.types.exitCode(), U64(kIterFallOut)));
        if(!row || !resumes) return row;

        Node* only = row;
        return builder.addAggregate(source, iterated.sent, exitCon, &only, 1);
    };

    Carry carry(*this, source);

    auto header = builder.addBlock(StringId());
    auto exit = builder.addBlock(StringId());

    if(resumes) builder.addBlockArgument(*header, owned(iterated.sent), source);

    carry.declare(builder, *header);
    carry.declare(builder, *exit);

    SmallArray<Operand, 8> entry;

    if(resumes) {
        auto first = fallOut();
        if(!first) return true;
        entry.push(builder.resultOf(*first, argumentMode(ownedContract(), SourceKind::Result)));
    }

    carry.edge(builder, entry);
    builder.addJump(source, *header, entry.pointer(), entry.size());

    builder.setBlock(*header);

    Node* answered = nullptr;
    if(resumes) answered = builder.base()[header->arguments.get(builder.base(), 0)];

    carry.arrive(builder, *header, resumes ? 1 : 0);

    auto scope = bindings.size();

    auto step = [&]() -> bool {
        /*
         * The pair, read as the step's own position asks - **exclusive**, because a step writes
         * through the block the second word names. The read is what roots the item's loan, which is
         * why it happens here and not once above the loop.
         */
        auto pair = readPlace(builder, source, place,
                              Request { Access::Unique, true });
        if(!pair) return false;

        auto sent = answered ? answered : fallOut();
        auto stop = builder.addConst(source, truthConstant(program, false));
        if(!sent || !stop) return false;

        Node* arguments[2] = { sent, stop };
        auto at = builder.addCallValue(source, *pair, iterated.step, arguments, 2);
        if(!at) return false;

        // Whether the producer answered anything, which for an address is the one value an item
        // cannot have. Compared as a number, because a comparison is over integers.
        auto number = builder.addBitcast(source, *at, program.types.words[0]);
        auto zero = builder.addConst(source, intConstant(program.types.words[0], 0));
        if(!number || !zero) return false;

        auto more = builder.addBinary(source, BinaryOp::Ne, *number, *zero);
        if(!more) return false;

        /*
         * The address, in storage across the branch.
         *
         * `failIfNot` opens a block, and a value crossing one is handed over as a block argument -
         * which the carry the loop was built with does not have room for. A frame slot is the one
         * thing that crosses without being an edge, and what is in it is a raw pointer, so writing
         * it once per pass costs a store.
         */
        auto address = hold(builder, *at, source);
        if(!address) return false;

        failIfNot(builder, *more, *exit, carry, source);

        auto found = readPlace(builder, source, *address, inspectRequest());
        if(!found) return false;

        /*
         * The item, rooted in the object - `iter(T)$item`, whose *signature* is the whole of it. Not
         * a read through the pointer: an address answered by a call names no origin by itself, and
         * the loan is what stops the next pass while the value is still live.
         */
        auto self = readPlace(builder, source, place, requestOf(itemSelf.occurrence));
        if(!self) return false;

        Node* held[2] = { self, found };
        auto value = callFunction(builder, reading, held, 2, source);
        if(!value) return false;

        auto named = loop.pat.kind == ast::Pat::Var;
        auto bound = builder.addLocal(named ? loop.pat.var : StringId(), valueOccurrence(*value),
                                      loop.pat.source, false);
        builder.addWrite(loop.pat.source, *builder.localPlace(*bound), *value, Destination::Init);

        if(named) {
            bindLocal(loop.pat.var, bound);
        } else if(loop.pat.kind != ast::Pat::Any &&
                  !bindIrrefutable(builder, loop.pat, *builder.localPlace(*bound),
                                   "a `for` binding"_v)) {
            return false;
        }

        return true;
    };

    if(!step()) {
        bindings.resize(U32(scope));
        if(!builder.isTerminated()) builder.addUnreachable(source);
        builder.setBlock(*exit);
        carry.arrive(builder, *exit, 0);
        return true;
    }

    {
        Loop frame(*this, &carry, *exit, *header, builder.region(), false);

        if(resumes) {
            frame.resume = owned(iterated.resume);
            frame.sent = iterated.sent;
            frame.proceedCon = proceedCon;
        }

        resolveStatement(builder, loop.body);
    }

    bindings.resize(U32(scope));

    if(!builder.isTerminated()) {
        SmallArray<Operand, 8> back;

        if(resumes) {
            if(decl()[iterated.resume]->kind != Type::Unit) {
                context.diagnostics.error(
                    "this loop's iterator hands back %@ on each pass, so every path through the body has to end with `continue v`"_v,
                    loop.body.source, typeText(context, decl(), iterated.resume));

                builder.addUnreachable(source);
            } else {
                auto unit = builder.addConst(source, unitConstant(program.types.unit));
                Node* only = unit;
                auto wrapped = unit ? builder.addAggregate(source, iterated.sent, proceedCon,
                                                           &only, 1)
                                    : nullptr;

                if(wrapped) {
                    back.push(builder.resultOf(*wrapped,
                                               argumentMode(ownedContract(), SourceKind::Result)));
                } else {
                    builder.addUnreachable(source);
                }
            }
        }

        if(!builder.isTerminated()) {
            carry.edge(builder, back);
            builder.addJump(source, *header, back.pointer(), back.size());
        }
    }

    builder.setBlock(*exit);
    carry.arrive(builder, *exit, 0);
    return true;
}

/*
 * `instance Iterate(f.Iter, Y, R)`, and the two members that fill it - §2.3's class.
 *
 * Generated per site type rather than answered structurally, which is where this class differs from
 * `Call`: what *calling* a value means is a fold over its type, and what *stepping* one does is a
 * body - the step call, and the `Maybe` around what came back. So there are two cells in a witness
 * and two functions to fill them, and both are `Function::Generated` for `f$step`'s reason: there is
 * nothing in MIR for them to be, since what they build is decided by the record's own layout.
 *
 * **For every producer**, because the class **borrows** the item out of the object:
 * `next(&self: source's) -> Maybe(source'item)`. That is the one signature a lending producer and an
 * owning one both have an instance of - the record keeps the item in its slot either way, and what a
 * body compiled once gets is a loan of it. The three pieces the signature needs are the ones written
 * below: the instantiation `Maybe(Y)`, the **origin slot** on `self`, and the **schema** that binds
 * the payload position to it. `LentIter.next` is the same statement written in source.
 *
 * What the loan buys is §3.3's stepping rule for free: the result is rooted in `self`, so stepping,
 * moving or dropping the object while the item is live is the loan checker's ordinary refusal rather
 * than a rule of this feature's own.
 */
static Contract declaredContractAt(DeclBase base, const Occurrence& declared, DeclList<U16> path);

bool Resolver::iterIterateInstance(Function& producer, RecordType& record,
                                   const Occurrence& handover, LocationId source) {
    auto with = substitutionOf(decl(), producer);

    // The **generic**'s channel, for `resolveObjectFor`'s reason: a clone carries parameters and a
    // result, and `<- R` is written on the declaration `bodyOwnerOf` answers.
    auto& declaring = bodyOwnerOf(decl(), producer);
    auto resume = declaring.resume.type
        ? substituteType(context, program, declaring.resume.type, with)
        : program.types.unit;

    auto recordType = asBase<Type>(DeclPtr<RecordType>(&record - *program.decl));

    IterateSubject subject;
    subject.type = recordType;
    subject.module = producer.module;
    subject.base = producer.symbol;
    subject.record = recordType;
    subject.from = DeclPtr<Function>(&producer - *program.decl);
    subject.next = Function::Generated::IterNext;
    subject.resume = Function::Generated::IterResume;

    /*
     * Which of the producer's origins each loan the object keeps is in - `self`'s shape.
     *
     * `$new` publishes one slot per **lent** parameter, in order, and its result's shape says where
     * in the record each one sits; the producer's own signature says which origin the parameter was
     * lent in. Slot `1 + s` of the step is the producer's origin `s`, so an item position the
     * producer bound to `s` and the record position holding what the caller lent in `s` meet in one
     * group at the call - the item is kept alive by the caller's storage, not by the object, and
     * stepping past it is no conflict. A parameter lent for the call only publishes nothing the
     * item could name, and is rooted in `self`'s own slot like everything else the object owns.
     */
    subject.declared = &declaring.handover;
    subject.producerSlots = declaring.originSlots;

    auto made = record.producerNew ? decl()[record.producerNew] : nullptr;
    if(made && !Shapes::isEmpty(made->result.shape)) {
        SmallArray<U16, 4> slotOfLent;
        for(U32 i = 0; i < declaring.writtenArity(); i++) {
            if(IterCaptureKind(record.captureKinds.get(decl(), i)) == IterCaptureKind::Value) continue;

            auto contract = decl()[declaring.args.get(decl(), i)]->occurrence.contract;
            auto slot = contract.origin() == Origin::Slot ? contract.slot() : kNoOriginSlot;
            slotOfLent.push(slot < declaring.originSlots ? U16(1 + slot) : U16(0));
        }

        auto& schema = *decl()[made->result.shape];
        SmallArray<ExternalSlot, 4> positions;

        for(auto slotPointer: schema.slots.contents(decl())) {
            auto& kept = *decl()[slotPointer];
            auto lent = kept.contract.slot();

            ExternalSlot position;
            position.path = kept.path;
            position.contract = slotContract(kept.contract.access(),
                                             lent < slotOfLent.size() ? slotOfLent[lent] : U16(0),
                                             kept.contract.form());
            positions.push(position);
        }

        subject.selfShape = program.shapes.intern(positions.pointer(), positions.size());
    }

    /*
     * `f$take` hands the item over the way `f$next` lends it, and so under the same shape (J127).
     *
     * It is the owning half of the same protocol: a `for` over a held `-> ->T` object moves each
     * item out of the slot, and what the item refers to is where the producer's declaration put it
     * whoever steps the object. Unshaped, an owned item holding a `yield'` reference into the record
     * - or a reference into storage the caller lent - came out of the call rooted in nothing, which
     * is J125's first bug again on the path `next` does not take. So `self` gets the step's shape and
     * the item its interior positions, each in `self`'s slot or in `1 + a` for the caller's origin
     * `a`, exactly as `iterateInstanceFor` roots them beneath `Maybe`'s payload - here the item is
     * the result itself, so the paths are the hand-over's own.
     */
    if(record.producerTake) {
        auto& take = *decl()[record.producerTake];
        decl()[take.args.get(decl(), 0)]->occurrence =
            Occurrence { recordType, slotContract(Access::Unique, 0), subject.selfShape };
        take.originSlots = U16(1 + subject.producerSlots);

        if(!Shapes::isEmpty(handover.shape)) {
            auto& schema = *decl()[handover.shape];
            SmallArray<ExternalSlot, 4> positions;

            for(auto slotPointer: schema.slots.contents(decl())) {
                auto& inner = *decl()[slotPointer];

                auto slot = U16(0);
                auto declared = declaredContractAt(decl(), declaring.handover, inner.path);
                if(declared.origin() == Origin::Slot && declared.slot() < subject.producerSlots) {
                    slot = U16(1 + declared.slot());
                }

                ExternalSlot position;
                position.path = inner.path;
                position.contract = slotContract(inner.contract.access(), slot, inner.contract.form());
                positions.push(position);
            }

            take.result = Occurrence { handover.type, ownedContract(),
                                       program.shapes.intern(positions.pointer(), positions.size()) };
        }
    }

    return iterateInstanceFor(subject, handover, resume, source);
}

/*
 * The contract a **declared** hand-over gives the position at `path` - the deepest position the
 * declaration bound on the way there, or the root's own where it bound none.
 *
 * By path rather than by index, which is the one thing `originContractAt` does not do: the positions
 * the object's item has are the *lent local's* (`handover.shape`, the storage the record holds), and
 * the declaration's are whatever the signature wrote - `%yield'I32` binds the pointee, and
 * `a'Entry(k, v)` binds nothing inside and means all of it. The two lists agree on paths and not on
 * order or length.
 */
static Contract declaredContractAt(DeclBase base, const Occurrence& declared, DeclList<U16> path) {
    auto answer = declared.contract;
    Size depth = 0;
    if(Shapes::isEmpty(declared.shape)) return answer;

    for(auto slotPointer: base[declared.shape]->slots.contents(base)) {
        auto& slot = *base[slotPointer];
        auto length = slot.path.size();
        if(length <= depth || length > path.size()) continue;

        auto prefix = true;
        for(Size i = 0; i < length && prefix; i++) {
            prefix = slot.path.get(base, i) == path.get(base, i);
        }

        if(prefix) {
            answer = slot.contract;
            depth = length;
        }
    }

    return answer;
}

/*
 * The two members and the instance, for whichever subject is being made steppable.
 *
 * Split out of `iterIterateInstance` because the erased `iter(T)` needs exactly this and has no
 * producer: what the class asks for is a type that steps, and whether the code is known from that
 * type or reached through a pair is the *body's* question rather than the signature's. So the
 * signatures are here once - `Maybe(source'item)`, the origin slot on `self`, and the schema that
 * binds the payload position to it - and the four generated kinds are what differ.
 */
bool Resolver::iterateInstanceFor(const IterateSubject& subject, const Occurrence& handover,
                                  DeclPtr<Type> resume, LocationId source) {
    auto klass = program.roles.klass(intrinsic::Known::iterate);
    if(!klass) return true;

    auto& declaration = *decl()[klass];
    if(declaration.members.size() < 2) return true;

    auto& next = *decl()[declaration.members.get(decl(), kIterateNext)];

    /*
     * `Maybe(Y)`, taken from the class member's own result rather than from a role of its own.
     *
     * The declaration says `-> Maybe(item)`, so what `Maybe` *is* here is whatever the library wrote
     * on that member - which is the same reading `instanceMember` gives every other class and is one
     * fewer row in `known.def` for a type the compiler has no opinion about.
     */
    auto maybe = recordDeclarationOf(decl(), next.result.type);
    if(!maybe) return true;

    DeclPtr<Type> payload[1] = { handover.type };
    auto answer = instantiateRecord(context, program, maybe, payload, 1, source);
    if(!answer) return false;

    /*
     * `Maybe(source'Y)` - the instantiation above with a **shape** over it, which is the whole of
     * what makes the item a loan rather than a copy.
     *
     * One position, at the field the carrying constructor owns, in origin slot 0: `qualifiesInterior`
     * reads exactly this - one field step and nothing after it - and answers that the member is
     * somebody else's, so `manifestNative` lays the payload out as a reference and the record owes
     * nothing for it. Found by which constructor *carries* something rather than by position, for
     * the reason the emitter finds it that way: the order `lib/Core` writes `Nothing` and `Just` in
     * is not a second fact here.
     */
    auto& carried = *(RecordType*)decl()[answer];
    auto lent = U16(0xffff);

    for(auto constructor: carried.cons.contents(decl())) {
        if(constructor.count) lent = constructor.first;
    }

    if(lent == 0xffff) return true;

    /*
     * **Shared**, whatever the producer's own convention is, because that is what the class declares:
     * `Maybe(source'item)` is an unqualified position and the instance has to be the same shape as
     * the member it fills. A `-> ->T` producer hands its value over at a *concrete* `for`, and it is
     * the record that holds it here - so what a body compiled once gets is a look at it.
     *
     * An owned slot would also be the wrong *bytes*: `isReferenceValue` answers false for an owned
     * contract however it is qualified, so the position would be laid out as the member and filled
     * with its address.
     *
     * And under the **form** the position takes at this type, which is `prepareArgument`'s own
     * restatement: `item` is a type variable in the class, so a shared position at `String` is that
     * string's *window* and at a record is the record's address. Reading it off the `Maybe`
     * declaration's own field is reading the same question the caller asks.
     */
    auto& generic = *(RecordType*)decl()[maybe];
    auto declared = generic.field(decl(), lent).occurrence.type;

    ExternalSlot position;
    position.path.push(program.decl, PathStep::field(lent));
    position.contract = substitutedContract(decl(), program, declared, handover.type,
                                            slotContract(Access::Shared, 0));

    /*
     * The item's **interior** positions under the payload's, each rooted where the producer's
     * declaration says - J125.
     *
     * They were left out, and that was two bugs. The **layout**: a position inside the item that is
     * a reference (`Entry('k, 'v)` at `String`, whose key is a window) is part of what the record's
     * slot holds, and a payload read without it read the window's two words as an owned string -
     * `pairs(m).next()` over string keys printed garbage. And the **extent**: `%yield'I32` says the
     * pointee is in the frame that handed it over, and here that frame *is* the object - the
     * producer keeps its locals in the record between steps and the next step reuses or frees them.
     * With the root the item's only tie to the object, and a pointer trivially copied, `let q = p`
     * kept the address and dropped the loan, and `q` read after the next `next()` read a finished
     * pass (J124 §2.5).
     *
     * So a position the declaration left to the frame - `yield'`, or the unnamed `'k` of
     * `Entry('k, 'v)` - is `self`'s slot, and the next step, a move or a drop of the object is the
     * ordinary conflict with a live copy. A position naming one of the **caller's** origins
     * (`-> %a'T`) is in storage the caller lent and outlives any one step and the object too, so it
     * is rooted in slot `1 + a`, which `self`'s shape binds to where the record keeps that loan.
     * Paths are the lent local's (`handover.shape`), since those are the bytes; the declaration is
     * read by path (`declaredContractAt`). The erased form has no declaration and roots every
     * position in the object, which is what its `$item` has always done.
     */
    SmallArray<ExternalSlot, 4> positions;
    positions.push(position);

    if(!Shapes::isEmpty(handover.shape)) {
        auto& schema = *decl()[handover.shape];

        for(U32 i = 0; i < schema.slots.size(); i++) {
            auto& inner = *decl()[schema.slots.get(decl(), i)];

            auto slot = U16(0);
            if(subject.declared) {
                auto declared = declaredContractAt(decl(), *subject.declared, inner.path);
                if(declared.origin() == Origin::Slot && declared.slot() < subject.producerSlots) {
                    slot = U16(1 + declared.slot());
                }
            }

            ExternalSlot interior;
            interior.path.push(program.decl, PathStep::field(lent));
            for(auto step: inner.path.contents(decl())) interior.path.push(program.decl, step);
            interior.contract = slotContract(inner.contract.access(), slot, inner.contract.form());
            positions.push(interior);
        }
    }

    Occurrence answered { answer, ownedContract(),
                          program.shapes.intern(positions.pointer(), positions.size()) };

    auto member = [&](StringView suffix, Function::Generated kind, DeclPtr<Type> sent) {
        auto pointer = generatedFor(subject.module, subject.base, suffix, kind, subject.record,
                                    subject.from, source);
        auto made = decl()[pointer];

        // `&self: source's` - the origin slot the result is rooted in, and the second of the three
        // pieces the borrowing form needs.
        auto self = new (program.decl) Arg();
        self->name = context.addUnqualifiedName("$self", 5);
        self->source = source;
        self->index = 0;
        self->occurrence = Occurrence { subject.type, slotContract(Access::Unique, 0),
                                        subject.selfShape };
        made->args.push(program.decl, self - *program.decl);

        if(sent) {
            auto sentArg = new (program.decl) Arg();
            sentArg->name = context.addUnqualifiedName("$v", 2);
            sentArg->source = source;
            sentArg->index = 1;
            sentArg->occurrence = owned(sent);
            made->args.push(program.decl, sentArg - *program.decl);
        }

        made->result = answered;
        made->originSlots = U16(1 + subject.producerSlots);
        return pointer;
    };

    auto nextMember = member("$next"_v, subject.next, nullptr);
    auto resumeMember = member("$resume"_v, subject.resume, resume);

    auto instance = new (program.decl) Instance();
    instance->klass = klass;
    instance->args.push(program.decl, subject.type);
    instance->args.push(program.decl, handover.type);
    instance->args.push(program.decl, resume);
    instance->members.push(program.decl, nextMember);
    instance->members.push(program.decl, resumeMember);
    instance->module = subject.module;
    instance->source = source;

    auto made = DeclPtr<Instance>(instance - *program.decl);
    decl()[subject.module]->instances.push(program.decl, made);

    if(!program.addInstance(decl(), made)) {
        context.diagnostics.error("internal: an iterator object's `Iterate` collided with an instance"_v,
                                  source);
        return false;
    }

    return true;
}

/*
 * The erased `iter(T)` - Design-Iterator-Objects.md §2.2 and §3.4, and the one function that mints
 * one.
 *
 * Interned on the protocol, so `iter(I32)` written in a field and `iter(I32)` written on a parameter
 * are one handle whichever module wrote them - which is what makes an object built for one position
 * pass through the other. The two things the mint adds beside the type are the **step signature**
 * the pair is called across and the generated `instance Iterate(iter(T), T, R)` that steps it, and
 * both are made once for the same reason the type is interned once.
 *
 * `| E` is not a channel here. An object's step would need a terminal failure on it - §6.6's first
 * gap - so the caller refuses it by name before reaching this; the protocol that arrives here is the
 * hand-over and the resume channel and nothing else.
 */
DeclPtr<Type> Resolver::erasedIterTypeOf(const Occurrence& handover, DeclPtr<Type> resume,
                                         LocationId source) {
    /*
     * The type itself is `mir::erasedIterTypeOf`'s and has been since J61 - a free function, because
     * `substituteType` mints one and cannot see a resolver. What is left here is the **instance**.
     *
     * **Queued rather than made, while the declarations are still being read.** A written `iter(T)`
     * is resolved in the field pass and in the header pass, and `Iterate` is a `lib/Core` class that
     * may not be declared yet at either - so the type is minted, which needs nothing, and the
     * instance waits for `completeErasedIterates`. After that pass every mint makes its instance on
     * the spot, because a body that adapts an object to a position is past the point where an
     * instance may still be added.
     */
    auto made = program.erasedIters.size();
    auto type = mir::erasedIterTypeOf(context, program, handover, resume, source, true);
    if(!type) return nullptr;

    if(program.erasedItersReady && program.erasedIters.size() != made) {
        if(!erasedIterateInstance(program.erasedIters[program.erasedIters.size() - 1])) {
            return nullptr;
        }
    }

    return type;
}

// One queued erased type made steppable - `erasedIterTypeOf`, where the queue is argued.
bool Resolver::erasedIterateInstance(Program::ErasedIter& pending) {
    if(!pending.pending) return true;
    pending.pending = false;

    /*
     * The class's own module, which is where these two functions and the instance go.
     *
     * An erased type belongs to no module - it is interned on a protocol and any module may write
     * it - so the declaration that *is* somewhere is the class, and hanging the instance beside it
     * is what keeps the orphan rule a statement about written instances rather than about generated
     * ones. It also makes the answer independent of which module happened to mint the type first.
     */
    auto klass = program.roles.klass(intrinsic::Known::iterate);
    if(!klass) return true;

    IterateSubject subject;
    subject.type = pending.type;
    subject.module = decl()[klass]->module;
    subject.base = program.uniqueSymbol(subject.module, context,
                                        stringView(typeText(context, decl(), pending.type)));
    subject.record = pending.type;
    subject.next = Function::Generated::IterErasedNext;
    subject.resume = Function::Generated::IterErasedResume;

    return iterateInstanceFor(subject, pending.handover, pending.resume, pending.source);
}

/*
 * `chunks$obj` - the **signature** of one class member's "make an object" entry, §6.4.
 *
 *      fn chunks$obj(self: c) -> iter('[a])
 *
 * One per member written `iter fn`, over the class's own context, so that a body compiled once has
 * something to be checked against and the lowering has something to build a thunk from. The member
 * itself cannot serve: what a call to an `iter fn` produces is a record minted per instance, which
 * is a different type for every instance and therefore no signature at all.
 *
 * **Made on demand**, at the first call that builds an object of the member, and that is what keeps
 * the witness of every other class exactly the table it was: `classObjectCount` counts the members
 * that have one, so a class whose `iter fn` no body ever objectifies grows no slot, fills no cell
 * and mints no record. The demand is settled while the bodies are read and every witness is filled
 * after them, so the layout cannot change under a table that has been built.
 *
 * The result type is minted **unrecorded** - `mir::erasedIterTypeOf`'s `record` argument - and that
 * is the one decision here. `iter('[a])` over a class's variable is a shape and not a protocol any
 * value has: what steps a value is the instance's, at the protocol substitution produced, and an
 * `Iterate` instance at this head would be a blanket instance over the class whose whole job is to
 * say which step to call.
 */
DeclPtr<Function> Resolver::iterObjectMember(Function& member) {
    if(member.iterObject || !member.klass) return member.iterObject;

    auto& klass = *decl()[member.klass];

    auto erased = mir::erasedIterTypeOf(context, program, member.handover, member.resume.type,
                                        member.source, false);
    if(!erased) return nullptr;

    auto text = Tritium::format("%@$obj", context.findName(member.name));
    auto symbol = program.uniqueSymbol(klass.module, context,
                                       StringView(text.text(), text.size()));

    auto made = new (program.decl) Function(symbol, klass.module);
    made->source = member.source;
    made->gen = klass.gen;
    made->klass = member.klass;
    made->classMember = member.classMember;
    made->originSlots = member.originSlots;
    made->objectEntry = true;
    made->result = owned(erased);

    /*
     * The member's **written** parameters, and not its continuation: what an `iter fn` declares last
     * is where a `for` puts its body, and a call that builds an object writes none - the object *is*
     * the continuation, resumed a step at a time.
     */
    for(U32 a = 0; a < member.args.size(); a++) {
        if(a == member.continuationArg) continue;

        auto& taken = *decl()[member.args.get(decl(), U16(a))];

        auto declared = new (program.decl) Arg();
        declared->name = taken.name;
        declared->source = taken.source;
        declared->index = U16(made->args.size());
        declared->occurrence = taken.occurrence;
        made->args.push(program.decl, declared - *program.decl);
    }

    member.iterObject = DeclPtr<Function>(made - *program.decl);
    return member.iterObject;
}

/*
 * `f$obj` - the entry's **body**, one per selected instance member, §6.4.
 *
 * What a witness cell holds is a function over the instance's own state record: `g$new` with the
 * call's arguments and one `fun.value` over the result, which is the heap-placed pair a consumer
 * steps. `Function::Generated::IterObject` is the body and `resolve_ssa/convert.cpp` is where it is
 * filled, for the reason every generated body is there - this is made after the drop elaboration has
 * run, so a MIR body would be one nothing placed a release into.
 *
 * Reached from `fillClassWitness` through `Program::IterObjects`, because the walk that fills a
 * witness is the reachability walk and the argument lists it fills one at are what that walk
 * discovers. Minting the record here is therefore a mint after the resolver's own passes, which is
 * the same position `instanceMember` already specializes a body from.
 */
DeclPtr<Function> Resolver::objectMaker(DeclPtr<Function> member, DeclPtr<Function> entry,
                                        LocationId source) {
    auto& selected = *decl()[member];
    if(selected.iterMaker) return selected.iterMaker;

    auto& producer = bodyOwnerOf(decl(), selected);
    auto with = substitutionOf(decl(), selected);

    auto site = iterSiteTypeOf(producer, with, source);
    if(!site) return nullptr;

    /*
     * And the **boxed** nests of whatever that mint reached, which is the one pass this position is
     * on the wrong side of - §6.3, `completeBoxedNests`.
     *
     * A witness is filled by the reachability walk, so a record minted here is minted after the pass
     * that gives every boxed member the shim its pair is called across. Draining it again is
     * idempotent - `iterErasedStep` interns, and a member already filled is filled with the same
     * handle - and it is what lets a **recursive** class member be objectified at all: without it
     * the member is null and the step reports "an embedded region has no object to step" against a
     * body the author did not write.
     */
    if(!completeBoxedNests()) return nullptr;

    auto& record = *(RecordType*)decl()[site];

    auto handover = substituteOccurrence(context, program, producer.handover, with);
    auto resume = producer.resume.type
        ? substituteType(context, program, producer.resume.type, with) : nullptr;

    auto erased = erasedIterTypeOf(handover, resume, source);
    if(!erased) return nullptr;

    // The **heap** shim and never the frame one, for the reason a box uses it: what the witness
    // hands back outlives the call that made it, so the record goes into a block with its teardown
    // in front of it - `RecordType::producerBox`.
    auto shim = iterErasedStep(record, erased, true, false, source);
    if(!shim) return nullptr;

    auto pointer = generatedFor(producer.module, producer.symbol, "$obj"_v,
                                Function::Generated::IterObject, site,
                                DeclPtr<Function>(&producer - *program.decl), source);

    auto made = decl()[pointer];

    /*
     * The **member's** own written parameters, and not the constructor's, which is the one decision
     * here that is not a copy.
     *
     * A witness cell is reached two ways: through a thunk, which adapts the class's declared
     * positions to whatever the concrete function takes, and **directly**, where a body specialized
     * at a concrete subject calls the same maker with no thunk in between. The second is what makes
     * the signature the member's: a call built against the class's declaration has to be a call this
     * accepts. `emitIterObject` is where the constructor's own conventions are met, on the same two
     * lines an embedded region's capture already goes through.
     */
    /*
     * As many positions as the entry declares, which is the member's written parameters and not its
     * continuation - a call that builds an object writes no body. Counted off the entry because a
     * **specialization** does not carry `continuationArg`, so the member in hand cannot say how many
     * of its own parameters a caller writes.
     */
    for(U32 a = 0; a < selected.args.size() && a < decl()[entry]->args.size(); a++) {
        auto& taken = *decl()[selected.args.get(decl(), U16(a))];

        auto declared = new (program.decl) Arg();
        declared->name = taken.name;
        declared->source = taken.source;
        declared->index = U16(made->args.size());
        declared->occurrence = taken.occurrence;
        made->args.push(program.decl, declared - *program.decl);
    }

    made->originSlots = selected.originSlots;
    made->result = owned(erased);

    selected.iterMaker = pointer;
    return pointer;
}

/*
 * Every erased type minted while the declarations were being read, made steppable - the pass
 * `resolveSiteSignatures` is one of, and for the same reason.
 *
 * A written `iter(T)` in a field or a header is resolved before `lib/Core`'s `Iterate` is certain to
 * be a declaration, and an instance made against a class that is not there is an instance that is
 * silently not made. So the mint is split: the *type* needs nothing but the protocol, and the
 * instance waits until every declaration is in.
 */
bool Resolver::completeErasedIterates() {
    program.erasedItersReady = true;

    auto ok = true;
    for(U32 i = 0; i < program.erasedIters.size(); i++) {
        if(!erasedIterateInstance(program.erasedIters[i])) ok = false;
    }

    return ok;
}

/*
 * Every boxed nest member given the child it holds - §6.3's recursion, and the one pass that has to
 * run after the last object in the program has been minted.
 *
 * A boxed member's field is the protocol `iter(Y)`, so which record the step has to build is not
 * something the field's *type* says. `RecordType::nestBoxed` is where it is said instead, and what
 * goes in it is the `g$erased.heap` shim the member's pair is called across - one handle rather than
 * two, because a shim carries the record it steps (`Function::generatedRecord`).
 *
 * Here rather than at the mint because a mutually recursive pair mints the second inside the first:
 * `walkB`'s box over `walkA` is decided while `walkA.Iter` is still being built, so the producer is
 * a name and the record is not yet a type. Every record in the program exists by the time this runs.
 *
 * The **heap** shim and never the frame one: a boxed level outlives the step that built it, which is
 * the whole reason the member is a box.
 */
bool Resolver::completeBoxedNests() {
    auto ok = true;

    for(auto& pending: boxedNests) {
        DeclPtr<Type> site = nullptr;
        for(auto& known: iterSites) {
            if(known.producer != pending.producer) continue;
            site = known.record;
            break;
        }

        if(!site) {
            context.diagnostics.error(
                "internal: a boxed recursive traversal names a producer with no record"_v,
                pending.source);
            ok = false;
            continue;
        }

        auto shim = iterErasedStep(*(RecordType*)decl()[site], pending.erased, true,
                                   pending.delegate, pending.source);
        if(!shim) { ok = false; continue; }

        /*
         * And the ordinary shim beside it where the member delegates - `RecordType::producerBox`.
         *
         * The two paths through a delegating region need two different pairs over one record: the
         * raw shim where the level delegates, because only that one may answer the third row, and
         * this one on the departure path, where the level steps the child itself and a third row
         * would be an answer nobody is listening for.
         */
        if(pending.delegate) {
            auto plain = iterErasedStep(*(RecordType*)decl()[site], pending.erased, true, false,
                                        pending.source);
            if(!plain) { ok = false; continue; }
        }

        decl()[pending.record]->nestBoxed.pointer(decl())[pending.entry] = shim;
    }

    return ok;
}

/*
 * `f$erased` - the step a `{step, block}` pair is called across, and the whole of what erasure costs
 * at a step.
 *
 *      fn f$erased(&$env: f.Iter, $sent: S, $cancel: Bool) -> %Y
 *
 * Shaped exactly as a **lifted lambda** is - the environment first and exclusive - because that is
 * what makes the erased value a closure: `addFunValue` builds the pair, `emitFunValue` places the
 * environment in the frame or in a heap block with its teardown in front of it, and `funTeardown`
 * releases it from a site that cannot name the producer. Nothing about the erased form is a second
 * mechanism; it is the closure block with an `iter fn`'s state record inside it.
 *
 * **Two of them per producer at most**, one per storage class, because `emitFunValue` reads where
 * the environment goes off the callee. Which one a position gets is `adaptIterValue`'s answer and is
 * the position's own convention: an owning position gets the heap block and a borrowing one gets the
 * frame, exactly as a lambda written at either does.
 */
DeclPtr<Function> Resolver::iterErasedStep(RecordType& site, DeclPtr<Type> erased, bool heap,
                                           bool raw, LocationId source) {
    auto recordType = asBase<Type>(DeclPtr<RecordType>(&site - *program.decl));

    for(auto& known: erasedSteps) {
        if(known.record == recordType && known.heap == heap && known.raw == raw) return known.step;
    }

    auto& producer = *decl()[site.producer];
    auto& iterated = *(IterType*)decl()[erased];

    /*
     * The **raw** shim - §6.3, and the one function that ever writes a step's third row.
     *
     * `f$erased` answers where the item is, or null; this answers those two and a third - the link
     * of a child this level has just entered, with the low bit set. Only a delegable box's pair
     * names it and only a trampoline steps one, which is what keeps the row off the channel a
     * consumer reads and is why a step that never delegates costs nothing for delegation existing.
     *
     * Always the heap variant, for the reason every box is: the child outlives the step that built
     * it.
     */
    auto pointer = generatedFor(producer.module, producer.symbol,
                                raw ? "$erased.step"_v : heap ? "$erased.heap"_v : "$erased"_v,
                                raw ? Function::Generated::IterErasedRaw
                                    : Function::Generated::IterErasedStep, recordType,
                                site.producer, producer.source);

    auto made = decl()[pointer];
    made->environment = recordType;
    made->environmentStorage = heap ? StorageClass::Heap : StorageClass::Frame;

    // The same statement `liftFunction` makes: a heap-placed environment is an object whose
    // placement left the frame, and that is what tells the reachability walk the allocator is
    // needed.
    if(heap) program.usesHeapPlacement = true;

    auto& step = *decl()[site.producerStep];

    auto environment = new (program.decl) Arg();
    environment->name = context.addUnqualifiedName("$env", 4);
    environment->source = producer.source;
    environment->index = 0;
    environment->occurrence = borrowed(recordType, Access::Unique);
    made->args.push(program.decl, environment - *program.decl);

    auto sent = new (program.decl) Arg();
    sent->name = context.addUnqualifiedName("$sent", 5);
    sent->source = producer.source;
    sent->index = 1;
    sent->occurrence = owned(iterated.sent);
    made->args.push(program.decl, sent - *program.decl);

    auto cancel = new (program.decl) Arg();
    cancel->name = context.addUnqualifiedName("$cancel", 7);
    cancel->source = producer.source;
    cancel->index = 2;
    cancel->occurrence = decl()[step.args.get(decl(), 2)]->occurrence;
    made->args.push(program.decl, cancel - *program.decl);

    made->result = owned(program.types.pointerType(iterated.handover.type));

    if(raw) site.producerRaw = pointer;
    else if(heap) site.producerBox = pointer;

    erasedSteps.push(ErasedStep { recordType, pointer, heap, raw });
    return pointer;
}

/*
 * `iter(T)$item` - the item an erased step answered, said as the **loan of the object** it is.
 *
 *      fn iter(T)$item(self: source'iter(T), $at: %Y) -> source'Y
 *
 * `f$item`'s twin, and for the same reason: the loan is what says the value is the object's, so it
 * has to be stated somewhere a walk that has only the call can read it. What the body does is
 * nothing at all in the ordinary case - a `%Y` and a `source'Y` are the same eight bytes, and this
 * signature is the only thing that says which they are - and one load where the hand-over is a
 * **window**, whose `source'[a]` is a descriptor rather than an address.
 *
 * Interned per erased type, on `iterErasedStep`'s terms: one per protocol, not one per producer,
 * because what it says is about the protocol.
 */
DeclPtr<Function> Resolver::iterErasedItem(DeclPtr<Type> erased, LocationId source) {
    for(auto& known: erasedItems) {
        if(known.record == erased) return known.step;
    }

    auto& iterated = *(IterType*)decl()[erased];

    auto klass = program.roles.klass(intrinsic::Known::iterate);
    auto module = klass ? decl()[klass]->module : here();

    auto base = program.uniqueSymbol(module, context,
                                     stringView(typeText(context, decl(), erased)));

    auto pointer = generatedFor(module, base, "$item"_v, Function::Generated::IterErasedItem,
                                erased, nullptr, source);

    auto item = decl()[pointer];

    auto self = new (program.decl) Arg();
    self->name = context.addUnqualifiedName("$self", 5);
    self->source = source;
    self->index = 0;
    self->occurrence = Occurrence { erased, slotContract(Access::Shared, 0), nullptr };
    item->args.push(program.decl, self - *program.decl);

    auto at = new (program.decl) Arg();
    at->name = context.addUnqualifiedName("$at", 3);
    at->source = source;
    at->index = 1;
    at->occurrence = owned(program.types.pointerType(iterated.handover.type));
    item->args.push(program.decl, at - *program.decl);

    // The hand-over's interior positions, renumbered onto this signature's one slot - `iterItem`'s
    // own paragraph, which is the same statement about the same thing.
    auto interior = iterated.handover.shape;

    if(!Shapes::isEmpty(interior)) {
        auto& schema = *decl()[interior];
        SmallArray<ExternalSlot, 4> slots;

        for(U32 i = 0; i < schema.slots.size(); i++) {
            auto& position = *decl()[schema.slots.get(decl(), i)];

            ExternalSlot renumbered;
            renumbered.path = position.path;
            renumbered.contract = slotContract(position.contract.access(), 0,
                                               position.contract.form());
            slots.push(renumbered);
        }

        interior = program.shapes.intern(slots.pointer(), slots.size());
    }

    item->result = Occurrence { iterated.handover.type,
                                slotContract(iterated.handover.contract.access(), 0,
                                             iterated.handover.contract.form()), interior };
    item->originSlots = 1;

    erasedItems.push(ErasedStep { erased, pointer, false });
    return pointer;
}

/*
 * A site-typed object where the position asked for `iter(T)` - **the** conversion point, and the
 * iterator's half of `adaptSiteValue`.
 *
 * What it builds is the pair: the step shim above as the code word, and the state record as the
 * environment. Which storage the environment gets is **this position's** answer and not the
 * producer's, which is the one thing that differs from a closure - a lambda's placement is settled
 * where the lambda is written, because its capture conventions depend on it, and an object's
 * captures were settled at its own construction. So an owning position moves the record into a heap
 * block once and a borrowing one points at the frame local, and §3.4's "must not copy the payload at
 * a borrowing position" is that sentence read from the other end.
 *
 * `hold` is the frame slot, exactly as a borrowing argument gets one. A value that is **already a
 * borrow** has nowhere of its own to be, and is refused in `adaptSiteValue`'s words: the pair would
 * name storage whose extent is somebody else's loan.
 */
Node* Resolver::adaptIterValue(Builder& builder, Node& value, RecordType& site,
                               DeclPtr<Type> erased, bool heap, LocationId source) {
    auto shim = iterErasedStep(site, erased, heap, false, source);
    if(!shim) return nullptr;

    Node* held = nullptr;

    if(valueOccurrence(value).contract.isOwned()) {
        /*
         * An object this statement built - `f(...)` at the position, or a `let` handed over.
         *
         * It needs storage to be the pair's second word, and `hold` is the frame slot a borrowing
         * argument already gets. At an owning position the read relocates it into the heap block the
         * lowering allocates; at a borrowing one the read is a **loan** of the slot, which is what
         * ties the pair to this frame and is what `checkCarriedOrigins` refuses to let escape.
         */
        auto place = hold(builder, value, source);
        if(!place) return nullptr;

        builder.body.local(builder.base(), LocalId(place->payload))->environment = true;

        held = heap ? readPlace(builder, source, *place, bindingRequest(Access::Owned))
                    : readPlace(builder, source, *place, Request { Access::Shared, true });
    } else if(!heap) {
        /*
         * An object somebody else holds, at a **borrowing** position - the pair points at it where
         * it already is, and nothing is copied.
         *
         * That is §3.4's "adapting a site-typed object to `iter(T)` at a borrowing position must not
         * copy the payload": the loan in hand is the loan the pair carries, and the object stays the
         * one the caller is holding. The step writes through it, which is why the position that
         * reads it has to be one that lends exclusively - `Iterate`'s `&self` is what says so, and a
         * shared loan is refused at the step rather than here.
         */
        held = &value;
    } else {
        context.diagnostics.error(
            "this object is borrowed here, and an owning `iter(T)` position keeps what it holds - hand the object over with `->`, or make this position borrow"_v,
            source);
        return nullptr;
    }

    if(!held) return nullptr;
    return builder.addFunValue(source, shim, erased, held, heap);
}

/*
 * `f$item` - the lent value, borrowed from the record, and the whole of §3.3's stepping rule.
 *
 *      fn f$item(self: source'f.Iter) -> source'Y
 *
 * A **signature** and not a field read, for the reason `LentIter.current`'s signature is what makes
 * the stack handle sound: the loan is what says "this value is the object's", so it has to be stated
 * somewhere a walk that has only the call can read it. What the body does is trivial - the slot
 * holds the value's address - and what the declaration does is everything: the result is rooted in
 * `self`, so the next step needs the record whole and is refused while the item is still live.
 *
 * The record itself publishes **no** origin for this. What the slot points at is storage the record
 * already holds - a payload member, re-established at every hand-over - so a slot on the *type*
 * would say the object refers to somebody else's storage, which is exactly the thing that would then
 * stop it being stored in a field. The loan lives on this signature and lasts one step.
 */
bool Resolver::iterItem(Function& producer, RecordType& record, const Occurrence& handover,
                        LocationId source) {
    auto pointer = iterGenerated(producer, record, "$item"_v, Function::Generated::IterItem);
    auto item = decl()[pointer];
    auto recordType = asBase<Type>(DeclPtr<RecordType>(&record - *program.decl));

    auto self = new (program.decl) Arg();
    self->name = context.addUnqualifiedName("$self", 5);
    self->source = producer.source;
    self->index = 0;
    self->occurrence = Occurrence { recordType, slotContract(Access::Shared, 0), nullptr };
    item->args.push(program.decl, self - *program.decl);

    /*
     * And **where** it is, for a delegating record - §6.3, and `iter(T)$item`'s second parameter one
     * tier in.
     *
     * A level that delegates hands over its innermost child's item, so the value is not in this
     * record's slot and there is no field for the signature to be about. What the loan is rooted in
     * is still the object - the whole chain hangs off it - so the declaration is the same statement
     * with the address passed in rather than read out.
     */
    if(record.linkType) {
        auto address = new (program.decl) Arg();
        address->name = context.addUnqualifiedName("$at", 3);
        address->source = producer.source;
        address->index = 1;
        address->occurrence = owned(program.types.pointerType(handover.type));
        item->args.push(program.decl, address - *program.decl);
    }

    /*
     * The hand-over's own **interior positions**, renumbered onto this signature's one slot.
     *
     * `Entry('k, 'v)` is a record whose fields are references, and which they are is the shape - so
     * dropping it here would hand the consumer the same bytes laid out as values. Every position
     * goes to slot 0, which is `iterConstructor`'s `handleShape` making the same statement about the
     * stack handle: whatever the item refers to is kept alive by the object, and this signature has
     * one origin set to say so in.
     */
    auto interior = handover.shape;

    if(!Shapes::isEmpty(interior)) {
        auto& schema = *decl()[interior];
        SmallArray<ExternalSlot, 4> slots;

        for(U32 i = 0; i < schema.slots.size(); i++) {
            auto& position = *decl()[schema.slots.get(decl(), i)];

            ExternalSlot renumbered;
            renumbered.path = position.path;
            renumbered.contract = slotContract(position.contract.access(), 0,
                                               position.contract.form());
            slots.push(renumbered);
        }

        interior = program.shapes.intern(slots.pointer(), slots.size());
    }

    item->result = Occurrence { handover.type,
                                slotContract(handover.contract.access(), 0), interior };
    item->originSlots = 1;

    record.producerItem = pointer;
    return true;
}

/*
 * `it.next()` on a producer whose passes hand a value back - §2.5, and the one of J50's revisits
 * that was a run-time meaning where a compile-time one is available.
 *
 * `next()` means "I have nothing to send". At index zero that is honest - the producer has not
 * reached a `yield` and never reads the answer - and at a **suspend** index it is a pass that
 * answered nothing, which J50 quietly turned into "end the producer". A consumer that wrote
 * `it.next()` on a `-> (Y <- R)` producer did not mean that, so it is a failure rather than a
 * silence, and it is checked at whichever tier can see it:
 *
 *  - **here**, where the site type is written out. `it.next()` at a concrete `f.Iter` names one
 *    producer, so whether its passes owe a value is a fact about this call, and the message says to
 *    write `resume(v)`.
 *  - **in the step**, where the object arrives through the class. An `Iterate(s, y, r)` witness is
 *    one cell of a body compiled once and knows only that `r` is not `{}` - which every resuming
 *    producer's is - so what it can check is the index, and `f$next` checks it (`emitIterStepper`).
 *
 * Answers false having reported. Null where the call is not this one, which is nearly every call:
 * two pointer comparisons before anything is read.
 */
bool Resolver::refuseNextOnResuming(Function& callee, const Substitution& with, LocationId source) {
    auto klass = program.roles.klass(intrinsic::Known::iterate);
    if(!klass || callee.klass != klass || callee.classMember != kIterateNext) return true;
    if(!with.count || !with.arguments[0]) return true;

    auto record = recordDeclarationOf(decl(), with.arguments[0]);
    if(!record) return true;

    auto& site = *(RecordType*)decl()[record];
    if(!site.producer) return true;

    auto& declaring = bodyOwnerOf(decl(), *decl()[site.producer]);
    if(!declaring.resume.type) return true;
    if(decl()[declaring.resume.type]->kind == Type::Unit) return true;

    context.diagnostics.error(
        "%@'s passes hand a value back, so `next()` has nothing to send it - write `resume(v)`"_v,
        source, context.findName(decl()[site.producer]->name));
    return false;
}

/*
 * Whether a concrete `it.next()` or `it.resume(v)` calls `member`, the generated step
 * `generatedMemberAt` selected, by its own name - J125.
 *
 * The class says `next(&self: source's) -> Maybe(source'item)`, and `item` is a variable there: the
 * signature has no position for anything *inside* the item, so a call typed by it cannot say that a
 * `%yield'I32` item points into the object, nor that an `Entry('k, 'v)` item's key is a window. The
 * generated member's signature says both (`iterateInstanceFor`), and at a concrete subject the
 * member is known - the instance is generated, ground, and is what `callTarget` would select at the
 * conversion anyway. So the call names it, exactly as `expandProvidedCall` replaces a concrete call
 * of a provided instance's member with what the member computes, and the loan analysis reads the
 * signature that is true of this object instead of the one every implementation shares. These four
 * are exactly `Iterate`'s members at a site type and at the erased one, so no member index is read.
 *
 * A body generic in `Iterate` still calls the class member, and the item stays opaque there.
 */
bool stepsOwnObject(const Function& member) {
    switch(member.generated) {
        case Function::Generated::IterNext:
        case Function::Generated::IterResume:
        case Function::Generated::IterErasedNext:
        case Function::Generated::IterErasedResume:
            return true;
        default:
            return false;
    }
}

/*
 * An object whose item points into the object, handed to a body generic in `Iterate` - refused,
 * J125.
 *
 * `stepsOwnObject` roots such an item in the object where the object's type is known. A body
 * generic in the class cannot: `item` is a variable there, a `%yield'I32` item is a bare `%I32` to
 * it, and `Copy(item)` lets it keep one past the next step and hand it back - `NG.yana` answered 0
 * from a finished pass. The class has no position to say "inside the item, and in the object", so
 * the refusal is at the one point that sees both sides: the requirement's discharge, where the
 * selected instance's step says what the class does not. A position the producer bound to one of
 * its caller's origins is not in the object, and passes.
 *
 * Not for the class's own members: `it.next()` at a known type is the step itself.
 */
bool Resolver::refuseOpaqueObjectItem(Function& callee, GenRequirement requirement,
                                      const Substitution& with, LocationId source) {
    auto klass = program.roles.klass(intrinsic::Known::iterate);
    if(!klass || requirement.klass != klass || callee.klass == klass) return true;

    SmallArray<DeclPtr<Type>, 4> arguments;
    for(auto argument: requirement.args.contents(decl())) {
        auto supplied = substituteType(context, program, argument, with);
        if(!supplied || !isConcreteType(decl(), supplied)) return true;
        arguments.push(supplied);
    }

    InstanceMatch found;
    if(!selectInstance(context, program, klass, arguments.pointer(), arguments.size(), source,
                       false, found)) {
        return true;
    }

    auto member = decl()[found.instance]->members.get(decl(), kIterateNext);
    if(!member) return true;

    auto& step = *decl()[member];
    if(step.generated != Function::Generated::IterNext &&
       step.generated != Function::Generated::IterErasedNext) {
        return true;
    }

    if(Shapes::isEmpty(step.result.shape)) return true;

    // The payload's own position is `[field]` and is the class's `source'item`; an interior one
    // is deeper, and one in slot 0 is in the object.
    for(auto slotPointer: decl()[step.result.shape]->slots.contents(decl())) {
        auto& position = *decl()[slotPointer];
        if(position.path.size() < 2 || position.contract.slot() != 0) continue;

        context.diagnostics.error(
            "%@ needs %@ to be steppable, and what it hands over refers into the object itself - a body generic in `Iterate` sees only %@ and could keep one past the next step, so step this object where its type is written"_v,
            source, context.findName(callee.name), typeText(context, decl(), arguments[0]),
            typeText(context, decl(), arguments[1]));
        return false;
    }

    return true;
}

/*
 * `f$take` - the item moved out of the slot, and `$held` cleared in the same breath.
 *
 *      fn f$take(&self: f.Iter) -> ->T
 *
 * A declaration for `f$item`'s reason said about a move: a `for` that read the field directly would
 * move out of `it.$slot` and then go round the back edge, and nothing in MIR says that the next step
 * refills the path - so the ownership analysis reported the second pass as a use after a move. The
 * call says only that the record was written through, which the step beside it says too.
 *
 * Only where the record carries `$held` (`RecordType::heldField`). Where the hand-over owns nothing
 * the slot is a copy and a plain field read is what it should be.
 */
bool Resolver::iterTake(Function& producer, RecordType& record, const Occurrence& handover) {
    auto pointer = iterGenerated(producer, record, "$take"_v, Function::Generated::IterTake);
    auto take = decl()[pointer];
    auto recordType = asBase<Type>(DeclPtr<RecordType>(&record - *program.decl));

    auto self = new (program.decl) Arg();
    self->name = context.addUnqualifiedName("$self", 5);
    self->source = producer.source;
    self->index = 0;
    self->occurrence = borrowed(recordType, Access::Unique);
    take->args.push(program.decl, self - *program.decl);

    take->result = owned(handover.type);

    record.producerTake = pointer;
    return true;
}

} // namespace mir
