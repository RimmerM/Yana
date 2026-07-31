#include "opt_pass.h"

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
 * (`addPlaceUse` in resolve/block.cpp). So a local whose users are all writes has, by construction,
 * had its address taken by nothing, been passed to nothing, been borrowed by nothing and been
 * dropped by nothing - because each of those is an instruction, and each would be in the list. The
 * one thing the list cannot say is whether removing the writes drops a value on the floor, and that
 * is the `needsTeardown` test rather than an analysis.
 *
 * That argument rests entirely on the use list being complete, which as the IR arrives here it is
 * not - the drop pass inserts its instructions without going through `Block::add`. `rebuildUses` in
 * opt.cpp is what makes the premise true, and this pass is the reason it exists.
 *
 * That is also why this is safe where the analysis is conservative: a local written after being read
 * is not read-only and evaporates here anyway, since SSA renaming is what forwarding already did.
 *
 * ## What is deliberately not done
 *
 * A local read in a block other than the one that wrote it stays. Making that work means placing
 * phis, which is a real pass (see lower_promote.cpp for the shape of one) rather than a case of
 * this; until it exists such a local simply keeps its storage, which is the conservative answer and
 * not a wrong one.
 */

namespace {

/*
 * The fields of an aggregate this pass is willing to take apart, and how a place names one.
 *
 * `constructor` is the `Downcast` a record's field path begins with - `%p@Point.x` is a downcast to
 * `Point` followed by field `x` - and is absent for a bare tuple. Reproducing that exactly is the
 * point of computing it here rather than at each use: a path this pass invents has to be one the
 * backends already know how to walk.
 */
struct Fields {
    TypePtr content = nullptr;
    Maybe<U16> constructor;
    Size count = 0;

    bool exists() const { return content != nullptr; }
};

Fields fieldsOf(OptContext& opt, TypePtr type) {
    if(!type) return {};

    Fields fields;
    auto content = type;

    if(opt.global[type]->kind == Type::Record) {
        auto record = (RecordType*)opt.global[type];

        // A sum has more than one shape and only one of them is live, which is a question about the
        // discriminant rather than about the path. An enum has no content at all.
        if(record->layout != RecordType::Single || record->constructors.isEmpty()) return {};

        fields.constructor = Just(U16(0));
        content = record->constructors.get(opt.global, 0).content;
    }

    if(!content || opt.global[content]->kind != Type::Tup) return {};

    fields.content = content;
    fields.count = ((TupType*)opt.global[content])->fields.size();
    return fields.count ? fields : Fields {};
}

TypePtr fieldType(OptContext& opt, const Fields& fields, Size index) {
    return ((TupType*)opt.global[fields.content])->fields.get(opt.global, index).type;
}

// One field of an aggregate, as a place: the given root and path, then the constructor's downcast
// where there is one, then the field.
Place fieldPlace(OptContext& opt, Place base, const Fields& fields, U16 index) {
    Place result = base;

    // A fresh list rather than the one `base` holds, since several of these are built from one base
    // and a shared list would have every field appended to the same path.
    result.projections = {};
    for(Size i = 0; i < base.projections.size(); i++) {
        result.projections.push(opt.program.arena, base.projections.get(opt.local, i));
    }

    if(auto constructor = fields.constructor) {
        result.projections.push(opt.program.arena, Projection {
            ProjectionKind::Downcast, constructor.unwrap(), nullptr
        });
    }

    result.projections.push(opt.program.arena, Projection { ProjectionKind::Field, index, nullptr });
    return result;
}

// The local whose storage this value is, or nothing where the value is not an allocation.
Maybe<U32> allocatedLocal(OptContext& opt, ModulePtr<Value> value) {
    if(!value || opt.local[value]->kind != Value::Alloc) return Nothing();

    for(U32 i = 0; i < opt.function->localCount(); i++) {
        if(opt.function->localAt(opt.local, i).value == value) return Just(i);
    }

    return Nothing();
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
 */
bool splitAggregateWrite(OptContext& opt, Block& block, Size index, InstInit& write) {
    auto type = opt.local[write.value]->type;
    if(!type || needsTeardown(*opt.module, type)) return false;

    auto source = allocatedLocal(opt, write.value);
    if(!source) return false;

    auto fields = fieldsOf(opt, type);
    if(!fields.exists()) return false;

    // The destination has to name the whole of the same shape, which is what makes the field paths
    // below line up. Anything else is a write of an aggregate into part of another one, and the
    // types would have to be walked to say which part.
    auto destination = write.place;

    Array<Inst*> replacement;
    for(U16 i = 0; i < U16(fields.count); i++) {
        auto member = fieldType(opt, fields, i);

        auto loaded = createInst<InstLoadPlace>(*opt.module, *opt.function, block, write.source, 0,
                                                member, fieldPlace(opt, Place::inLocal(source.unwrap()), fields, i));

        auto stored = createInst<InstInit>(*opt.module, *opt.function, block, write.source, 0,
                                           opt.program.scalar.unit,
                                           fieldPlace(opt, destination, fields, i),
                                           (ModulePtr<Value>)(loaded - opt.local), write.kind);

        replacement.push(loaded);
        replacement.push(stored);
    }

    /*
     * Registered through `Block::add` rather than written into the list directly, because `add` is
     * what records every use - a use list this pass filled in by hand would be one more place for
     * the two directions to disagree. It appends, so the list is rebuilt afterwards with the new
     * instructions where the write was.
     */
    auto existing = block.instructions.size();
    for(auto instruction: replacement) block.add(*opt.module, instruction);

    Array<ModulePtr<Inst>> ordered;
    for(Size i = 0; i < existing; i++) {
        if(i == index) {
            for(auto j = existing; j < block.instructions.size(); j++) {
                ordered.push(block.instructions.get(opt.local, j));
            }
        }

        ordered.push(block.instructions.get(opt.local, i));
    }

    block.instructions.clear();
    for(auto instruction: ordered) block.instructions.push(opt.program.arena, instruction);

    // The write itself, now that what replaces it is in front of it.
    eraseInstruction(opt, (ModulePtr<Inst>)(&write - opt.local));

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

    Array<ModulePtr<Inst>> writes;

    for(auto user: opt.local[slot.value]->uses.contents(opt.local)) {
        auto& instruction = *opt.local[user];
        if(instruction.kind != Value::Init && instruction.kind != Value::Assign) return false;

        // The local appearing as the *value* of a write is a whole-aggregate read of it, which is
        // the case `splitAggregateWrite` exists to remove and this one must not mistake for a write.
        auto& write = (InstInit&)instruction;
        if(write.value == slot.value) return false;
        if(write.place.root != PlaceRoot::Local || write.place.local != index) return false;

        writes.push(user);
    }

    if(writes.isEmpty()) return false;

    for(auto write: writes) eraseInstruction(opt, write);
    eraseInstruction(opt, (ModulePtr<Inst>)slot.value);

    // The slot keeps its name and its type for anything that prints it, and stops claiming storage
    // that no longer exists. `scalarizable` in resolve/lower.cpp reads exactly this field to decide
    // whether a local has an allocation to take apart.
    slot.value = nullptr;
    opt.function->locals.set(opt.local, index, slot);

    return true;
}

}

void scalarizeLocals(OptContext& opt) {
    for(auto blockPointer: opt.function->blocks.contents(opt.local)) {
        auto block = opt.local[blockPointer];

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto pointer = block->instructions.get(opt.local, i);
            auto instruction = opt.local[pointer];
            if(instruction->kind != Value::Init && instruction->kind != Value::Assign) continue;

            // A whole-value write names the local with no path of its own; anything with a path is
            // already a field write.
            auto& write = (InstInit&)*instruction;
            if(write.place.projections.isNotEmpty()) continue;

            // The replacements land in front of the write and the write goes away, so the walk
            // continues over them - each is a field write with a path, which the test above skips.
            splitAggregateWrite(opt, *block, i, write);
        }
    }

    for(U32 i = 0; i < opt.function->localCount(); i++) eliminateDeadLocal(opt, i);
}
