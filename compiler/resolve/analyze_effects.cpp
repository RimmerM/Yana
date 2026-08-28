#include "analyze_pass.h"

/*
 * The numbering, and what each instruction does to the locals it touches.
 *
 * Nothing here is a decision. It is the spine every other pass indexes by and the per-instruction
 * summary all of them read, which is why it is one file: an instruction added to the IR is added to
 * exactly one switch, and a pass that would have needed a case of its own is a pass that is asking
 * a question this file does not answer.
 */

void numberFunction(Analysis& analysis) {
    for(Size i = 0; i < analysis.blockCount(); i++) {
        auto block = analysis.blockAt(i);
        BlockRange range;
        range.first = U32(analysis.order.size());

        for(auto phi: block->phis(analysis.local)) analysis.order.push((ModulePtr<Inst>)phi);
        for(auto instruction: block->instructions(analysis.local)) analysis.order.push(instruction);
        if(block->terminator()) analysis.order.push(block->terminator());

        range.end = U32(analysis.order.size());
        analysis.blockRanges.push(range);
    }

    analysis.instructionCount = analysis.order.size();
    for(Size i = 0; i < analysis.instructionCount; i++) {
        *analysis.indexOf.add(U32(analysis.order[i])).value = U32(i);
    }
}

// The local a place is rooted in, or none. A global outlives every function and a raw pointer's
// target is outside the ownership model by definition, so neither contributes.
U32 rootLocal(Analysis& analysis, const Place& place) {
    if(place.root != PlaceRoot::Local) return maxLimit<U32>;
    return place.local < analysis.localCount ? place.local : maxLimit<U32>;
}

/*
 * One slot read, and everything that slot is a view of - see Local::viewOf.
 *
 * A slice is live storage that belongs to somebody else, so reading it reads the array it was taken
 * from. Walking the chain rather than one step covers a subslice of a slice.
 *
 * The bound is the frame's own width, and that is the difference between a guard and a cap. A chain
 * with no cycle in it visits each slot at most once, so `localCount` steps cannot be reached by a
 * well-formed frame and reaching them means a rewrite built a cycle - while a *fixed* bound is a
 * limit the program can hit, and hitting it drops a slot out of `uses` silently. What that costs is
 * not a missed optimization: a root that is not used is a root the drop pass releases early, which
 * is a use-after-free in a body whose only distinction was being a few slices deeper than the last.
 */
static void useSlot(Analysis& analysis, Effects& effects, U32 root) {
    for(Size depth = 0; root != maxLimit<U32> && depth <= analysis.localCount; depth++) {
        effects.uses.push(root);
        root = analysis.function.localAt(analysis.local, root).viewOf;
    }
}

static void useValue(Analysis& analysis, Effects& effects, ModulePtr<Value> value);

// The local a place names, and - when it names one through a borrow - whatever that borrow was
// taken of. `*(get(xs, i))` reads `xs`, and the chain from the read back to it is the same one
// useValue walks.
static void useRoot(Analysis& analysis, Effects& effects, const Place& place) {
    useSlot(analysis, effects, rootLocal(analysis, place));

    if(place.root == PlaceRoot::Borrow || place.root == PlaceRoot::Pointer) {
        useValue(analysis, effects, place.pointer);
    }
}

/*
 * The local a value is the contents of, or none. An aggregate that lives in storage is named by
 * the value that produced it - a call result, a copy, an allocation - which is what lets an SSA
 * operand be recognized as an owned slot.
 *
 * Read off the value rather than searched for. It was a scan of the local table for the slot whose
 * `value` matched, run once per operand per instruction per fixpoint round; `Value::slot` is the
 * same pairing recorded where it is made, which is `Function::addLocal`.
 */
U32 backingLocal(Analysis& analysis, ModulePtr<Value> value) {
    if(!value) return maxLimit<U32>;

    auto slot = analysis.local[value]->slot;
    if(slot >= analysis.localCount) return maxLimit<U32>;

    assertTrue(analysis.function.localAt(analysis.local, slot).value == value);
    return slot;
}

/*
 * Reading a value that is the contents of a slot is a use of that slot.
 *
 * Aggregates travel through the IR as the value that produced them rather than as a load, so without
 * this an owned record passed to a call would look dead at the point it was created.
 *
 * ## Why this is not `analyze_provenance.cpp`, which was tried
 *
 * Analysis-Borrows.md §8.8 named these two as one relation computed twice, and the merge is written
 * up there as the large remaining item. It was built, and it does not hold: **provenance and this are
 * different relations, and they part company at a hand-over.**
 *
 * Provenance answers *which storage may this value refer to*, and its `Move` arm rightly says the
 * moved value refers to the source's slot - the relocation is the bytes, so it does. This answers
 * *which slots does a use of this value read*, and after a move the source slot is **gone**: reading
 * the moved value is not a read of the place it came out of. Substituting one for the other made
 * `let &->out = lhs` in `Array.native.yana`'s `joinedArrays` report `lhs has been moved out of and
 * cannot be used again` at every later use of `out`, and took 249 fixtures with it.
 *
 * The safety argument in §8.8 was half right and the half it missed is the half that matters.
 * Over-approximating is harmless for *liveness* - a slot live longer is a drop placed later - but
 * `effects.uses` is also what `checkMoves` reads, and there an extra use is a false diagnostic rather
 * than a missed optimization. `Exchange` is the same shape for the same reason.
 *
 * What the two *do* share is every arm that does not cross a hand-over, and the fixpoint has four of
 * those this walk has not: `Phi`, `CallDyn`, `GenCall` and `Native`. Adding `Phi` was tried, on the
 * grounds that a merge crosses no hand-over and a use after the join is a use of every arm's roots.
 * It is **not** here, because no program could be written that needed it: `let b = if c then
 * borrowIt(x) else borrowIt(y)` answers correctly without it, and what it cost over the corpus was
 * 24 more lines of lowered IR for a hole nothing could reach. The other three are the same question
 * and are open on the same terms - an arm goes in when a body needs it, not because the fixpoint next
 * door has one.
 *
 * ## Using a borrow is using what it borrows
 *
 * `f(&xs)` reads `xs` at the call and not only where the borrow was created, which is the whole of
 * what a borrow *is* - and without saying so, a local whose last mention is the borrow instruction
 * looks dead one instruction later, so the drop pass is entitled to release it while the callee
 * still holds the address.
 *
 * Two steps, and the second is why one is not enough. A borrow may be taken of a borrow - what
 * convertBorrow's weakening of a mutable one produces - and a call may hand one *back*, which is
 * exactly what the `return` marker declares. `xs[i]` is both at once: a borrow of the array, a
 * call to `get` that returns a borrow rooted in it, and then a read of the element. The array is
 * read at that last point, and the marker is the only thing that says so.
 *
 * This is the same walk lastUseOf makes for exclusivity, over the same two rules. It is not
 * shared because the two answer different questions - that one is per borrow and produces an
 * extent, this one is per use and produces a set - and folding them together would make the
 * borrow checker's extent depend on the liveness it is not allowed to consult.
 *
 * Deduplicated on the way in rather than bounded on the way out. A `pending` that may hold the same
 * value twice is a worklist whose length is the *walk* rather than the graph, and a diamond - two
 * reads of one borrow, joined - doubles it at every level. The cap that used to stand here was
 * therefore a limit an ordinary body could reach, and past it the walk stopped without saying so,
 * which reads to the drop pass as "this root is not used here" and places the release before the last
 * read through it.
 */
static void useValue(Analysis& analysis, Effects& effects, ModulePtr<Value> value) {
    useSlot(analysis, effects, backingLocal(analysis, value));

    SmallArray<ModulePtr<Value>, 8> pending;
    auto reach = [&](ModulePtr<Value> next) {
        if(!next) return;
        for(auto seen: pending) { if(seen == next) return; }
        pending.push(next);
    };

    reach(value);

    for(Size i = 0; i < pending.size(); i++) {
        auto current = pending[i];
        if(!current) continue;

        auto& produced = *analysis.local[current];

        if(produced.kind == Value::Borrow || produced.kind == Value::Address) {
            Place place;
            if(!firstPlace(produced, place)) continue;

            // The shallow half of useRoot, deliberately: the deep half is this loop, and calling it
            // here would make the two mutually recursive with no bound but the shape of the body.
            useSlot(analysis, effects, rootLocal(analysis, place));

            if(place.root == PlaceRoot::Borrow || place.root == PlaceRoot::Pointer) {
                reach(place.pointer);
            } else {
                /*
                 * A borrow of a slot that is *itself* a view, which is what makes a chain of two
                 * calls carry a root the way one call does.
                 *
                 * `sum(slice(elements(xs), 1, 4))` builds the descriptor `elements` returned into a
                 * slot and then borrows that slot for `slice`, whose own `return self` says the
                 * result is rooted in it. Marking the slot used is not enough: what keeps `xs` alive
                 * is the *call* that filled it, and the arm below reads the declared roots off it.
                 * Without this the array's last use is the `elements` call, and the drop pass
                 * releases the run between it and the read through the subslice - a use-after-free
                 * whose only distinction is that the borrow reached its root through two calls
                 * rather than one.
                 *
                 * Guarded on the slot's type holding a borrow, which is the same test the loaded
                 * form below makes. A slot that owns its contents is the end of the chain and not a
                 * step in it, and following one would make every call that produced an owned value
                 * a use of whatever that call borrowed.
                 */
                auto root = rootLocal(analysis, place);
                if(root != maxLimit<U32>) {
                    auto slot = analysis.function.localAt(analysis.local, root);
                    if(slot.value && containsBorrowLike(analysis.module, slot.type)) reach(slot.value);
                }
            }
        } else if(produced.kind == Value::LoadPlace &&
                  containsBorrowLike(analysis.module, produced.type) &&
                  !isMemoryType(analysis.global, produced.type)) {
            /*
             * A *register* value holding a borrow, read back out of the storage a call wrote it
             * into - and the one shape the three branches below cannot see.
             *
             * `find(m, key) -> Maybe(&v)` declares `return self`, so the borrow inside the result is
             * rooted in the map. But the result is one word once Repr folds `Nothing` into the null
             * address, so it is *direct*: the call's result is a local, the match reads the payload
             * back out of that local, and the read is neither an aggregate load nor a pointer one.
             * Without this the walk stops at the local and the map is dead one instruction after the
             * call - so the drop pass releases it, and the borrow the match then reads through names
             * storage that has been handed back.
             *
             * Pushing the local's *defining* value is what closes it: for a call that value is the
             * call, and the `Value::Call` arm below reads the `return` group off its summary exactly
             * as it does for a bare `&T` result. For anything else - a local an `alloc` defined - it
             * is a value with no roots to contribute and the walk ends there anyway.
             */
            auto& place = ((InstLoadPlace&)produced).place;
            auto root = rootLocal(analysis, place);
            useSlot(analysis, effects, root);

            if(place.root == PlaceRoot::Borrow || place.root == PlaceRoot::Pointer) {
                reach(place.pointer);
            } else if(root != maxLimit<U32>) {
                auto slot = analysis.function.localAt(analysis.local, root);
                if(slot.value) reach(slot.value);
            }
        } else if(produced.kind == Value::LoadPlace && isMemoryType(analysis.global, produced.type)) {
            /*
             * A loaded aggregate *is* its place rather than a copy of it - findPlace answers with
             * the place a LoadPlace named, which is what keeps a field of a field one projection
             * path. So using the value is using the storage, for as long as the value is used.
             *
             * Memory types only. A scalar read out of a place is a copy in a register with no
             * relationship to where it came from, and following that would keep the owner of every
             * field anyone ever read live to the end of the function.
             */
            auto& place = ((InstLoadPlace&)produced).place;
            useSlot(analysis, effects, rootLocal(analysis, place));

            if(place.root == PlaceRoot::Borrow || place.root == PlaceRoot::Pointer) {
                reach(place.pointer);
            }
        } else if(produced.kind == Value::LoadPlace && isPointer(analysis.global, produced.type)) {
            /*
             * The one scalar for which the paragraph above is not true - and the ownership model
             * already has a name for it, since analyze_provenance's refersToStorage counts a raw
             * pointer alongside an aggregate and a borrow.
             *
             * A container's elements are reached through exactly this shape: `xs[i]` loads
             * `xs.run.items`, offsets it, and reads through the result. That used to be three
             * instructions inside `Index(Array(a)).get`, where the `return self` marker on the
             * callee's signature told the caller the result was rooted in the array; expanding the
             * accessor at the call site (Implementation-Simplification.md §2) puts the load in the
             * caller's own body, and without this the array is dead at that load and the drop pass
             * releases the run between it and the read through it.
             *
             * Conservative in the one direction that is safe: it delays a drop and never moves one
             * earlier. What it deliberately does not do is make a raw pointer *checked* - the target
             * of a `%T` is still outside the ownership graph, and a pointer written into a field and
             * read back somewhere else still refers to nothing this can compute.
             */
            auto& place = ((InstLoadPlace&)produced).place;
            useSlot(analysis, effects, rootLocal(analysis, place));

            if(place.root == PlaceRoot::Borrow || place.root == PlaceRoot::Pointer) {
                reach(place.pointer);
            }
        } else if(produced.kind == Value::Select) {
            /*
             * Either arm of a select, on the same terms as the cast below: which reference this is
             * is what the condition decided, and what either of them refers into is unchanged by
             * being chosen. `analyze_provenance` joins the two arms at its own `Select` arm.
             *
             * Reachable here because `settle` runs a full optimizer round on a callee before a site
             * is judged against it, so a body this analysis sees can already hold the collapsed form
             * of `if c then &a else &b`.
             */
            auto& select = (InstSelect&)produced;
            reach(select.whenTrue);
            reach(select.whenFalse);
        } else if(produced.kind == Value::Cast || produced.kind == Value::Bitcast) {
            /*
             * A cast of a reference is the same address written differently, so it carries the same
             * roots - which analyze_provenance says at its own `Cast` arm in the same words.
             *
             * What its absence cost: `stringData(s)` is a `borrow` of the string and a `cast` of that
             * borrow to `'StringData`, so a read through the result is a `LoadPlace` rooted in the
             * *cast*. The walk pushed the cast, found no arm for it, and stopped - so a `->` parameter
             * whose only mention was that borrow looked dead one instruction later, and the drop pass
             * released its run between the cast and the load through it. `"a" ++ copy("b")` answered
             * `"aa"`: the freed run was handed straight back to the accumulator's own growth, and the
             * append read the bytes it had just written there.
             *
             * Asked of the *operand* rather than of the result, because both directions are real:
             * `cast %borrow : 'StringData` narrows one reference to another, and
             * `bitcast(run.items) :: %U8` reads a typed pointer as bytes for `copyMemory`. Anything
             * else pushes a value with no roots to contribute and the walk ends there anyway, so the
             * test is a filter on work rather than on correctness.
             */
            auto from = ((InstUnary&)produced).from;
            if(from && refersToStorage(analysis, analysis.local[from]->type)) reach(from);
        } else if((produced.kind == Value::Add || produced.kind == Value::Sub) &&
                  isPointer(analysis.global, produced.type)) {
            /*
             * Offsetting a pointer does not change what it refers into, which is the other half of
             * the rule above: an element address is the base plus a scaled index, and the base is
             * what carries the root.
             *
             * Whichever operand is the pointer, rather than the left one. `p + n` is the only form
             * the pointer intrinsics build, and reading it off the type instead of the position is
             * both free and one fewer thing to keep in agreement with them.
             */
            auto& binary = (InstBinary&)produced;

            for(auto operand: { binary.lhs, binary.rhs }) {
                if(operand && isPointer(analysis.global, analysis.local[operand]->type)) {
                    reach(operand);
                }
            }
        } else if(produced.kind == Value::Call) {
            auto& call = (InstCall&)produced;
            auto summary = summaryOf(analysis, call.callee);
            if(!summary || !summary->declaredRoots) continue;

            U16 position = 0;

            for(auto arg: call.args.contents(analysis.local)) {
                if(summary->declaredRoots & rootBit(position)) {
                    useSlot(analysis, effects, backingLocal(analysis, arg));
                    reach(arg);
                }

                position++;
            }
        }
    }
}

/*
 * Ownership leaving this frame through a value.
 *
 * Writing an owned aggregate into another place, returning it, or merging it into a phi all hand
 * its contents to something else. The slot it came out of must not be dropped afterwards, or the
 * same storage is released twice - which for `Pair {left: makeBuffer(32), ...}` would be a double
 * free of the buffer the field now owns.
 *
 * Only types the program may not simply duplicate transfer. For everything else the write is a copy
 * of bytes nobody is responsible for, and saying it moved would make the source unusable for no
 * reason.
 *
 * That is two questions and it used to be asked as one. A droppable type is the case the paragraph
 * above is about, and for a long time it was the only way to be non-copyable - so `needsTeardown`
 * stood in for both. `Atomic(a)` is the first type that separates them: it releases nothing, every
 * content it admits being trivial, and it is deliberately not `TrivialCopy` because a copied atomic
 * is a *second location* and two threads synchronizing through two locations are not synchronizing.
 * Asked only about teardown, this recorded no transfer for one - so `Holder {flag: c}` duplicated
 * the atomic into the record and left `c` live and usable beside it, silently, which is the whole
 * of what §3.1 exists to prevent.
 */
static bool transfersOwnership(Analysis& analysis, TypePtr type) {
    if(needsTeardown(analysis.module, type)) return true;

    // Asked of the context rather than of the type, on sinkValue's terms: an unconstrained `a` is
    // non-TrivialCopy inside the body that names it however a caller later substitutes it.
    return !ownershipIn(analysis.module, functionGen(analysis.global, analysis.function), type).trivialCopy;
}

/*
 * The slot a departing operand reads out of, where it reads a whole one - Design-Test.md §11.1's P3.
 *
 * `backingLocal` cannot answer this: it matches a slot whose *defining value* is the operand, which
 * is true of a call result or a construction and false of a load. So `Failure {claim: claim}` handed
 * ownership on out of a named local and nothing above recorded that the local had been read there at
 * all - the last thing that touched it was the load, several instructions earlier.
 *
 * `maxLimit<U32>` for everything else, which includes a projected load. A projection is a partial
 * move and checkMoves rejects rather than represents one, so there is nothing here for it to be the
 * whole of.
 */
static U32 departingSlot(Analysis& analysis, ModulePtr<Value> value) {
    if(!value || analysis.local[value]->kind != Value::LoadPlace) return maxLimit<U32>;

    auto& place = ((InstLoadPlace&)*analysis.local[value]).place;
    if(place.root != PlaceRoot::Local || place.projections.size() != 0) return maxLimit<U32>;

    return place.local < analysis.localCount ? place.local : maxLimit<U32>;
}

static void transferFrom(Analysis& analysis, Effects& effects, ModulePtr<Value> value) {
    auto root = backingLocal(analysis, value);

    /*
     * A read of a whole slot, which is a *use* of it here whatever else it turns out to be.
     *
     * Recorded unconditionally and with no move beside it, and the two halves of that are what make
     * P3 land in the right order. The use has to exist before liveness runs, because a departure
     * that takes the value has to be inside the value's live range or the drop placer releases it
     * above the line that hands it on. Whether it *is* a move is a question about liveness, so it
     * cannot be answered here at all - see computeLastUseMoves, which answers it afterwards and
     * pushes the move then.
     */
    if(root == maxLimit<U32>) {
        auto departing = departingSlot(analysis, value);
        if(departing == maxLimit<U32>) return;

        // Only where ownership could travel. A trivially copyable scalar read into a record field is
        // a copy under every rule there is, so extending its live range to the write would be a
        // change to the printed ranges of nearly every function in the tree that decided nothing.
        auto type = analysis.function.localAt(analysis.local, departing).type;
        if(transfersOwnership(analysis, type)) useSlot(analysis, effects, departing);

        return;
    }

    // Through the view chain, like every other read: writing a slice into a record reads the array
    // the slice is a view of, so the array stays live to that point. See Local::viewOf.
    useSlot(analysis, effects, root);

    auto type = analysis.function.localAt(analysis.local, root).type;
    if(transfersOwnership(analysis, type)) effects.moves.push(root);
}

/*
 * The same, for an argument that reaches a callee with no `move` in front of it.
 *
 * An erased class dispatch applies no conventions - `emitGenericDispatch` says why, and it is a
 * decision about the ABI rather than an omission: the callee is not a function this site reaches,
 * so what a `->` costs is settled where the witness is loaded. What it still *does* is consume,
 * and the argument it consumes arrives as a plain load of the slot. `backingLocal` cannot see
 * through one - a load produces a value rather than being a slot's contents - so the place is
 * asked directly.
 *
 * The direct call takes the same route, for the case its own arm was written about: `cloneGenCall`
 * turns a dispatch into a concrete call at instantiation, and nothing puts a move in front of an
 * argument that did not have one.
 *
 * Only a whole local. A projection would be a partial move, which checkMoves rejects rather than
 * represents, and saying the whole slot moved would be a lie in the direction that leaks nothing
 * and releases something twice.
 */
static void transferFromArgument(Analysis& analysis, Effects& effects, ModulePtr<Value> value) {
    if(!value) return;

    auto& produced = *analysis.local[value];

    if(produced.kind == Value::LoadPlace) {
        auto& place = ((InstLoadPlace&)produced).place;

        if(place.root == PlaceRoot::Local && place.local < analysis.localCount &&
           place.projections.size() == 0) {
            useSlot(analysis, effects, place.local);

            auto type = analysis.function.localAt(analysis.local, place.local).type;
            if(transfersOwnership(analysis, type)) effects.moves.push(place.local);
            return;
        }
    }

    transferFrom(analysis, effects, value);
}

static void deriveEffects(Analysis& analysis) {
    auto local = analysis.local;
    analysis.effects.reset(analysis.order.size());

    for(Size index = 0; index < analysis.order.size(); index++) {
        auto& instruction = *local[analysis.order[index]];
        auto& effects = analysis.effects[index];

        // A value that owns storage of its own defines the slot recording it. That covers the
        // aggregate results - a call's, a copy's - which are created already filled rather than
        // allocated and then written into.
        auto produced = backingLocal(analysis, (ModulePtr<Value>)analysis.order[index]);
        if(produced != maxLimit<U32> && instruction.kind != Value::Arg) {
            // An allocation ends the slot's live range going backwards - nothing above it can be
            // reaching contents that did not exist - without making it owned, since it puts
            // nothing in the storage it creates. That is exactly the split between the two lists.
            effects.defs.push(produced);
            if(instruction.kind != Value::Alloc) effects.inits.push(produced);
        }

        switch(instruction.kind) {
            case Value::Init:
            case Value::Assign: {
                auto& write = (InstInit&)instruction;
                auto root = rootLocal(analysis, write.place);

                if(root != maxLimit<U32>) {
                    auto whole = write.place.projections.isEmpty();

                    if(whole) {
                        effects.defs.push(root);
                        if(instruction.kind == Value::Assign) effects.overwrites.push(root);
                    } else {
                        // A field write leaves the rest of the slot alone, so it reads as a use.
                        // That covers the liveness half of a field *assignment* as well - what such
                        // a write replaces is one field, and the drop for it is stated over that
                        // field's place rather than over the slot. See placeOverwriteDrops.
                        effects.uses.push(root);
                    }

                    // Filling a field is still what makes a constructed aggregate owned: there is
                    // no single instruction that initializes one as a whole.
                    if(whole || instruction.kind == Value::Init) effects.inits.push(root);
                }

                transferFrom(analysis, effects, write.value);
                break;
            }

            /*
             * Every element at once, and each of them a hand-over.
             *
             * Ownership-equivalent to the per-element `Init`s this replaces, and that is the whole
             * claim: those wrote through a *pointer*-rooted place, so `rootLocal` answered nothing
             * and the defs/inits half of the case above never ran. `transferFrom` was the only
             * effect they had, and it is the only one here.
             *
             * The run's own root is not used here for the same reason it was not there - writing
             * into storage a pointer names says nothing about a local.
             */
            case Value::Aggregate: {
                auto& aggregate = (InstAggregate&)instruction;

                /*
                 * The run's own root, where it has one.
                 *
                 * Nothing for a pointer-rooted place, which is what an `Array(a)`'s buffer is on
                 * either target - writing into storage a pointer names says nothing about a local.
                 * A `[T *n]` literal is the case that does: its elements *are* the local's storage,
                 * so this is the same use-and-init an element `Init` recorded, said once for all of
                 * them. It is `uses` rather than `defs` because the path is not empty - an element
                 * write leaves the rest of the slot alone, which is the rule the Init case states.
                 */
                auto root = rootLocal(analysis, aggregate.place);
                if(root != maxLimit<U32>) {
                    /*
                     * The same split the `Init` case makes, and for the same reason. A path into the
                     * slot leaves the rest of it alone, so it reads as a use; an *empty* path is the
                     * whole value, which is a definition - a sum built as one instruction is the
                     * shape that has one, and calling it a use made the analysis believe the slot
                     * held something before the construction ran.
                     */
                    if(aggregate.place.projections.isEmpty()) {
                        effects.defs.push(root);
                    } else {
                        effects.uses.push(root);
                    }

                    effects.inits.push(root);
                }

                eachAggregateComponent(analysis.local, aggregate,
                                       [&](const AggregateComponent& component, Size) {
                    transferFrom(analysis, effects, component.value);
                });

                break;
            }

            case Value::Move: {
                auto& moved = (InstMove&)instruction;
                useRoot(analysis, effects, moved.place);

                auto root = rootLocal(analysis, moved.place);
                if(root != maxLimit<U32>) effects.moves.push(root);
                break;
            }

            case Value::Exchange: {
                auto& exchange = (InstExchange&)instruction;
                useRoot(analysis, effects, exchange.place);

                // The incoming value goes into the place, so whatever owed a drop for it is now the
                // place's business - the same handover an Init of the same value would be.
                transferFrom(analysis, effects, exchange.value);
                break;
            }

            case Value::Call: {
                /*
                 * A default-convention argument is a borrow, so passing one keeps the caller's slot
                 * alive for the call without handing it over.
                 *
                 * A `->` argument is a handover, and it is recorded here as well as by the
                 * `InstMove` the resolver emits in front of it. **The move is not always there**:
                 * `cloneGenCall` turns a `gencall` into a direct call at instantiation, and an
                 * argument that was a constructed aggregate in the generic body arrives already
                 * built, so nothing put a move in front of it. Without this the frame goes on owning
                 * what the callee took - `push(xs, Cell {key: a})` in a body generic in `k` dropped
                 * the cell the array was holding, which is a use-after-free at every caller and
                 * reads as every entry being the same one.
                 *
                 * Saying it twice costs nothing: `moves` is a set of slots, and `transferFrom`
                 * declines anything with no teardown to hand over.
                 */
                auto& call = (InstCall&)instruction;
                auto callee = call.callee ? local[call.callee] : nullptr;
                U16 position = 0;

                for(auto arg: call.args.contents(local)) {
                    useValue(analysis, effects, arg);

                    auto declared = callee && position < callee->args.size()
                        ? local[callee->args.get(local, position)] : nullptr;

                    if(declared && declared->convention == ast::BindType::Sink) {
                        transferFromArgument(analysis, effects, arg);
                    }

                    position++;
                }

                break;
            }

            case Value::GenCall: {
                /*
                 * The same two facts the direct call above records, for the same reason and out of
                 * the same place: `InstGenCall::callee` is the class signature this dispatches to,
                 * and a signature is where a `->` is declared. Which *instance* runs is not known
                 * here and does not have to be - a class method's conventions are the class's, so
                 * every instance takes the argument the same way.
                 *
                 * The move the resolver emits in front of a `->` argument is **not** there on this
                 * path, unlike the concrete one: an erased class call passes the loaded value
                 * straight through. So this is the only record of the handover rather than the
                 * second one, and without it the frame goes on owning what the callee took -
                 * `acc = acc ++ copy(v)` released the accumulator it had just given away, once per
                 * iteration, which is a use-after-free in the middle of a fold.
                 *
                 * Invisible until now because a generic body got no drops at all (see
                 * analyzeFunction): nothing acted on the ownership this pass was getting wrong.
                 */
                auto& call = (InstGenCall&)instruction;
                auto callee = call.callee ? local[call.callee] : nullptr;
                U16 position = 0;

                for(auto arg: call.args.contents(local)) {
                    useValue(analysis, effects, arg);

                    auto declared = callee && position < callee->args.size()
                        ? local[callee->args.get(local, position)] : nullptr;

                    if(declared && declared->convention == ast::BindType::Sink) {
                        transferFromArgument(analysis, effects, arg);
                    }

                    position++;
                }

                break;
            }

            case Value::Ret:
                // Returning hands ownership to the caller. Without this the value would be dropped
                // on the way out and the caller handed released storage.
                transferFrom(analysis, effects, ((InstRet&)instruction).value);
                break;

            /*
             * A phi's operands are *not* used at the phi - see `attributePhiEdges`, which attributes
             * each of them to the end of the predecessor it arrives from instead. Named here so that
             * it stays out of the walk below, where it would be exactly the false use-after-move
             * that pass exists to avoid.
             */
            case Value::Phi:
                break;

            /*
             * Everything that only reads what it names, which is most of the IR: the loads, the
             * borrows, the computations, the two block operations, the atomics, the dynamic call and
             * the branch.
             *
             * The trait walk rather than an arm apiece. Those arms were the operand list of each
             * kind written out a second time, which is how `select` and `tableslot` came to have no
             * arm at all: neither existed when the switch was written, and "absent" reads here as
             * "reads nothing", so a witness slot's table was dead at the instruction that loaded it.
             *
             * The two halves are `useRoot` split in two, and together they are exactly it: the slot a
             * place is rooted in comes from `eachPlace`, and the value a borrow- or pointer-rooted
             * one starts at is an operand like any other, so `eachOperand` yields it. What is left
             * over is each kind's own operands.
             */
            default:
                eachPlace(instruction, [&](const Place& place) {
                    useSlot(analysis, effects, rootLocal(analysis, place));
                });

                eachOperand(local, instruction, [&](ModulePtr<Value> operand) {
                    useValue(analysis, effects, operand);
                });

                break;
        }

    }
}

/*
 * A value that refers into a slot keeps that slot alive for as long as the value is.
 *
 * An aggregate is never loaded into a register: `load %pair.left` produces the *address* of the
 * field, which is to say a borrow of it. So the slot is used wherever that value is used, not only
 * where the load was written - without this, `firstByte(pair.left)` would drop the pair between
 * taking the address of its field and reading through it.
 *
 * One level deep for the ordinary case, which is what the resolver produces: placeFor() recovers a
 * place from a load rather than loading again, so chains of these do not arise. The address an
 * `addressOf` hands out is a different matter - a raw pointer can be stored anywhere and outlive any
 * extent this could compute - and is unchecked by construction, which is what `%T` means.
 *
 * **The exception is a borrow written into storage that carries it onward**, and there the walk has
 * to follow. A capture reaches its call in three steps - into the environment, the environment's
 * address into the function value, the function value into the call - so a borrow whose own uses end
 * at the first of those is live until the last. Stopping at the first is what dropped a `String`
 * captured by a lens continuation between the capture and the call:
 *
 *     let text = made()      -- %local0
 *     let n = plusOne(4)     -- the rest of the block is the continuation, which reads `text`
 *
 * emitted `init %local1.text, %v3` and then `drop %local0` on the next line, because `%v3`'s only
 * use was that write. The continuation then read freed memory, natively and silently.
 *
 * `carriesLoanOnward` is the step, and it is analyze_borrow's - that pass already followed this
 * chain for the loan extent the conflict checks read, and the two disagreeing is what the defect
 * was. `seen` is against a cycle rather than against depth: an environment stored into a function
 * value stored into another environment is a chain, not a loop, but a rewrite could make one.
 */
static void extendBorrowUses(Analysis& analysis) {
    // Each step carries whether it was reached past a phi - see `Effects::joins`, which is where
    // such a step's attribution goes instead of `uses`. Sticky, because a merge does not become
    // exact again further down the chain.
    struct Carrier { ModulePtr<Value> value; bool merged; };

    SmallArray<Carrier, 16> pending;
    SmallArray<ModulePtr<Value>, 16> seen;

    for(Size i = 0; i < analysis.instructionCount; i++) {
        auto pointer = analysis.order[i];
        auto& instruction = *analysis.local[pointer];

        Place place;
        auto borrows = instruction.kind == Value::Borrow || instruction.kind == Value::Address ||
                       (instruction.kind == Value::LoadPlace &&
                        isMemoryType(analysis.global, instruction.type));

        if(!borrows || !firstPlace(instruction, place)) continue;

        auto root = rootLocal(analysis, place);
        if(root == maxLimit<U32>) continue;

        pending.clear();
        seen.clear();
        pending.push(Carrier { (ModulePtr<Value>)pointer, false });
        seen.push((ModulePtr<Value>)pointer);

        auto follow = [&](ModulePtr<Value> next, bool merged) {
            for(auto value: seen) { if(value == next) return; }

            seen.push(next);
            pending.push(Carrier { next, merged });
        };

        while(pending.size()) {
            auto carrier = pending[pending.size() - 1].value;
            auto merged = pending[pending.size() - 1].merged;
            pending.pop();

            for(auto user: analysis.local[carrier]->uses(analysis.local)) {
                auto& use = *analysis.local[user];

                /*
                 * A phi carries the loan on, and its operands are used on the edges into it rather
                 * than at the merge - this file's own rule, and the reason `deriveEffects` gives the
                 * phi an empty arm. Pushing the use at the phi is exactly the false use-after-move
                 * `attributePhiEdges` exists to avoid, and it is what `let f = if p then one else
                 * two` over two closures reported when this walk first ignored the distinction.
                 */
                if(use.kind == Value::Phi) {
                    auto& phi = (InstPhi&)use;

                    for(auto input: phi.inputs.contents(analysis.local)) {
                        if(input.value != carrier || !input.block) continue;

                        auto from = analysis.local[input.block];
                        if(!from->terminator()) continue;

                        auto edge = analysis.indexOf.get(U32(from->terminator()));
                        if(edge) analysis.effects[edge.unwrap()].uses.push(root);
                    }

                    /*
                     * And on past the merge - but only where the phi *carries* a reference rather
                     * than *moving* an owned value, which is the whole of the difference between
                     * the two things a phi can do with what this walk is following.
                     *
                     * A phi of owned values takes ownership: `let f = if p then one else two` over
                     * two closures leaves each environment's slot dead on the edge it arrived by,
                     * and the merged value is what owns one of them afterwards. The edges above are
                     * the exact statement of that and there is nothing past them to say - which is
                     * why `ClosurePhiDrop`'s two environments are live for one arm each. Continuing
                     * would make both live from entry to exit, since a slot live at a merge is live
                     * back through *every* predecessor, including the one that never built it.
                     *
                     * A phi of references owns nothing, and there the edges are not enough: the
                     * value is read *after* the merge, so the edge attribution alone puts the drop
                     * at the predecessor's `jmp`, one instruction before the block that reads
                     * through the reference. `yield if fits then values(buffer) else
                     * stringBytes(spilled)` in `Native/CString.native.yana` is that case - the root
                     * has to reach the continuation, and every later user of the merged value is
                     * attributed at itself by the arm below.
                     *
                     * Still not a use *at* the phi either way, which is the distinction this whole
                     * arm exists for - the merge attributes to its edges and to its consumers, and
                     * to nothing in between. That is what keeps a temporary built on one arm out of
                     * the move check: `if c then addressOf(buf[0]) else stringBytes(s)` goes through
                     * the `Flat(a)` a container access builds, which is `Uninitialized` on the other
                     * arm, and a use recorded at the merge reported it as "may have been moved out
                     * of on some paths reaching here" about a value nobody wrote down.
                     */
                    if(!transfersOwnership(analysis, use.type)) follow((ModulePtr<Value>)user, true);
                } else if(auto index = analysis.indexOf.get(U32(user))) {
                    auto& effects = analysis.effects[index.unwrap()];
                    if(merged) effects.joins.push(root); else effects.uses.push(root);
                }

                /*
                 * A call that *declares* it hands the root on, which is the `return` marker read
                 * from this side - Analysis-Language.md §1, and the same `declaredRoots` bit
                 * `useValue`'s `Call` arm reads one position at a time.
                 *
                 * The walk continues at the call's **result**, because that is what carries the root
                 * onward: `stringBytes(s)` answers an address into `s`, and every later use of that
                 * address is a use of `s`. Without this the walk stopped at the call - a call is
                 * neither an `Init` nor a `Borrow`, so neither arm below took it - and the root got
                 * no further. Where the next use is an ordinary instruction that cost nothing,
                 * since `useValue` reaches the same root through the same summary at that
                 * instruction; where the next use is a **phi** it was the whole difference, because
                 * `deriveEffects` gives a phi an empty arm on purpose and this walk is the only
                 * thing that attributes a phi's operands at all.
                 *
                 * What it cost: `yield if fits then values(buffer) else stringBytes(spilled)` in
                 * `Native/CString.native.yana` released `spilled` on the way into the merge, so the
                 * kernel was handed a free-list header and a path that existed reported `NotFound`.
                 * Silently, and only for a path long enough to spill.
                 *
                 * The position test is against the argument this walk is standing on rather than
                 * against every rooted position: a call with two rooted arguments carries each of
                 * them, and each is reached by the pass' own iteration over the one it starts from.
                 */
                if(use.kind == Value::Call) {
                    auto& call = (InstCall&)use;
                    auto summary = summaryOf(analysis, call.callee);

                    if(summary && summary->declaredRoots) {
                        U16 position = 0;

                        for(auto arg: call.args.contents(analysis.local)) {
                            if(arg == carrier && (summary->declaredRoots & rootBit(position))) {
                                follow((ModulePtr<Value>)user, merged);
                                break;
                            }

                            position++;
                        }
                    }
                }

                /*
                 * And on into whatever the use wrote the carrier into. `Init` into storage that
                 * carries takes the walk to that storage's own value; an `Address` or `Borrow` *of*
                 * an environment is the address the function value holds, so the walk continues at
                 * the result. Both arms are analyze_borrow's, stated the same way round.
                 */
                Place carried;
                auto storedInto = use.kind == Value::Init && firstPlace(use, carried);
                auto derivedFrom = (use.kind == Value::Address || use.kind == Value::Borrow) &&
                                   firstPlace(use, carried);

                if(!storedInto && !derivedFrom) continue;

                /*
                 * A **reborrow** - a borrow taken of the carrier rather than of a slot - refers into
                 * exactly the storage the carrier does, so the walk continues at it.
                 *
                 * It has no root local of its own, which is why this is tested before the lookup
                 * below rather than as a third arm after it: the place is `Borrow`-rooted, and
                 * `rootLocal` answers "not a local" for one, so the chain used to end here. That is
                 * how a *lens* lost a root that the same code in a plain function kept - the
                 * specialized body reads its parameter back and reborrows it,
                 *
                 *     %v17 = borrow %local4 : 'String
                 *     %v18 = load [%v17] : String
                 *     %v19 = borrow [%v17] : 'String
                 *     %v20 = call stringBytes, %v19 : %U8
                 *
                 * so the one extra step between the slot and the call was the whole difference.
                 *
                 * Exact rather than merged: a reborrow is the same storage in the same block, so it
                 * carries whatever the step it came from carried and adds no imprecision of its own.
                 */
                if(derivedFrom && carried.pointer == carrier &&
                   (carried.root == PlaceRoot::Borrow || carried.root == PlaceRoot::Pointer)) {
                    follow((ModulePtr<Value>)user, merged);
                    continue;
                }

                auto carrierRoot = rootLocal(analysis, carried);
                if(carrierRoot == maxLimit<U32>) continue;

                auto slot = analysis.function.localAt(analysis.local, carrierRoot);

                ModulePtr<Value> next = nullptr;
                if(storedInto && carriesLoanOnward(analysis, slot, analysis.local[carrier]->type)) {
                    next = slot.value;
                } else if(derivedFrom && slot.closureEnv) {
                    next = (ModulePtr<Value>)user;
                }

                if(next) follow(next, merged);
            }
        }
    }
}

/*
 * A phi's operands are used on the edges into it, not at the phi.
 *
 * Attributing them to the join block instead is the classic way to get a false use-after-move: at
 * the join, every arm's slot has been merged with the arms that never wrote it, so all of them read
 * as "owned on some paths". Attributing each operand to the end of the predecessor it comes from is
 * both what actually happens and what makes the state at the join say nothing at all about slots
 * that belong to one arm.
 */
static void attributePhiEdges(Analysis& analysis) {
    for(Size b = 0; b < analysis.blockCount(); b++) {
        auto block = analysis.blockAt(b);

        for(auto phiPointer: block->phis(analysis.local)) {
            auto& phi = *analysis.local[phiPointer];

            for(auto input: phi.inputs.contents(analysis.local)) {
                auto from = analysis.local[input.block];
                if(!from->terminator()) continue;

                auto index = analysis.indexOf.get(U32(from->terminator()));
                if(!index) continue;

                transferFrom(analysis, analysis.effects[index.unwrap()], input.value);
            }
        }
    }
}

/*
 * The three together, which is the only order they are ever wanted in: what each instruction does,
 * then the two corrections that make the answer the one the CFG has rather than the one the operand
 * list has.
 */
void computeEffects(Analysis& analysis) {
    deriveEffects(analysis);
    extendBorrowUses(analysis);
    attributePhiEdges(analysis);
}

/*
 * The last-use move - Design-Test.md §11.1's P3.
 *
 *     let &claim = "expected {expected}, got {actual}"
 *     record(Failure {site: at, claim: claim, detail: ""})
 *
 * A read of a whole owned local in a sink position is a move when the local is dead afterwards. Sink
 * positions are exactly the departure points `eachTransferOperand` walks - a record-literal field, an
 * array or tuple element, a `return`, an exchange, a phi input, the right side of an initializing
 * `let` - and "dead afterwards" is what liveness already computes.
 *
 * The effect is the one the neighbouring case in `transferFrom` already has, extended from a value
 * that has no name to a value that has one and does not need it any more. **There is no moved-from
 * state**: after the move the local is simply not there, nothing observes an emptied husk, and no
 * teardown runs on one - which is why the example needs no line after the constructor where C++
 * needs `std::move` *and* a destructor that tolerates the result.
 *
 * Moved on one path and live on another is a drop flag, which the lattice already builds: this
 * writes a `Moved` into the effects and the join does the rest. That machinery was previously
 * reachable only through the spellings this replaces.
 *
 * What it deletes is `exchange(x, empty)` as a move idiom, which matters more than the syntax:
 * `exchange` on an object-typed place returns an alias on JavaScript and the write that follows
 * destroys the value that was read. A language whose ordinary move is spelled with a defective
 * instruction is one bad `Array` field away from a silent miscompile.
 *
 * The cost, stated where it is paid: whether a departure is a move or an error now depends on
 * liveness, so adding a use later in a function can turn a working move into an error at a line the
 * author did not touch. That is Rust's model and it is the right trade - and it is why the
 * diagnostic checkTransfer prints for the other half of this rule names *both* the departure and the
 * later use, since the second one is otherwise invisible.
 */
void computeLastUseMoves(Analysis& analysis) {
    analysis.lastUseMoves.reset(analysis.function.valueCounter);

    auto& live = analysis.scratch.work;

    for(Size b = 0; b < analysis.blockCount(); b++) {
        auto range = analysis.blockRanges[b];
        if(range.end == range.first) continue;

        live.copyFrom(analysis.liveOut[b]);

        // Backwards, so that `live` is the set over the gap *following* the instruction being looked
        // at - which is exactly "afterwards". The step is applyBackward's, one instruction at a time.
        for(Size i = range.end; i > range.first; i--) {
            auto& instruction = *analysis.local[analysis.order[i - 1]];

            // Whether this departure is a move is one question and where it happens is another, and
            // for a phi input the two have different answers - see the loop below.
            auto departure = i - 1;

            auto mark = [&](ModulePtr<Value> operand) {
                auto root = departingSlot(analysis, operand);
                if(root == maxLimit<U32> || live[root]) return;

                // Only a slot this frame owns. Handing on out of borrowed storage or out of a global
                // is a different rule and a different diagnostic - checkTransfer's, which reaches
                // those before it reaches this.
                if(!analysis.tracked[root].owned) return;

                auto type = analysis.function.localAt(analysis.local, root).type;
                if(!transfersOwnership(analysis, type)) return;

                analysis.lastUseMoves.set(analysis.local[operand]->id, true);
                analysis.effects[departure].moves.push(root);
            };

            if(instruction.kind == Value::Phi) {
                /*
                 * A phi input departs on its *edge*, and recording the move at the phi instead was a
                 * use-after-free.
                 *
                 * `fn f(m: Maybe(Int)) -> String = match m: Just(i) -> "{i}"` builds the string in
                 * an arm and merges it: the arm's last instruction reads the whole slot for the phi,
                 * and "dead afterwards" is true, so the read is a move. Written into the phi's own
                 * effects that move lands in the *successor*, so the arm still owned the buffer at
                 * its terminator and the drop placer released it there - between the arm building
                 * the string and the merge reading it. The block-bodied spelling of the same
                 * function returns out of the arm and has no phi, which is why one form worked and
                 * the other handed back freed memory.
                 *
                 * The correction is attributePhiEdges' exactly, and for the same reason: the operand
                 * list says the use is at the phi and the CFG says it is on the edge, and everything
                 * the ownership lattice concludes is positional. **Only the position moves.** The
                 * liveness question stays where it is - `live` here is the set after the phi, which
                 * is "afterwards" for a value whose only reader is the merge, while the set after the
                 * predecessor's terminator holds this very input and would answer that every merged
                 * value is still live.
                 *
                 * Safe to write into a block this walk has not reached: `applyBackward` reads defs,
                 * uses, overwrites and joins, and never `moves` - the lattice is the only reader, and
                 * it runs after this.
                 */
                for(auto input: ((InstPhi&)instruction).inputs.contents(analysis.local)) {
                    auto from = analysis.local[input.block];
                    if(!from->terminator()) continue;

                    auto index = analysis.indexOf.get(U32(from->terminator()));
                    if(!index) continue;

                    departure = index.unwrap();
                    mark(input.value);
                }
            } else {
                eachTransferOperand(analysis.local, instruction, [&](ModulePtr<Value> operand, LocationId) {
                    mark(operand);
                });
            }

            applyBackward(analysis, i - 1, i, live);
        }
    }
}
