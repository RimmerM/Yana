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
 * from. Walking the chain rather than one step covers a subslice of a slice; the depth bound is
 * against a cycle a rewrite could introduce, since nothing in the resolver builds one.
 */
static void useSlot(Analysis& analysis, Effects& effects, U32 root) {
    for(auto depth = 0; root != maxLimit<U32> && depth < 8; depth++) {
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

// Reading a value that is the contents of a slot is a use of that slot. Aggregates travel through
// the IR as the value that produced them rather than as a load, so without this an owned record
// passed to a call would look dead at the point it was created.
static void useValue(Analysis& analysis, Effects& effects, ModulePtr<Value> value) {
    useSlot(analysis, effects, backingLocal(analysis, value));

    /*
     * Using a borrow is using what it borrows.
     *
     * `f(&xs)` reads `xs` at the call and not only where the borrow was created, which is the whole
     * of what a borrow *is* - and without saying so, a local whose last mention is the borrow
     * instruction looks dead one instruction later, so the drop pass is entitled to release it while
     * the callee still holds the address.
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
     */
    SmallArray<ModulePtr<Value>, 8> pending;
    pending.push(value);

    for(Size i = 0; i < pending.size() && i < 32; i++) {
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
                pending.push(place.pointer);
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
                pending.push(place.pointer);
            } else if(root != maxLimit<U32>) {
                auto slot = analysis.function.localAt(analysis.local, root);
                if(slot.value) pending.push(slot.value);
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
                pending.push(place.pointer);
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
                pending.push(place.pointer);
            }
        } else if(produced.kind == Value::Cast || produced.kind == Value::Bitcast) {
            /*
             * A cast of a reference is the same address written differently, so it carries the same
             * roots - which analyze_provenance says at its own `Cast` arm in the same words, and
             * this walk did not.
             *
             * What it cost: `stringData(s)` is a `borrow` of the string and a `cast` of that borrow
             * to `'StringData`, so a read through the result is a `LoadPlace` rooted in the *cast*.
             * The walk pushed the cast, found no arm for it, and stopped - so a `->` parameter whose
             * only mention was that borrow looked dead one instruction later, and the drop pass
             * released its run between the cast and the load through it. `"a" ++ copy("b")` answered
             * `"aa"`: the freed run was handed straight back to the accumulator's own growth, and
             * the append read the bytes it had just written there.
             *
             * Asked of the *operand* rather than of the result, because both directions are real:
             * `cast %borrow : 'StringData` narrows one reference to another, and
             * `bitcast(run.items) :: %U8` reads a typed pointer as bytes for `copyMemory`. Anything
             * else pushes a value with no roots to contribute and the walk ends there anyway, so the
             * test is a filter on work rather than on correctness.
             */
            auto from = ((InstUnary&)produced).from;
            if(from && refersToStorage(analysis, analysis.local[from]->type)) pending.push(from);
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
                    pending.push(operand);
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
                    pending.push(arg);
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
 * Only droppable types transfer. For everything else the write is a copy of bytes nobody is
 * responsible for, and saying it moved would make the source unusable for no reason.
 */
static void transferFrom(Analysis& analysis, Effects& effects, ModulePtr<Value> value) {
    auto root = backingLocal(analysis, value);
    if(root == maxLimit<U32>) return;

    // Through the view chain, like every other read: writing a slice into a record reads the array
    // the slice is a view of, so the array stays live to that point. See Local::viewOf.
    useSlot(analysis, effects, root);

    auto type = analysis.function.localAt(analysis.local, root).type;
    if(needsTeardown(analysis.module, type)) effects.moves.push(root);
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
            case Value::Alloc:
                // Allocating creates storage and puts nothing in it. What makes a slot owned is
                // the Init that follows, which is why `let x = e` is two instructions.
                //
                // A run's length is read here, though, and saying so is what keeps whatever computed
                // it live up to this point - see InstAlloc::extent.
                useValue(analysis, effects, ((InstAlloc&)instruction).extent);
                break;

            case Value::LoadPlace:
                useRoot(analysis, effects, ((InstLoadPlace&)instruction).place);
                break;

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

            case Value::Borrow:
                useRoot(analysis, effects, ((InstBorrow&)instruction).place);
                break;

            case Value::Move: {
                auto& moved = (InstMove&)instruction;
                useRoot(analysis, effects, moved.place);

                auto root = rootLocal(analysis, moved.place);
                if(root != maxLimit<U32>) effects.moves.push(root);
                break;
            }

            /*
             * Both places are read and both are written, and the state of neither changes. So both
             * are uses and neither is a def, a move or an init - the whole of what makes these two
             * the operations that need no lattice.
             *
             * A use is not nothing, though: it is what keeps the old contents live up to here, which
             * is what stops the last-use rule dropping a value that this instruction is about to
             * hand somewhere else. It is also what makes reading a moved-out slot through one of
             * these the use-after-move it is.
             */
            case Value::Swap: {
                auto& swap = (InstSwap&)instruction;
                useRoot(analysis, effects, swap.a);
                useRoot(analysis, effects, swap.b);
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

            case Value::Copy:
                useRoot(analysis, effects, ((InstCopy&)instruction).place);
                break;

            case Value::Drop:
                useRoot(analysis, effects, ((InstDrop&)instruction).place);
                break;

            case Value::Address:
                useRoot(analysis, effects, ((InstAddress&)instruction).place);
                break;

            case Value::Cast:
            case Value::Bitcast:
            case Value::Neg:
            case Value::Not:
            case Value::ByteSwap:
            case Value::CountBits:
            case Value::LeadingZeros:
            case Value::TrailingZeros:
            case Value::Sqrt:
            case Value::Abs:
            case Value::Trunc:
            case Value::Floor:
            case Value::Ceil:
            case Value::Round:
                useValue(analysis, effects, ((InstUnary&)instruction).from);
                break;

            case Value::Fma:
                useValue(analysis, effects, ((InstFma&)instruction).a);
                useValue(analysis, effects, ((InstFma&)instruction).b);
                useValue(analysis, effects, ((InstFma&)instruction).c);
                break;

            case Value::Sha256Rounds:
                useValue(analysis, effects, ((InstSha256Rounds&)instruction).state);
                useValue(analysis, effects, ((InstSha256Rounds&)instruction).feed);
                useValue(analysis, effects, ((InstSha256Rounds&)instruction).keys);
                break;

            case Value::ShaBinary:
                useValue(analysis, effects, ((InstShaBinary&)instruction).lhs);
                useValue(analysis, effects, ((InstShaBinary&)instruction).rhs);
                break;

            case Value::Add:
            case Value::Sub:
            case Value::Mul:
            case Value::MulHi:
            case Value::Div:
            case Value::Rem:
            case Value::Shl:
            case Value::Shr:
            case Value::Sar:
            case Value::Rol:
            case Value::Ror:
            case Value::And:
            case Value::Or:
            case Value::BitsUpTo:
            case Value::GatherBits:
            case Value::ScatterBits:
            case Value::Crc32:
            case Value::Xor:
            case Value::Cmp:
                useValue(analysis, effects, ((InstBinary&)instruction).lhs);
                useValue(analysis, effects, ((InstBinary&)instruction).rhs);
                break;

            // The vector kinds, whose operands are lanes and vectors and are read the way every
            // other computation's are: used, never handed over. A vector is TrivialCopy, so there is
            // no ownership in one for any of these to move.
            case Value::VecSplat:
                useValue(analysis, effects, ((InstVecSplat&)instruction).from);
                break;

            case Value::VecLane:
            case Value::VecWithLane:
                useValue(analysis, effects, ((InstVecLane&)instruction).from);
                useValue(analysis, effects, ((InstVecLane&)instruction).value);
                break;

            case Value::VecShuffle:
                useValue(analysis, effects, ((InstVecShuffle&)instruction).left);
                useValue(analysis, effects, ((InstVecShuffle&)instruction).right);
                break;

            case Value::VecReduce:
                useValue(analysis, effects, ((InstVecReduce&)instruction).from);
                break;

            case Value::Native:
                for(auto arg: ((InstNative&)instruction).args.contents(local)) {
                    useValue(analysis, effects, arg);
                }

                break;

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
                        transferFrom(analysis, effects, arg);
                    }

                    position++;
                }

                break;
            }

            case Value::CallDyn: {
                // The callee's code and environment are read out of the function value, which is
                // what keeps the value itself alive across the call it is used by.
                auto& call = (InstCallDyn&)instruction;
                useValue(analysis, effects, call.callable);
                useValue(analysis, effects, call.address);

                for(auto arg: call.args.contents(local)) useValue(analysis, effects, arg);
                break;
            }

            case Value::GenCall:
                for(auto arg: ((InstGenCall&)instruction).args.contents(local)) {
                    useValue(analysis, effects, arg);
                }

                break;

            case Value::Je:
                useValue(analysis, effects, ((InstJe&)instruction).cond);
                break;

            case Value::Ret:
                // Returning hands ownership to the caller. Without this the value would be dropped
                // on the way out and the caller handed released storage.
                transferFrom(analysis, effects, ((InstRet&)instruction).value);
                break;

            default:
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
 * One level deep, which is what the resolver produces: placeFor() recovers a place from a load
 * rather than loading again, so chains of these do not arise. The address an `addressOf` hands out
 * is a different matter - a raw pointer can be stored anywhere and outlive any extent this could
 * compute - and is unchecked by construction, which is what `%T` means.
 */
static void extendBorrowUses(Analysis& analysis) {
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

        for(auto user: instruction.uses(analysis.local)) {
            auto index = analysis.indexOf.get(U32(user));
            if(index) analysis.effects[index.unwrap()].uses.push(root);
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
