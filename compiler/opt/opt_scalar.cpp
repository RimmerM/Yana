#include "opt_pass.h"
#include "../resolve/expr.h"

/*
 * Aggregates that stop being built.
 *
 * Two rewrites, and the second is the one worth having:
 *
 *  - a whole-aggregate write whose source is another local's storage becomes one write per field;
 *  - a local nothing reads, and whose type owes no teardown, loses its writes and its allocation.
 *
 * Neither is interesting alone. Together with the place forwarding in opt_place.cpp they take apart
 * the shape every constructed record has in this IR:
 *
 *      %v0 = alloc : Point        %p = alloc : Point
 *      init %local0@Point.x, 3    init %p@Point.x, 3      (nothing)
 *      init %local0@Point.y, 4 -> init %p@Point.y, 4  ->
 *      %p = alloc : Point         %v8 = load %p@Point.x
 *      init %p, %v0               ...                     %v11 = add 3, 1
 *
 * The splitting turns the temporary into field writes, forwarding answers every read from the value
 * that was written, and what is left is a local nothing reads - which is the second rule. The record
 * is never constructed on either target: on JS that is an object literal that no longer exists, and
 * natively an alloca and its stores.
 *
 * ## Why this needs no ownership analysis
 *
 * `scalarizable` in resolve/lower.cpp - the native-only pass this generalizes - asks
 * `OwnershipResult` whether a local is read-only, escapes, is droppable, or needs a stable address.
 * None of that is asked here, because the use list answers all of it and answers it exactly.
 *
 * Every instruction naming a place rooted in a local is recorded as a user of that local's `Alloc`
 * (`addPlaceUse` in resolve/edit.cpp). So a local whose users are all writes has, by construction,
 * had its address taken by nothing, been passed to nothing, been borrowed by nothing and been
 * dropped by nothing - because each of those is an instruction, and each would be in the list. The
 * one thing the list cannot say is whether removing the writes drops a value on the floor, and that
 * is the `needsTeardown` test rather than an analysis.
 *
 * That argument rests entirely on the use list being complete, and it is: every rewrite between the
 * resolver and this pass goes through `IrEditor` (resolve/edit.h), which is the whole reason that
 * type exists. It used not to be - the drop pass spliced its instructions into a block without
 * appending them - and a `rebuildUses` in front of the optimizer is what stood in for it.
 *
 * That is also why this is safe where the analysis is conservative: a local written after being read
 * is not read-only and evaporates here anyway, since SSA renaming is what forwarding already did.
 *
 * ## Where the reads go
 *
 * The rule above is the whole of this pass, and it says nothing about a local read anywhere - which
 * is why on its own it removes almost nothing. Something has to make the reads stop existing first,
 * and there are two passes that do: opt_place.cpp within a block, and opt_promote.cpp across them.
 * The second is what needed phis and is the reason it is a pass of its own rather than a case of
 * this one.
 *
 * That is worth stating because it is where the *ordering* in opt.cpp comes from. This pass runs
 * before promotion because promotion works on one place per field and a record written whole is one
 * place until `splitAggregateWrite` has taken it apart; and the removal that promotion earns is
 * therefore the next round's rather than this one's.
 */

namespace {

// The local whose storage this value is, or nothing where the value is not an allocation.
Maybe<U32> allocatedLocal(OptContext& opt, ModulePtr<Value> value) {
    if(!value || opt.local[value]->kind != Value::Alloc) return Nothing();

    for(U32 i = 0; i < opt.function->localCount(); i++) {
        if(opt.function->localAt(opt.local, i).value == value) return Just(i);
    }

    return Nothing();
}

/*
 * Whether the destination of an aggregate write is storage whose shape this stage knows.
 *
 * What the split needs of it is that appending a field to its path names that field of that storage.
 * An empty path - a whole local - satisfies it because the allocation is the storage, and a `Field`
 * satisfies it because a record's allocation covers all of its fields at once. `Deref` goes through a
 * pointer, `Index` may be any element, and `Property` on JS is a write to a host object that expects
 * the whole value rather than one field of it at a time, so none of those does.
 *
 * `Downcast` is the one that has to be asked about rather than answered, and it is where the first
 * attempt at this was wrong.
 *
 * A downcast to the sole constructor of a single-constructor record is a step within one allocation:
 * the constructor's storage *is* the record's storage on every target, which is why `fieldPlace`
 * builds one in front of every field it names. A downcast into a **sum** is not, and cannot be made
 * into one here: which storage a constructor's payload occupies is `compiler/repr`'s decision, and it
 * is entitled to fold the payload into the whole value - `Maybe(Point)` is `Point | null` on JS, where
 * the allocation for the `Maybe` is `null` and writing the payload's fields into it is writing
 * properties onto nothing. `NicheHost.yana` is exactly that program and said so.
 *
 * Asking the *type* whether it has one constructor is a question about the declaration rather than
 * about the layout, so it is one this stage may ask - see Analysis-Optimization.md §2(a). The answer
 * being "sum" is what makes the write a representation decision this pass has to leave alone.
 */
bool splittableDestination(OptContext& opt, const IndexSet& contained, const Place& place) {
    if(place.root != PlaceRoot::Local && place.root != PlaceRoot::Global) return false;

    auto& projections = const_cast<Place&>(place).projections;

    /*
     * A whole-local destination is the shape every `let` has - construct in a temporary, copy into
     * the binding - and splitting it is what this pass has always done, whatever becomes of the
     * local afterwards.
     *
     * A destination *inside* something is a nested construction, and there the split only pays if
     * the thing containing it is going to disappear too. Where it is not - a record handed to a
     * call, one with a `Sink` - splitting replaces one write with one per field and nothing removes
     * either, which costs twice: `codegen/js` can no longer emit the record as a single object
     * literal, and `opt_pack.cpp` sees one write per field where it had one per storage unit and
     * co-packs less. Both showed up as measured growth.
     *
     * Containment is the cheap form of "may yet disappear": a local whose address was handed to
     * nothing is one `eliminateDeadLocal` can still remove once its reads are gone. It is the same
     * question opt_promote.cpp asks and the same answer.
     */
    if(projections.isNotEmpty()) {
        if(place.root != PlaceRoot::Local) return false;
        if(place.local >= contained.size() || !contained[place.local]) return false;
    }

    auto scalarizable = true;

    // `step.owner` is the type the projection is taken of, which is what the walk already had in
    // hand - see resolve/place.h. Asking `placeType` for each prefix answered the same thing and
    // walked the path again to do it.
    walkPlace(*opt.module, *opt.function, place, [&](const PlaceStep& step) {
        auto decline = [&]() {
            scalarizable = false;
            return false;
        };

        if(step.broken) return decline();

        switch(step.kind) {
            case ProjectionKind::Field:
                return true;
            case ProjectionKind::Downcast:
                if(!step.owner || opt.global[step.owner]->kind != Type::Record) return decline();
                if(((RecordType*)opt.global[step.owner])->layout != RecordType::Single) return decline();
                return true;
            default:
                return decline();
        }
    });

    return scalarizable;
}

/*
 * Whether a whole-aggregate write of this value into this destination is one the split below can
 * perform - which is the same question the sink two functions down has to ask about a value it is
 * *considering* writing there, and the reason this is a predicate rather than a run of early
 * returns inside the rewrite.
 *
 * Only where the source is a local's own storage, because that is the only source whose fields are
 * places this can name. A call result or a copy is a value of an aggregate type and there is nothing
 * to project out of it without storage to project *from* - which is the storage this is trying to
 * remove.
 *
 * `needsTeardown` is the whole of the ownership condition. A field-wise write relocates each field
 * exactly as the whole-value write relocated all of them, so the two differ only where relocating is
 * a call rather than the bytes - an authored `Sink`, or a member with one - and a type with either
 * needs a teardown by construction.
 */
bool splittableSource(OptContext& opt, const Place& destination, ModulePtr<Value> value) {
    auto type = opt.local[value]->type;
    if(!type || needsTeardown(*opt.module, type)) return false;

    if(!allocatedLocal(opt, value)) return false;

    auto fields = fieldsOf(opt, type);
    if(!fields.exists()) return false;

    /*
     * A nested write of a value the target co-packs, which is where splitting costs rather than pays.
     *
     * `Two {f: Flags {a, b}, g: Flags {a, b}}` writes a whole `Flags` into `t.f`, and a `Flags` whose
     * two booleans share a storage unit is *one* read-modify-write of that unit. Split into `t.f.a`
     * and `t.f.b` it is two, each of which reads the word back to preserve the bit it does not own -
     * so the same program emitted twice the memory traffic. `ScalarRecord.yana` measured it as 448
     * more bytes of JavaScript and nothing gained.
     *
     * Asked only of a nested destination, and deliberately. Splitting a *whole-local* write is what
     * makes every `let p = Point {...}` disappear and is the reason this pass exists; whether it too
     * should leave a packed aggregate whole is a real question with a different answer at stake, and
     * it wants its own measurement rather than being changed on the way past.
     */
    if(const_cast<Place&>(destination).projections.isNotEmpty()) {
        for(U16 i = 0; i < U16(fields.count); i++) {
            auto field = opt.repr.fieldOf(fields.content, i);
            if(field && field->isPacked()) return false;
        }
    }

    return true;
}

/*
 * Replacing a whole-aggregate write with one write per field.
 *
 * The destination may be a field rather than a whole local, which is what takes a *nested* record
 * apart: `Early {later: Later {value: 6}}` builds the inner record in a temporary and writes the
 * whole of it into `later`, so the outer record survives until that write is one write per field of
 * the inner one. The paths line up because an `Init`'s value has its place's type, so the fields
 * being walked are the fields of the storage being written either way.
 */
bool splitAggregateWrite(OptContext& opt, Block& block, Size index, InstInit& write) {
    if(!splittableSource(opt, write.place, write.value)) return false;

    auto source = allocatedLocal(opt, write.value).unwrap();
    auto fields = fieldsOf(opt, opt.local[write.value]->type);
    auto destination = write.place;

    InstList replacement;
    for(U16 i = 0; i < U16(fields.count); i++) {
        auto member = fieldType(opt, fields, i);

        auto loaded = createInst<InstLoadPlace>(*opt.module, *opt.function, block, write.source, StringId(),
                                                member, fieldPlace(opt, Place::inLocal(source), fields, i));

        auto stored = createInst<InstInit>(*opt.module, *opt.function, block, write.source, StringId(),
                                           opt.program.scalar.unit,
                                           fieldPlace(opt, destination, fields, i),
                                           (ModulePtr<Value>)(loaded - opt.local), write.kind);

        replacement.push(loaded);
        replacement.push(stored);
    }

    opt.ir().insert(block, index, replacement);

    // The write itself, now that what replaces it is in front of it.
    opt.ir().eraseInstruction((ModulePtr<Inst>)(&write - opt.local));

    opt.changed = true;
    return true;
}

/*
 * SROA over a phi of aggregate locals.
 *
 * This is the shape an inlined function returning a record leaves behind, and it is the one aggregate
 * the two rules above cannot get at between them. `wrap` in Is.yana builds a `Maybe(Int)` in one arm
 * and another in the other, and inlining it into `viaCall` leaves the join holding
 *
 *      %v13 = phi [b2, %v21], [b1, %v17] : Maybe(Int)
 *      init %local0, %v13
 *      %v2 = load %local0.discriminant
 *      %v  = load %local0@Just
 *
 * `splitAggregateWrite` declines that write twice over. A phi is not a local's storage, so there is
 * no place to project a field out of; and `Maybe(Int)` is a **sum**, whose fields are not a shape
 * this stage may write into at all - which storage a constructor's payload occupies is
 * `compiler/repr`'s decision, and `splittableDestination` says so at length. So the record stays in
 * memory, every read of it stays a load, and two allocations survive a function whose whole body is
 * `value > 0 ? value : 0`.
 *
 * Both of those go away by turning the phi round: instead of merging the *storage* and reading the
 * merged copy, read each alternative in the predecessor that produced it and merge the **values**.
 * One phi per place the destination is read at, which is a phi of loadable values rather than a phi
 * of allocations, and nothing is ever written into a sum's storage - the reads that were legal off
 * the copy are the same reads off the thing copied.
 *
 * The other way round is to sink the copy onto the edges, which is smaller - no phi is invented - and
 * strictly weaker: what it leaves in each predecessor is a whole-aggregate write, which is the rule
 * above, which declines a sum. It is the right transform for a single-constructor record and this one
 * subsumes it wherever the reads are in the join.
 *
 * ## What makes it sound
 *
 * **The copy is the phi's only reader, and it writes a whole local.** With another reader the phi
 * survives and so does every allocation feeding it, so there is nothing to gain; with a *nested*
 * destination the storage being read is not the storage the phi named.
 *
 * **Every reader of that local is a load in this block.** This is the clause that makes moving the
 * loads onto the edges an identity rather than speculation: a load in the join runs exactly when the
 * join runs, and the join runs exactly when one of its predecessors ran. A read further down would be
 * one this hoists above the branch that guards it, which on a niche-folded representation is a load
 * through a payload that is not there.
 *
 * **Every predecessor leaves exactly one way, and it is here.** The other half of that identity: a
 * load placed at the end of a block with a second successor also runs on the path that never reached
 * the join. `soleSuccessor` answers null for `je %c, X, X` too, where there are two edges rather than
 * one.
 *
 * **Every alternative is a local nothing outside its own predecessor touches.** The load is moved to
 * the end of that predecessor, so what it has to be worth is that the storage says the same thing
 * there as it did at the join. Nothing writes it after the block that built it - every user of the
 * allocation is in that block, or is the phi - so it does. This is stronger than containment and
 * cheaper to check, and it is the condition that actually matters: containment is about callees,
 * while what could go wrong here is a store in the join itself.
 */
bool splitPhiOfLocals(OptContext& opt, const IndexSet& contained, Block& block,
                      ModulePtr<InstPhi> pointer) {
    auto here = (ModulePtr<Block>)(&block - opt.local);
    auto phi = opt.local[pointer];

    if(!phi->type || needsTeardown(*opt.module, phi->type)) return false;
    if(phi->useCount() != 1) return false;

    ModulePtr<Inst> user = nullptr;
    for(auto each: phi->uses(opt.local)) user = each;

    auto instruction = opt.local[user];
    if(instruction->kind != Value::Init && instruction->kind != Value::Assign) return false;
    if(instruction->block != here) return false;

    auto& write = (InstInit&)*instruction;
    if(write.value != (ModulePtr<Value>)pointer) return false;
    if(write.place.root != PlaceRoot::Local || write.place.projections.isNotEmpty()) return false;

    auto destination = write.place.local;
    if(destination >= contained.size() || !contained[destination]) return false;

    auto slot = opt.function->localAt(opt.local, destination);
    if(!slot.value || opt.local[slot.value]->kind != Value::Alloc) return false;

    /*
     * The alternatives, one per incoming edge and in that order rather than in the phi's own - which
     * is the order the phis built below have to be in, since that is how lowering and codegen/js
     * match an alternative to the edge it arrives over. The two agree today; reading the edges is
     * what keeps this from depending on that.
     */
    auto& inputs = ((InstPhi*)phi)->inputs;
    if(inputs.size() != block.predecessorCount() || inputs.size() == 0) return false;

    SmallArray<U32, 4> sources;
    for(Size i = 0; i < block.predecessorCount(); i++) {
        auto predecessor = block.predecessorAt(opt.local, i);
        if(opt.local[predecessor]->soleSuccessor() != here) return false;

        Maybe<U32> source;
        for(Size at = 0; at < inputs.size(); at++) {
            auto input = inputs.get(opt.local, at);
            if(input.block != predecessor) continue;

            // A phi naming itself is a loop-carried alternative, and there is no block to read one
            // out of - the value arriving over the back edge is the phi's own from the last time.
            if(input.value == (ModulePtr<Value>)pointer) return false;

            source = allocatedLocal(opt, input.value);
            break;
        }

        if(!source) return false;

        auto& alternative = *opt.local[opt.function->localAt(opt.local, source.unwrap()).value];
        for(auto each: alternative.uses(opt.local)) {
            if(each == (ModulePtr<Inst>)pointer) continue;
            if(opt.local[each]->block != predecessor) return false;
        }

        sources.push(source.unwrap());
    }

    SmallArray<ModulePtr<Inst>, 8> reads;
    for(auto each: opt.local[slot.value]->uses(opt.local)) {
        if(each == user) continue;

        auto& read = *opt.local[each];
        if(read.kind != Value::LoadPlace || read.block != here) return false;

        auto& load = (InstLoadPlace&)read;
        if(!staysInFrame(opt, contained, load.place)) return false;
        if(load.place.local != destination) return false;
        if(!holdsLoadableValue(opt, load.type)) return false;

        // A read of the *whole* local is declined for opt_promote.cpp's reason rather than for one of
        // this rule's: such a place is a scalar local, which both targets already have a better form
        // for, and the phi this would build is worse than the `var` JS already has. It cannot arise
        // for the aggregates this exists for - a whole-aggregate read is not a loadable value - so
        // this is what keeps the two passes saying one thing about scalars.
        if(load.place.projections.isEmpty()) return false;

        reads.push(each);
    }

    if(reads.isEmpty()) return false;

    // One phi per place, not per read: two reads of `%local0.discriminant` are one merged value, and
    // CSE cannot say so afterwards because two phis are two definitions rather than one expression.
    struct Merged {
        Place place;
        TypePtr type;
        ModulePtr<Value> value;
    };

    SmallArray<Merged, 8> merged;

    for(auto each: reads) {
        auto& load = (InstLoadPlace&)*opt.local[each];

        ModulePtr<Value> value = nullptr;
        for(auto& done: merged) {
            if(done.type == load.type && samePlace(opt, done.place, load.place)) value = done.value;
        }

        if(!value) {
            auto joined = createInst<InstPhi>(*opt.module, *opt.function, block, load.source,
                                              StringId(), load.type);

            for(Size i = 0; i < sources.size(); i++) {
                auto predecessor = block.predecessorAt(opt.local, i);

                // The same path off another root. A `Place` is a root and a projection list, and the
                // list is the one the read already names - the alternatives have the destination's
                // type, so the path that walked one walks the other.
                auto place = load.place;
                place.local = sources[i];

                auto lifted = createInst<InstLoadPlace>(*opt.module, *opt.function,
                                                        *opt.local[predecessor], load.source,
                                                        StringId(), load.type, place);

                opt.ir().append(*opt.local[predecessor], lifted);
                joined->inputs.push(opt.program.arena,
                                    PhiInput { predecessor, (ModulePtr<Value>)(lifted - opt.local) });
            }

            // Detached until its alternatives exist, for the reason IrEditor::append states: appending
            // is what records them as uses.
            opt.ir().append(block, joined);

            value = (ModulePtr<Value>)(joined - opt.local);
            merged.push(Merged { load.place, load.type, value });
        }

        opt.ir().replaceValue((ModulePtr<Value>)each, value);
        opt.ir().eraseInstruction(each);
    }

    /*
     * The copy and the phi are left where they are. Nothing reads the destination any more, so
     * `eliminateDeadLocal` below removes the write and the allocation on this same pass, and the phi
     * that then reads nothing goes in `eliminateDeadValues` - which is also what releases the
     * alternatives, since being named by the phi was the last thing keeping them alive.
     */
    opt.changed = true;
    return true;
}

/*
 * The same copy, sunk onto the edges instead - which is what is left where the rule above declines.
 *
 * Its clause about the readers is the one that costs coverage: a read *below* the join is one the
 * phi form would have to hoist above whatever guards it. Nothing has to move for a **write** to sink,
 * though, so the shape that rule leaves alone is one this one can still take apart, one step less
 * directly: a copy at the end of each predecessor is a whole-aggregate write of a local's storage,
 * which `splitAggregateWrite` turns into field writes on the next round, and opt_promote.cpp then
 * carries those across the blocks the reads are in.
 *
 * Strictly weaker where both apply, so it runs second. What it produces is stores into the
 * destination, which have to be promoted away again; and `splittableSource` declines a *sum*
 * outright, since a field-wise write into one is a representation decision this stage may not take -
 * see `splittableDestination`. So this is the single-constructor record whose readers are spread
 * over blocks, and nothing else.
 *
 * The soundness clauses are the phi rule's minus the readers. The write must be the first instruction
 * in the block, so there is nothing above it to reorder against and no read of the destination that
 * would now see the value; and every predecessor must leave exactly one way, so a copy at the end of
 * one runs on the paths the join ran on and no others. That second clause is also what makes the
 * destination's storage reachable: a place written at the top of the join is rooted in something that
 * dominates the join, and a predecessor whose only exit is the join lies on every path to it.
 */
bool sinkWriteIntoPredecessors(OptContext& opt, Block& block, InstInit& write) {
    auto here = (ModulePtr<Block>)(&block - opt.local);

    auto value = opt.local[write.value];
    if(value->kind != Value::Phi) return false;
    if(value->block != here || value->useCount() != 1) return false;

    auto& phi = (InstPhi&)*value;
    if(phi.inputs.size() == 0) return false;

    for(Size i = 0; i < phi.inputs.size(); i++) {
        auto input = phi.inputs.get(opt.local, i);

        // A phi naming itself is a loop-carried alternative, and the block it arrives from is one
        // this write would then run in on every iteration rather than once.
        if(input.value == write.value) return false;
        if(opt.local[input.block]->soleSuccessor() != here) return false;

        // Asked of each of them before anything moves: a copy that lands in a predecessor and is
        // then not split is strictly worse than the one it replaced.
        if(!splittableSource(opt, write.place, input.value)) return false;
    }

    for(Size i = 0; i < phi.inputs.size(); i++) {
        auto input = phi.inputs.get(opt.local, i);
        auto predecessor = opt.local[input.block];

        auto copy = createInst<InstInit>(*opt.module, *opt.function, *predecessor, write.source,
                                         StringId(), opt.program.scalar.unit, write.place,
                                         input.value, write.kind);

        // `append` puts a non-terminator on the end of the instruction list, which is in front of the
        // jump - a terminator is not in that list at all. See Block, where the two are separate.
        opt.ir().append(*predecessor, copy);
    }

    opt.ir().eraseInstruction((ModulePtr<Inst>)(&write - opt.local));

    opt.changed = true;
    return true;
}

/*
 * A local nothing reads.
 *
 * "Reads" is every user that is not a write *through this local's own place*, which includes the
 * whole-value read a call argument is and the one an aggregate copy is. So the test is the use list
 * and one predicate about the type, and it needs no case for borrows, addresses, moves or drops -
 * each of those is a user and none of them is a write.
 *
 * An `InstAggregate` is a write here for the same reason the `Init`s it replaced were, and it has to
 * be: a literal built into a local nothing reads used to disappear one element at a time, so making
 * it one instruction would otherwise have made it *survive* - `sizeOf([Int *4])` grew by sixteen
 * instructions when fixed arrays were first routed through it. That is also what an allocation needs
 * before a managed target can stop giving it a manufactured initial value: the elision leaves the
 * allocation's contents entirely to the aggregate, so an aggregate nothing reads has to take the
 * allocation with it rather than keep it alive.
 */
bool eliminateDeadLocal(OptContext& opt, U32 index) {
    auto slot = opt.function->localAt(opt.local, index);
    if(!slot.value || !slot.type) return false;
    if(opt.local[slot.value]->kind != Value::Alloc) return false;

    /*
     * A parameter's storage belongs to the caller and is not this frame's to remove, however little
     * this frame does with it.
     *
     * A *closure environment* stood beside it on the reading that the storage belongs to the
     * function value built out of it. What makes that true is the environment word - an `addressof`
     * of this local, which is a user and not a write, so the walk below refuses such a local anyway
     * and the flag adds nothing while the closure exists. Once the last call through the closure has
     * been resolved and the function value has gone, the address goes with it and what is left is a
     * record of this frame's that nothing reads. See inlineDynamicCall and computeContainment, which
     * is the same statement asked about aliasing rather than about removal.
     */
    if(slot.borrowed) return false;

    /*
     * A *function value*, which is the one type owing a teardown that is left alone here.
     *
     * The rule the removal rests on is that nothing may drop what the writes put there, and the walk
     * below is what states it: every ownership instruction is a user, and by the time this pass runs
     * a drop is a `Drop` or the call `dischargeOwnership` turned one into - so a local anything tears
     * down has a use that is not a write and is refused two paragraphs down whatever its type.
     *
     * `needsTeardown` stood in front of that as a belt and its own comment said so. What it was
     * *also* doing was keeping a dead closure alive: a function value over borrowed captures has a
     * `teardown$none` header, so the ownership passes emit no drop for it at all, and the type-level
     * answer then said "owes a teardown" about a local that owes nothing and nothing reads. That is
     * exactly the local an inlined `calldyn` leaves behind - see inlineDynamicCall - and keeping it
     * keeps the `addressof` of its environment alive, which is what stops the environment's fields
     * from being forwarded and so stops the next call from resolving.
     *
     * Narrowed to function values rather than removed, because for every other type owing a teardown
     * the belt costs nothing: a `String` or a container local that reaches here has its drop in the
     * use list, so the two answers agree and only the second one is load-bearing.
     */
    if(needsTeardown(*opt.module, slot.type) && !isFunction(opt.global, slot.type)) return false;

    SmallArray<ModulePtr<Inst>, 8> writes;

    for(auto user: opt.local[slot.value]->uses(opt.local)) {
        auto& instruction = *opt.local[user];

        if(instruction.kind == Value::Aggregate) {
            auto& aggregate = (InstAggregate&)instruction;
            if(aggregate.place.root != PlaceRoot::Local || aggregate.place.local != index) return false;

            // The local as one of the *values* is the whole-value read the case below rejects, and
            // reaches this instruction the same way: an aggregate of aggregates.
            auto reads = false;
            eachAggregateComponent(opt.local, aggregate,
                                   [&](const AggregateComponent& component, Size) {
                reads = reads || component.value == slot.value;
            });

            if(reads) return false;

            writes.push(user);
            continue;
        }

        if(instruction.kind != Value::Init && instruction.kind != Value::Assign) return false;

        // The local appearing as the *value* of a write is a whole-aggregate read of it, which is
        // the case `splitAggregateWrite` exists to remove and this one must not mistake for a write.
        auto& write = (InstInit&)instruction;
        if(write.value == slot.value) return false;
        if(write.place.root != PlaceRoot::Local || write.place.local != index) return false;

        writes.push(user);
    }

    if(writes.isEmpty()) return false;

    for(auto write: writes) opt.ir().eraseInstruction(write);

    // The slot keeps its name and its type for anything that prints it, and stops claiming storage
    // that no longer exists - which the erase below does on its own, since removing an instruction
    // empties every slot it was the whole contents of. `scalarizable` in resolve/lower.cpp reads
    // exactly that field to decide whether a local has an allocation to take apart. Emptying it here
    // as well used to be the way this was written, and was worse than redundant: `slot` is a copy
    // taken before any of the erasing, so writing it back would put back whatever else had changed.
    opt.ir().eraseInstruction((ModulePtr<Inst>)slot.value);

    return true;
}

/*
 * A construction one of whose components is itself a record, taken back apart into its stores.
 *
 * `Nested {inner: Inner {a, b}, extra}` builds the inner record in a temporary and hands the whole of
 * it over as a component. That component is an aggregate *copy*, and copying is what this pass exists
 * to remove: split into a store per field of the inner record, the temporary has no readers left and
 * goes away with it. Kept as one instruction it is two allocations and two `copy`s of the same bytes -
 * seventy lowered instructions across the corpus when records first became one instruction.
 *
 * The whole aggregate is expanded rather than the one component, because what replaces the component
 * is `n` stores and the point of the instruction is that its components are written together. An
 * aggregate missing a field is one no target can build whole anyway, so there is nothing left to keep.
 *
 * The stores it becomes are the ones it replaced, so `splitAggregateWrite` reaches each of them on a
 * later iteration of the walk below and takes the nested one apart - which is the same recursion the
 * comment there describes, entered one step higher up.
 */
bool splitAggregate(OptContext& opt, Block& block, Size index, InstAggregate& aggregate) {
    auto splittable = false;
    eachAggregateComponent(opt.local, aggregate, [&](const AggregateComponent& component, Size) {
        auto type = opt.local[component.value]->type;
        if(!type || needsTeardown(*opt.module, type)) return;

        splittable = splittable ||
            (allocatedLocal(opt, component.value) && fieldsOf(opt, type).exists());
    });

    if(!splittable) return false;

    InstList replacement;
    eachWrittenComponent(opt.local, opt.module->arena, aggregate,
                         [&](Place place, ModulePtr<Value> value, Size) {
        replacement.push(createInst<InstInit>(
            *opt.module, *opt.function, block, aggregate.source, StringId(), opt.program.scalar.unit,
            place, value, Value::Init));
    });

    opt.ir().insert(block, index, replacement);
    opt.ir().eraseInstruction((ModulePtr<Inst>)(&aggregate - opt.local));

    opt.changed = true;
    return true;
}

}

void scalarizeLocals(OptContext& opt) {
    auto& contained = containmentOf(opt);

    /*
     * The phis, ahead of the walk rather than inside it, and the reason is which blocks it writes.
     * What it produces lands in the *predecessors* of the block it is looking at, which the walk
     * below has already passed - so doing it in place would leave every one of those loads for the
     * next round of the fixed point. Here they are all in place before the first block is split.
     */
    for(auto blockPointer: opt.function->blocks.contents(opt.local)) {
        auto block = opt.local[blockPointer];

        // The count taken up front, because what the rule appends is phis of its own: they land on
        // the end of this list, so the loop would otherwise walk each one and decline it.
        auto split = false;
        for(Size i = 0, count = block->phiCount(); i < count; i++) {
            split = splitPhiOfLocals(opt, contained, *block, block->phiAt(opt.local, i)) || split;
        }

        // Only where the phi rule did not fire, and only on the block's first instruction, which is
        // the copy's whole shape - see the note on `sinkWriteIntoPredecessors`, which is the weaker
        // of the two and takes what the other declined.
        if(split || block->instructionCount() == 0) continue;

        auto instruction = opt.local[block->instructionAt(opt.local, 0)];
        if(instruction->kind != Value::Init && instruction->kind != Value::Assign) continue;

        auto& write = (InstInit&)*instruction;
        if(!splittableDestination(opt, contained, write.place)) continue;

        sinkWriteIntoPredecessors(opt, *block, write);
    }

    for(auto blockPointer: opt.function->blocks.contents(opt.local)) {
        auto block = opt.local[blockPointer];

        for(Size i = 0; i < block->instructionCount(); i++) {
            auto pointer = block->instructionAt(opt.local, i);
            auto instruction = opt.local[pointer];

            if(instruction->kind == Value::Aggregate) {
                // Ahead of the store rule rather than beside it, and with the same `i--` for the same
                // reason: what it leaves behind is the stores, and one of them is the nested write
                // that rule takes apart.
                if(splitAggregate(opt, *block, i, (InstAggregate&)*instruction)) i--;
                continue;
            }

            if(instruction->kind != Value::Init && instruction->kind != Value::Assign) continue;

            auto& write = (InstInit&)*instruction;
            if(!splittableDestination(opt, contained, write.place)) continue;

            /*
             * The replacements land in front of the write and the write goes away, so the walk
             * continues over them - and that is what makes nesting work rather than needing a case:
             * a field that is itself a record produces a write of one, which this reaches on a later
             * iteration and splits again. It terminates because each step writes a strictly smaller
             * type, and a record cannot contain itself by value.
             *
             * A write this cannot use is not a write it has to recognize. Every one of the reasons
             * to decline is a property of the *value* being written - it is not a local's storage,
             * its type has no fields, its type owes a teardown - so a scalar field write simply
             * fails `fieldsOf` and costs a test.
             */
            splitAggregateWrite(opt, *block, i, write);
        }
    }

    for(U32 i = 0; i < opt.function->localCount(); i++) eliminateDeadLocal(opt, i);
}
