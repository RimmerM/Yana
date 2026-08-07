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
 * Replacing a whole-aggregate write with one write per field.
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
 *
 * The destination may be a field rather than a whole local, which is what takes a *nested* record
 * apart: `Early {later: Later {value: 6}}` builds the inner record in a temporary and writes the
 * whole of it into `later`, so the outer record survives until that write is one write per field of
 * the inner one. The paths line up because an `Init`'s value has its place's type, so the fields
 * being walked are the fields of the storage being written either way.
 */
bool splitAggregateWrite(OptContext& opt, Block& block, Size index, InstInit& write) {
    auto type = opt.local[write.value]->type;
    if(!type || needsTeardown(*opt.module, type)) return false;

    auto source = allocatedLocal(opt, write.value);
    if(!source) return false;

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
    if(write.place.projections.isNotEmpty()) {
        for(U16 i = 0; i < U16(fields.count); i++) {
            auto field = opt.repr.fieldOf(fields.content, i);
            if(field && field->isPacked()) return false;
        }
    }

    auto destination = write.place;

    InstList replacement;
    for(U16 i = 0; i < U16(fields.count); i++) {
        auto member = fieldType(opt, fields, i);

        auto loaded = createInst<InstLoadPlace>(*opt.module, *opt.function, block, write.source, StringId(),
                                                member, fieldPlace(opt, Place::inLocal(source.unwrap()), fields, i));

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

    // A parameter's storage belongs to the caller, and a closure's environment to the function value
    // built out of it. Neither is this frame's to remove, however little this frame does with it.
    if(slot.borrowed || slot.closureEnv) return false;

    // Removing the writes would drop whatever was written on the floor. A local of such a type has a
    // `Drop` naming it, so this is belt and braces - but the rule the removal rests on is this one
    // rather than the absence of an instruction.
    if(needsTeardown(*opt.module, slot.type)) return false;

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
    ScratchSet contained(opt.sets, 0);
    computeContainment(opt, *contained);

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
            if(!splittableDestination(opt, *contained, write.place)) continue;

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
