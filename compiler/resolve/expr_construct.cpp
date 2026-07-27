#include "expr.h"
#include "name.h"

/*
 * Storage, places and aggregates.
 *
 * A place is a local plus a path into it (Implementation-IR.md part 2). Milestone 2 needs three
 * of the projection kinds - Discriminant, Downcast and Field - and produces places in exactly
 * two situations: constructing an aggregate, and reading a field out of one. Everything else in
 * the resolver still works on SSA values, which is what keeps scalars out of memory entirely.
 */

ModulePtr<Value> ExprResolver::allocate(TypePtr type, LocationId source, StringId valueName) {
    auto allocation = emit<InstAlloc>(source, valueName, type, maxLimit<U32>);
    auto result = ref(allocation);

    allocation->local = function.addLocal(module, type, valueName, result);
    return result;
}

// The place a value is stored in. A value loaded out of a place is addressed through that same
// place again rather than through a copy, so that a field of a field resolves to one projection
// path rather than to a chain of temporaries.
Place ExprResolver::placeFor(ModulePtr<Value> value, LocationId source) {
    if(value && local[value]->kind == Value::LoadPlace) {
        return ((InstLoadPlace*)local[value])->place;
    }

    for(U32 i = 0; i < function.localCount(); i++) {
        if(function.localAt(local, i).value == value) return Place::inLocal(i);
    }

    context.diagnostics.error("aggregate value does not have an addressable place"_v, source);
    return Place::inLocal(maxLimit<U32>);
}

// The place a value occupies, creating one if it has none. A scalar normally has none - it is in
// a register, and that is the point of it - so taking its address is what makes storage exist,
// which is why this is the one operation `addressOf` needs and ordinary code never does.
Place ExprResolver::materialize(ModulePtr<Value> value, LocationId source) {
    if(value && local[value]->kind == Value::LoadPlace) {
        return ((InstLoadPlace*)local[value])->place;
    }

    for(U32 i = 0; i < function.localCount(); i++) {
        if(function.localAt(local, i).value == value) return Place::inLocal(i);
    }

    // Deliberately unnamed: the value already carries the name this was written under, and giving
    // the storage the same one would print two different things as `%x`.
    auto storage = allocate(valueType(value), source);
    auto place = placeFor(storage, source);

    initialize(place, value, source);
    return place;
}

Place ExprResolver::project(Place place, ProjectionKind kind, U16 index, ModulePtr<Value> value) {
    Place result = place;
    result.projections = {};

    for(auto projection: place.projections.contents(local)) {
        result.projections.push(module.arena, projection);
    }

    result.projections.push(module.arena, Projection { kind, index, value });
    return result;
}

// The type of the storage a place's root names, before any projection.
TypePtr ExprResolver::placeRootType(const Place& place) {
    switch(place.root) {
        case PlaceRoot::Local:
            if(place.local >= function.localCount()) return module.scalar.error;
            return function.localAt(local, place.local).type;
        case PlaceRoot::Global:
            return place.global ? local[place.global]->type : module.scalar.error;
        case PlaceRoot::Pointer: {
            // The pointee is the storage: `%Node` roots a place holding a Node.
            auto pointee = pointeeType(global, valueType(place.pointer));
            return pointee ? pointee : module.scalar.error;
        }
    }

    return module.scalar.error;
}

TypePtr ExprResolver::placeType(const Place& place) {
    auto type = placeRootType(place);
    auto projections = place.projections;

    for(auto projection: projections.contents(local)) {
        switch(projection.kind) {
            case ProjectionKind::Discriminant:
                type = module.scalar.int_;
                break;
            case ProjectionKind::Deref: {
                auto pointee = pointeeType(global, type);
                if(!pointee) return module.scalar.error;

                type = pointee;
                break;
            }
            case ProjectionKind::Downcast: {
                if(global[type]->kind != Type::Record) return module.scalar.error;

                auto record = (RecordType*)global[type];
                if(projection.index >= record->constructors.size()) return module.scalar.error;

                type = record->constructors.get(global, projection.index).content;
                break;
            }
            case ProjectionKind::Field: {
                if(global[type]->kind != Type::Tup) return module.scalar.error;

                auto tuple = (TupType*)global[type];
                if(projection.index >= tuple->fields.size()) return module.scalar.error;

                type = tuple->fields.get(global, projection.index).type;
                break;
            }
            default:
                return module.scalar.error;
        }
    }

    return type;
}

ModulePtr<Value> ExprResolver::load(Place place, LocationId source, StringId valueName) {
    auto type = placeType(place);
    if(isUnit(global, type)) return nullptr;

    return ref(emit<InstLoadPlace>(source, valueName, type, place));
}

void ExprResolver::initialize(Place place, ModulePtr<Value> value, LocationId source) {
    if(isUnit(global, placeType(place))) return;

    if(!value) {
        context.diagnostics.error("cannot initialize aggregate field without a value"_v, source);
        return;
    }

    emit<InstInit>(source, 0, module.scalar.unit, place, value);
}

// The address of a place, as a pointer to whatever it holds. Taking one is what forces a value
// that could have stayed in a register into storage - see InstAddress.
ModulePtr<Value> ExprResolver::addressOf(Place place, LocationId source, StringId valueName) {
    auto type = resolvePointerType(module, placeType(place));
    return ref(emit<InstAddress>(source, valueName, type, place));
}

// Writes one value into each field of `tuple`, matching the arguments to fields by name where
// they have one and by position otherwise.
bool ExprResolver::fillTuple(Place place, TupType& tuple, ast::ParseList<ast::TupArg> astArgs, LocationId source) {
    auto args = astArgs.contents(parse);

    Array<ModulePtr<Value>> values;
    values.reserve(tuple.fields.size());
    for(Size i = 0; i < tuple.fields.size(); i++) values.push(nullptr);

    Size positional = 0;
    auto success = true;

    for(auto arg: args) {
        Size index = maxLimit<Size>;

        if(arg.name) {
            for(Size i = 0; i < tuple.fields.size(); i++) {
                if(tuple.fields.get(global, i).name == arg.name) {
                    index = i;
                    break;
                }
            }
        } else {
            while(positional < values.size() && values[positional]) positional++;
            if(positional < values.size()) index = positional++;
        }

        if(index == maxLimit<Size>) {
            context.diagnostics.error(arg.name ? "constructed tuple has no field with this name"_v : "too many tuple arguments"_v, arg.value.source);
            success = false;
            continue;
        }

        if(values[index]) {
            context.diagnostics.error("tuple field specified more than once"_v, arg.value.source);
            success = false;
            continue;
        }

        auto expected = tuple.fields.get(global, index).type;
        values[index] = resolve(arg.value, expected);

        if(values[index] && !isMemoryType(global, expected)) {
            values[index] = convert(values[index], expected, arg.value.source);
        }
    }

    if(args.size() != tuple.fields.size()) {
        context.diagnostics.error("incorrect number of tuple fields"_v, source);
        success = false;
    }

    for(Size i = 0; i < values.size(); i++) {
        if(!values[i]) {
            context.diagnostics.error("no value provided for tuple field"_v, source);
            success = false;
            continue;
        }

        initialize(project(place, ProjectionKind::Field, U16(i)), values[i], source);
    }

    return success;
}

ModulePtr<Value> ExprResolver::resolveTuple(const ast::Expr& expr, ast::ParseList<ast::TupArg> astArgs, TypePtr target) {
    if(astArgs.isEmpty()) return nullptr;

    TupType* tuple = nullptr;
    Array<ModulePtr<Value>> inferredValues;

    // With no expected type the tuple's own type is whatever its arguments turn out to be, so
    // they are resolved first and the type interned from their results.
    if(target && global[target]->kind == Type::Tup) {
        tuple = (TupType*)global[target];
    } else {
        Array<Field> fields;

        for(auto arg: astArgs.contents(parse)) {
            // The tuple's type is about to be interned from these, so a literal that nothing
            // decided settles here rather than becoming a field type nothing can lay out.
            auto value = settle(resolve(arg.value), arg.value.source);
            inferredValues.push(value);
            fields.push(Field { valueType(value), arg.name, 0 });
        }

        tuple = resolveTupleType(module, toBuffer(fields), expr.source);
    }

    auto result = allocate((Type*)tuple - global, expr.source);
    auto place = placeFor(result, expr.source);

    if(inferredValues.isNotEmpty()) {
        for(Size i = 0; i < inferredValues.size(); i++) {
            initialize(project(place, ProjectionKind::Field, U16(i)), inferredValues[i], expr.source);
        }
    } else {
        fillTuple(place, *tuple, astArgs, expr.source);
    }

    return result;
}

/*
 * Which type a constructor of a generic record produces here.
 *
 * Nothing about `Just` itself says what its element type is, so it comes from one of two places:
 * the expected type, or the argument. The expected type is tried first because it is the only
 * one that can also settle a constructor carrying no argument - `Nothing` on its own is not
 * inferable and has to be told.
 *
 * Falling back to the argument means resolving it before the storage it will initialize exists.
 * That is why the values are handed back in `resolved`: re-resolving them after the allocation
 * would emit them twice.
 */
TypePtr ExprResolver::constructedType(ConstructorRef reference, ast::ParseList<ast::TupArg> args, TypePtr target,
                                      Array<ModulePtr<Value>>& resolved, LocationId source) {
    auto declaration = global[reference.record];
    auto env = declaration->gen ? global[declaration->gen] : nullptr;
    if(!env || env->types.isEmpty()) return (Type*)declaration - global;

    if(target && global[target]->kind == Type::Record &&
       ((RecordType*)global[target])->base(global) == reference.record) {
        return target;
    }

    Array<TypePtr> bindings;
    for(Size i = 0; i < env->types.size(); i++) bindings.push(nullptr);

    auto content = declaration->constructors.get(global, reference.index).content;
    auto contents = args.contents(parse);

    if(content && !isUnit(global, content) && contents.size()) {
        // A tuple content is matched field by field, so `Pair(1, 2.5)` can bind two variables
        // from two arguments; anything else takes its one argument whole.
        auto tuple = global[content]->kind == Type::Tup ? (TupType*)global[content] : nullptr;
        auto perField = tuple && (contents.size() > 1 || tuple->fields.size() == contents.size());

        for(Size i = 0; i < contents.size(); i++) {
            // The record's type arguments are inferred from these, so - as for a tuple - a
            // literal has to have settled on a type before it can be one of them.
            auto value = settle(resolve(contents[i].value), contents[i].value.source);
            resolved.push(value);
            if(!value) continue;

            auto pattern = content;
            if(perField) {
                if(i >= tuple->fields.size()) continue;
                pattern = tuple->fields.get(global, i).type;
            } else if(i) {
                continue;
            }

            matchType(global, pattern, valueType(value), { bindings.pointer(), bindings.size() });
        }
    }

    for(auto binding: bindings) {
        if(binding) continue;

        context.diagnostics.error("cannot infer the type arguments of %@ here - give the expected type"_v, source,
                                  context.findName(declaration->name));
        return module.scalar.error;
    }

    return instantiateRecord(module, reference.record, toBuffer(bindings), source);
}

ModulePtr<Value> ExprResolver::resolveConstruct(const ast::Expr& expr, const ast::ConExpr& construct, TypePtr target) {
    if(construct.type.kind != ast::Type::Con) {
        context.diagnostics.error("constructor must have a named type"_v, expr.source);
        return nullptr;
    }

    auto found = findConstructor(module, construct.type.name, expr.source);
    if(!found) {
        context.diagnostics.error("unknown constructor %@"_v, expr.source, context.findName(construct.type.name));
        return nullptr;
    }

    auto reference = found.unwrap();
    Array<ModulePtr<Value>> inferredValues;
    auto recordType = constructedType(reference, construct.args, target, inferredValues, expr.source);
    if(global[recordType]->kind != Type::Record) return nullptr;

    auto record = (RecordType*)global[recordType];

    if(target && !sameType(target, recordType)) {
        context.diagnostics.error("constructor produces %@ but %@ is expected"_v, expr.source,
                                  describeType(context, global, recordType), describeType(context, global, target));
    }

    auto constructor = record->constructors.get(global, reference.index);
    auto constructArgs = construct.args;
    auto args = constructArgs.contents(parse);

    // A record whose constructors all carry nothing is just its discriminant, so constructing
    // one produces the index as a value rather than storage holding it.
    if(record->layout == RecordType::Enum) {
        if(args.size()) context.diagnostics.error("nullary constructor does not take arguments"_v, expr.source);
        return makeInt(expr.source, recordType, reference.index);
    }

    auto result = allocate(recordType, expr.source);
    auto root = placeFor(result, expr.source);

    if(record->layout == RecordType::Multi) {
        auto discriminant = makeInt(expr.source, module.scalar.int_, reference.index);
        initialize(project(root, ProjectionKind::Discriminant, 0), discriminant, expr.source);
    }

    auto content = constructor.content;
    auto contentPlace = project(root, ProjectionKind::Downcast, reference.index);

    if(!content || isUnit(global, content)) {
        if(args.size()) context.diagnostics.error("nullary constructor does not take arguments"_v, expr.source);
    } else if(inferredValues.isNotEmpty()) {
        // The arguments were already resolved to infer the type; only the writes are left.
        if(global[content]->kind == Type::Tup && inferredValues.size() > 1) {
            auto tuple = (TupType*)global[content];

            for(Size i = 0; i < inferredValues.size() && i < tuple->fields.size(); i++) {
                auto expected = tuple->fields.get(global, i).type;
                auto value = isMemoryType(global, expected) ? inferredValues[i]
                                                            : convert(inferredValues[i], expected, expr.source);

                initialize(project(contentPlace, ProjectionKind::Field, U16(i)), value, expr.source);
            }
        } else {
            auto value = isMemoryType(global, content) ? inferredValues[0]
                                                       : convert(inferredValues[0], content, expr.source);

            initialize(contentPlace, value, expr.source);
        }
    } else if(global[content]->kind == Type::Tup) {
        fillTuple(contentPlace, *(TupType*)global[content], construct.args, expr.source);
    } else if(args.size() != 1 || args[0].name) {
        context.diagnostics.error("constructor requires one positional argument"_v, expr.source);
    } else {
        auto value = resolve(args[0].value, content);
        if(value && !isMemoryType(global, content)) value = convert(value, content, args[0].value.source);

        initialize(contentPlace, value, expr.source);
    }

    return result;
}

Maybe<Place> ExprResolver::projectField(Place place, const ast::Expr& field, LocationId source) {
    if(field.kind != ast::Expr::Var) {
        context.diagnostics.error("field selection requires a field name"_v, field.source);
        return Nothing();
    }

    auto type = placeType(place);

    // A single-constructor record has no discriminant to test, so selecting a field out of one
    // is a downcast to its only constructor followed by an ordinary field projection.
    if(global[type]->kind == Type::Record) {
        auto record = (RecordType*)global[type];
        if(record->layout != RecordType::Single) {
            context.diagnostics.error("direct field selection requires a single-constructor record"_v, source);
            return Nothing();
        }

        place = project(place, ProjectionKind::Downcast, 0);
        type = record->constructors.get(global, 0).content;
    }

    if(!type || global[type]->kind != Type::Tup) {
        context.diagnostics.error("value does not contain named fields"_v, source);
        return Nothing();
    }

    auto tuple = (TupType*)global[type];
    for(Size i = 0; i < tuple->fields.size(); i++) {
        if(tuple->fields.get(global, i).name == field.var) {
            return Just(project(place, ProjectionKind::Field, U16(i)));
        }
    }

    context.diagnostics.error("unknown field %@"_v, field.source, context.findName(field.var));
    return Nothing();
}

ModulePtr<Value> ExprResolver::resolveField(const ast::Expr& expr, const ast::FieldExpr& field) {
    // A field of raw memory is reached through the place the pointer names, so `(*node).value`
    // needs no dereference of its own beyond the one the place already is.
    auto& target = unwrapNested(field.target);

    if(target.kind == ast::Expr::Prefix) {
        auto& prefix = *parse[target.prefix];

        if(prefix.op.kind == ast::Expr::Var && prefix.op.var == Context::nameHash("*"_v)) {
            auto pointer = resolve(prefix.on);
            if(!pointer) return nullptr;

            if(isPointer(global, valueType(pointer))) {
                auto place = projectField(Place::atPointer(pointer), field.field, expr.source);
                return place ? load(place.unwrap(), expr.source) : nullptr;
            }
        }
    }

    auto value = resolve(field.target);
    if(!value) return nullptr;

    auto place = projectField(placeFor(value, field.target.source), field.field, expr.source);
    return place ? load(place.unwrap(), expr.source) : nullptr;
}
