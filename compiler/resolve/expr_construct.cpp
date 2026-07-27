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

    // A value that occupies nothing has nowhere to be put, so there is no address to answer with
    // and the honest answer is to say so. Allocating for one would hand back a pointer to whatever
    // followed it in the frame, which is a dangling pointer the program has no way to notice.
    //
    // Everything else *can* be given storage, and the address it then has is the address of a
    // temporary - which is what taking the address of a value rather than of a variable means,
    // whether the value came from a call, an arithmetic expression, or an immutable global. This is
    // the whole of the check on purpose: a value reaches here with a settled, concrete type (see
    // emitGenericCall), so a size that cannot be allocated for is the one case left.
    auto type = valueType(value);
    if(isUnit(global, type)) {
        // Worded for addressOf, which is the only caller; a second one would want it generalized.
        context.diagnostics.error("cannot take the address of a value of type %@, which occupies no storage"_v,
                                  source, describeType(context, global, type));
        return Place::inLocal(maxLimit<U32>);
    }

    // Deliberately unnamed: the value already carries the name this was written under, and giving
    // the storage the same one would print two different things as `%x`.
    auto storage = allocate(type, source);
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
// they have one and by position otherwise. A field the arguments left out takes the default its
// declaration gave it, and is an error where there is none - `defaults` is empty for an anonymous
// tuple, which has no declaration to have written one in.
bool ExprResolver::fillTuple(Place place, TupType& tuple, ast::ParseList<ast::TupArg> astArgs,
                             GlobalList<FieldDefault>* defaults, LocationId source) {
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

    for(Size i = 0; i < values.size(); i++) {
        if(!values[i]) {
            auto field = tuple.fields.get(global, i);

            if(auto def = fieldDefault(defaults, U16(i))) {
                values[i] = constantBits(field.type, def.unwrap(), source);
            } else if(field.name) {
                context.diagnostics.error("no value provided for field %@"_v, source,
                                          context.findName(field.name));
                success = false;
                continue;
            } else {
                context.diagnostics.error("no value provided for tuple field"_v, source);
                success = false;
                continue;
            }
        }

        initialize(project(place, ProjectionKind::Field, U16(i)), values[i], source);
    }

    return success;
}

Maybe<U64> ExprResolver::fieldDefault(GlobalList<FieldDefault>* defaults, U16 field) {
    if(!defaults) return Nothing();

    for(auto def: defaults->contents(global)) {
        if(def.field == field) return Just(def.value);
    }

    return Nothing();
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
        fillTuple(place, *tuple, astArgs, nullptr, expr.source);
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
        // Defaults are read from the declaration rather than from `record`, which may be an
        // instantiation of it: what a field falls back to is a property of the declaration, and
        // an instantiation can be made before the declaration's defaults have been read.
        // `reference.record` is always the declaration - see findConstructor.
        auto declared = ((RecordType*)global[reference.record])->constructors.get(global, reference.index);
        fillTuple(contentPlace, *(TupType*)global[content], construct.args, &declared.defaults, expr.source);
    } else if(args.size() != 1 || args[0].name) {
        context.diagnostics.error("constructor requires one positional argument"_v, expr.source);
    } else {
        auto value = resolve(args[0].value, content);
        if(value && !isMemoryType(global, content)) value = convert(value, content, args[0].value.source);

        initialize(contentPlace, value, expr.source);
    }

    return result;
}

/*
 * Record update - `{value | field: x}`.
 *
 * A copy of `value` with some of its fields replaced, which is what makes an immutable record
 * usable at all: without it there is no way to change one field of one, and the only workaround is
 * to write every other field out again. The result is a new value of the source's own type, so the
 * update names the type rather than being told it - `{v | x: 1}` is a `Vec2` wherever a `Vec2` is
 * wanted and nowhere else.
 *
 * The whole of it is a copy followed by the writes: storage of the source's type, the source
 * initialized into it, and one field initialization per argument, into a place projected out of
 * the copy. A nested path is that projection with more steps and nothing else - the copy is what
 * every path writes into, so `{v | .a.b: 1, .a.c: 2}` sets two fields of one `v.a` and the source
 * expression is named once however many paths there are.
 *
 * `{->v | ...}` is the same expression consuming its source instead of copying it, which is the
 * only version that can be cheaper than a copy and the only one that needs to know whether anyone
 * else can still see `v`. That is the ownership resolver's question, so it reports until then.
 */
ModulePtr<Value> ExprResolver::resolveTupUpdate(const ast::Expr& expr, const ast::TupUpdateExpr& update,
                                                TypePtr target) {
    if(update.bind == ast::BindType::Sink) {
        context.diagnostics.error("a move update is deferred until the ownership resolver - write `{value | ...}` to copy"_v,
                                  expr.source);
        return nullptr;
    }

    auto value = resolve(update.value, target);
    if(!value) return nullptr;

    auto type = valueType(value);
    auto args = update.args;

    // Only a value that *has* named fields can have them replaced. A reference is excluded rather
    // than followed: an update through one would copy the reference and then write through it into
    // what it points at, which is an assignment wearing an expression's clothes.
    auto kind = global[type]->kind;
    auto single = kind == Type::Record && ((RecordType*)global[type])->layout == RecordType::Single;

    if(!single && kind != Type::Tup) {
        context.diagnostics.error("cannot update %@ - a record update requires a single-constructor record or a tuple"_v,
                                  update.value.source, describeType(context, global, type));
        return nullptr;
    }

    auto result = allocate(type, expr.source);
    auto root = placeFor(result, expr.source);
    initialize(root, value, expr.source);

    for(auto arg: args.contents(parse)) {
        auto path = arg.path;
        auto place = root;
        auto reached = true;

        for(auto field: path.contents(parse)) {
            auto next = projectField(place, field, arg.value.source, expr.source);
            if(!next) {
                reached = false;
                break;
            }

            place = next.unwrap();
        }

        if(!reached || path.isEmpty()) continue;

        auto expected = placeType(place);
        auto replacement = resolve(arg.value, expected);
        if(!replacement) continue;

        if(!isMemoryType(global, expected)) replacement = convert(replacement, expected, arg.value.source);
        initialize(place, replacement, arg.value.source);
    }

    return result;
}

/*
 * Reaching through a reference.
 *
 * `.` reads through a reference of any kind rather than through `%T` alone. Every rung of
 * Design.md's [reference-kind ladder](Design.md#reference-kinds) is an opaque primitive in the
 * type system: a raw pointer is an address, a region pointer an offset, and a checked reference a
 * fat pointer, and none of the three exposes what it is made of as a field. There is therefore
 * never a name the selection could have meant instead of the target's, which is what makes reading
 * through all of them one rule rather than a special case for the unchecked one.
 *
 * What differs between the rungs is the step each dereference needs, not what `.` means. A raw
 * pointer's is the address itself, so it is a place projection and nothing more. A region
 * pointer's needs the `Region` handle its `*` requires, and a checked reference's the generation
 * compare - neither of which has a representation the resolver can produce yet, so both report
 * here rather than quietly producing an unchecked read. When they land, this check is what turns
 * into their step, and field selection reaches through them with nothing else to change.
 */
bool ExprResolver::reportUnfollowedReference(TypePtr type, LocationId source) {
    auto kind = global[type]->kind;
    if(kind != Type::Ref && kind != Type::RegionPtr) return false;

    context.diagnostics.error(kind == Type::Ref
        ? "reading a field through a checked reference is not available yet - it needs the generation check a dereference performs"_v
        : "reading a field through a region pointer is not available yet - it needs the `Region` handle a dereference requires"_v,
        source);

    return true;
}

Maybe<Place> ExprResolver::projectField(Place place, const ast::Expr& field, LocationId source) {
    if(field.kind != ast::Expr::Var) {
        context.diagnostics.error("field selection requires a field name"_v, field.source);
        return Nothing();
    }

    return projectField(place, field.var, field.source, source);
}

Maybe<Place> ExprResolver::projectField(Place place, StringId field, LocationId fieldSource, LocationId source) {
    auto type = placeType(place);

    // Field selection reads through a reference, one step per `.`, so a chain reaches through as
    // many links as it has - see reportUnfollowedReference above for why this is one rule over
    // every reference kind. A raw pointer's step is the projection; the checked rungs report.
    if(isPointer(global, type)) {
        place = project(place, ProjectionKind::Deref, 0);
        type = placeType(place);
    } else if(reportUnfollowedReference(type, source)) {
        return Nothing();
    }

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
        if(tuple->fields.get(global, i).name == field) {
            return Just(project(place, ProjectionKind::Field, U16(i)));
        }
    }

    context.diagnostics.error("unknown field %@"_v, fieldSource, context.findName(field));
    return Nothing();
}

ModulePtr<Value> ExprResolver::resolveField(const ast::Expr& expr, const ast::FieldExpr& field) {
    auto value = resolve(field.target);
    if(!value) return nullptr;

    // A reference is the root of the place its field lives in, rather than something that has to
    // be in a place of its own first. That is what lets `n.value` work on a `%Node` that came from
    // an argument or a call: there is no storage holding the pointer, and none is needed.
    if(reportUnfollowedReference(valueType(value), field.target.source)) return nullptr;

    auto root = isPointer(global, valueType(value)) ? Place::atPointer(value)
                                                    : placeFor(value, field.target.source);

    auto place = projectField(root, field.field, expr.source);
    return place ? load(place.unwrap(), expr.source) : nullptr;
}
