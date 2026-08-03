#pragma once

#include "generic.h"
#include "module.h"
#include "witness.h"

/*
 * One walk over a place path, and the only one that decides what each step arrives at.
 *
 * Every consumer of a `Place` used to walk it itself - the printer, four walks in `resolve/lower.cpp`,
 * the JS place walk, five passes in `compiler/opt`, `witness.cpp`, the drop pass and `placeType` -
 * and each one repeated the same `switch(ProjectionKind)` to carry a type along beside whatever it
 * was actually accumulating. The type stepping is not the interesting half of any of them, and it is
 * the half they had to agree about: `boxedStep` is a rule about `Field::boxed` that every walk has to
 * apply, and a walk that missed it did not fail to build. `pointeeType` answered null, the next
 * Downcast read constructor zero of nothing, and the assertion fired somewhere in `container.h`.
 *
 * So the stepping is written once here and each caller keeps only what is its own: an offset, an
 * address, a bit range, a property name, an alias question. `PlaceStep` carries the answers no
 * caller should work out for itself - what the owner was, what the step arrived at, and whether the
 * step crossed a box.
 *
 * Deliberately *not* about layout. A `Repr` is a target's answer and belongs to whoever emits; what
 * is shared is the structure, which is the same on every target. A caller that needs both asks this
 * for the type and its own `ReprTable` for the placement, with `PlaceStep::owner` as the type to ask
 * about - which is exactly the value each of those walks used to have to remember to keep.
 */

/*
 * One resolved step.
 *
 * `owner` is the type the path had before the step and `type` the type it has after it, so a caller
 * that wants "the type the last projection was taken of" reads the last step's `owner` rather than
 * running a second walk to one short of the end.
 */
struct PlaceStep {
    TypePtr owner = nullptr;
    TypePtr type = nullptr;

    ProjectionKind kind = ProjectionKind::Field;
    U16 index = 0;

    // The value that selects an element - an `Index`, and nothing else.
    ModulePtr<Value> value = nullptr;

    // Which projection this is, counting from zero.
    Size at = 0;

    /*
     * The step reached storage holding a `%T` rather than a `T` - `Field::boxed` or
     * `Constructor::boxed` - so `type` is that pointer and what names the value is the `Deref` the
     * resolver appended after this step. See boxedStep.
     *
     * Read by every walk that turns a step into an address or a bit range, because a boxed step is
     * a load rather than an offset and is never inside a packed word.
     */
    bool crossedBox = false;

    /*
     * The step could not be resolved: the owner was not the shape the projection needs, or a type
     * ran out on the way. `type` is null and the walk stops.
     *
     * A dump is asked about programs that did not resolve as well as ones that did, so this is a
     * reachable state rather than an assertion - and a caller that only runs on resolved programs
     * may treat it as one.
     */
    bool broken = false;
};

// What a place is rooted in, before any projection.
TypePtr placeRootType(Module& module, Function& function, const Place& place);

/*
 * The walk. `step` is handed each projection in order and answers whether to continue, which is what
 * lets a caller that has decided its own answer stop without walking the rest.
 *
 * `limit` stops after that many projections, which is how the owner of a place's *last* projection
 * used to be asked for. Prefer `PlaceStep::owner`, which answers the same question without a second
 * walk; `limit` remains for the callers that genuinely want a prefix, such as the address of the
 * owner a property witness is called with.
 */
template<class F>
void walkPlace(Module& module, Function& function, const Place& place, F&& step,
               Size limit = maxLimit<Size>) {
    auto global = *module.types;
    auto local = *module.arena;

    PlaceStep current;
    current.type = placeRootType(module, function, place);

    auto fail = [&]() {
        current.broken = true;
        current.type = nullptr;
        step(current);
    };

    auto projections = place.projections;

    for(auto projection: projections.contents(local)) {
        if(current.at >= limit) return;

        current.owner = current.type;
        current.kind = projection.kind;
        current.index = projection.index;
        current.value = projection.value;
        current.crossedBox = false;

        auto owner = current.owner;
        auto ownerKind = owner ? global[owner]->kind : Type::Error;

        switch(projection.kind) {
            case ProjectionKind::Discriminant:
                // An enum *is* its discriminant and a folded tag is not stored at all, but both of
                // those are facts about the representation. Structurally the step arrives at the
                // tag, which is an Int.
                current.type = module.scalar.int_;
                break;

            case ProjectionKind::Property: {
                /*
                 * What the constraint promised the field holds. The owner is not consulted: the
                 * whole point of the slot is that the owner is not known here.
                 */
                auto env = functionGen(global, function);
                if(!env) return fail();

                auto& schema = genSchemaOf(module, *env);
                TypePtr result = nullptr;

                for(auto slot: schema.slots.contents(global)) {
                    if(slot.kind == GenSlotKind::Property && slot.index == projection.index) {
                        result = slot.result;
                    }
                }

                if(!result) return fail();
                current.type = result;
                break;
            }

            case ProjectionKind::Deref: {
                auto pointee = pointeeType(global, owner);
                if(!pointee) return fail();

                current.type = pointee;
                break;
            }

            case ProjectionKind::Downcast: {
                if(ownerKind != Type::Record) return fail();

                auto record = (RecordType*)global[owner];
                if(projection.index >= record->constructors.size()) return fail();

                auto constructor = record->constructors.get(global, projection.index);
                current.crossedBox = constructor.boxed;
                current.type = boxedStep(module, constructor.content, constructor.boxed);
                break;
            }

            case ProjectionKind::Field: {
                /*
                 * A function value is two addresses and a header, and they are projected into rather
                 * than being a representation only lowering knows about - which is what lets the
                 * same Init, LoadPlace and Drop machinery build one, read one and tear one down.
                 */
                if(ownerKind == Type::Fun) {
                    if(projection.index >= FunValueLayout::kProjectionCount) return fail();

                    current.type = funValueFieldType(module, projection.index);
                    break;
                }

                if(ownerKind != Type::Tup) return fail();

                auto tuple = (TupType*)global[owner];
                if(projection.index >= tuple->fields.size()) return fail();

                auto field = tuple->fields.get(global, projection.index);
                current.crossedBox = field.boxed;
                current.type = boxedStep(module, field.type, field.boxed);
                break;
            }

            case ProjectionKind::Index:
                // A fixed array steps *into* itself and a run steps *along*, which is the one split
                // this walk draws that is not a type change: the element type of a `%a` or a
                // reference is already what the path had - Implementation-Containers.md §14.1.
                if(ownerKind == Type::Array) current.type = ((ArrayType*)global[owner])->content;
                break;

            case ProjectionKind::Unit:
                // The word a packed field lives in, which is the same storage under a width rather
                // than a step into anything. `index` is how wide that word is, in bits.
                break;
        }

        if(!step(current)) return;
        current.at++;
    }
}

/*
 * What a place holds, which is the whole of what most callers want from the walk above.
 *
 * `module.scalar.error` rather than null for a path that did not resolve, because the resolver's own
 * callers compare against it and a diagnostic has already been reported by whoever built the place.
 */
inline TypePtr placeType(Module& module, Function& function, const Place& place,
                         Size limit = maxLimit<Size>) {
    auto type = placeRootType(module, function, place);

    walkPlace(module, function, place, [&](const PlaceStep& step) {
        type = step.broken ? module.scalar.error : step.type;
        return !step.broken;
    }, limit);

    return type;
}

/*
 * The type a place's last projection is taken of, or null where the place has no projections - which
 * is a whole local, and has no owner to speak of.
 */
inline TypePtr placeOwnerType(Module& module, Function& function, const Place& place) {
    TypePtr owner = nullptr;

    walkPlace(module, function, place, [&](const PlaceStep& step) {
        owner = step.broken ? nullptr : step.owner;
        return !step.broken;
    });

    return owner;
}
