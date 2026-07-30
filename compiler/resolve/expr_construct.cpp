#include "expr.h"
#include "generic.h"
#include "name.h"
#include "witness.h"

/*
 * Storage, places and aggregates.
 *
 * A place is a local plus a path into it (Implementation-IR.md part 2). Milestone 2 needs three
 * of the projection kinds - Discriminant, Downcast and Field - and produces places in exactly
 * two situations: constructing an aggregate, and reading a field out of one. Everything else in
 * the resolver still works on SSA values, which is what keeps scalars out of memory entirely.
 */

ModulePtr<Value> ExprResolver::allocate(TypePtr type, LocationId source, StringId valueName,
                                        ast::BindType convention, bool closureEnv) {
    // Storage of a type this body cannot see needs that type's size, which is the first thing a
    // TypeDesc carries. Recording the requirement here rather than at each construction site is
    // what makes it exhaustive: every generic aggregate that occupies storage passes through.
    if(auto env = functionGen(global, function)) requireTypeSlot(module, *env, type);

    auto allocation = emit<InstAlloc>(source, valueName, type, maxLimit<U32>);
    auto result = ref(allocation);

    allocation->local = function.addLocal(module, type, valueName, result, convention, false, closureEnv);
    return result;
}

// The place a value came out of, or nothing where it came out of no storage at all. A value loaded
// out of a place is addressed through that same place again rather than through a copy, so that a
// field of a field resolves to one projection path rather than to a chain of temporaries.
Maybe<Place> ExprResolver::findPlace(ModulePtr<Value> value) {
    if(!value) return Nothing();

    if(local[value]->kind == Value::LoadPlace) {
        return Just(((InstLoadPlace*)local[value])->place);
    }

    for(U32 i = 0; i < function.localCount(); i++) {
        if(function.localAt(local, i).value == value) return Just(Place::inLocal(i));
    }

    return Nothing();
}

Place ExprResolver::placeFor(ModulePtr<Value> value, LocationId source) {
    if(auto found = findPlace(value)) return found.unwrap();

    context.diagnostics.error("aggregate value does not have an addressable place"_v, source);
    return Place::inLocal(maxLimit<U32>);
}

// Whether this place may be written through. A local says so with the convention of the binding
// that owns it, a global with `let &`, and the memory a raw pointer names is always writable -
// Design.md's Pointers section, which is also why an immutable binding holding one can still root
// a write. A borrow of a place is exactly a write capability handed to someone else, so a mutable
// borrow asks this same question.
bool ExprResolver::isWritablePlace(const Place& place) {
    switch(place.root) {
        case PlaceRoot::Local:
            return place.local < function.localCount() &&
                   function.localAt(local, place.local).convention == ast::BindType::Ref;
        case PlaceRoot::Global:
            return place.global && local[place.global]->mut;
        case PlaceRoot::Pointer:
            return true;
        case PlaceRoot::Borrow:
            // A borrow carries its own answer: `&T` was handed out as a write capability and `&`
            // with no mutability was not. That is the whole of what the two types differ in.
            return isBorrow(global, valueType(place.pointer)) &&
                   ((BorrowType*)global[valueType(place.pointer)])->mut;
    }

    return false;
}

// The place a value occupies, creating one if it has none. A scalar normally has none - it is in
// a register, and that is the point of it - so taking its address is what makes storage exist,
// which is why this is the one operation `addressOf` needs and ordinary code never does.
Place ExprResolver::materialize(ModulePtr<Value> value, LocationId source) {
    if(auto place = findPlace(value)) return place.unwrap();

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
TypePtr placeRootType(Module& module, Function& function, const Place& place) {
    auto global = *module.types;
    auto local = *module.arena;
    auto valueType = [&](ModulePtr<Value> value) {
        return value ? local[value]->type : module.scalar.unit;
    };

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
        case PlaceRoot::Borrow: {
            auto type = valueType(place.pointer);
            return isBorrow(global, type) ? ((BorrowType*)global[type])->to : module.scalar.error;
        }
    }

    return module.scalar.error;
}

TypePtr placeType(Module& module, Function& function, const Place& place) {
    auto global = *module.types;
    auto local = *module.arena;
    auto type = placeRootType(module, function, place);
    auto projections = place.projections;

    for(auto projection: projections.contents(local)) {
        switch(projection.kind) {
            case ProjectionKind::Discriminant:
                type = module.scalar.int_;
                break;
            case ProjectionKind::Property: {
                // What the constraint promised the field holds. The owner is not consulted: the
                // whole point of the slot is that the owner is not known here.
                auto env = functionGen(global, function);
                if(!env) return module.scalar.error;

                auto& schema = genSchemaOf(module, *env);
                TypePtr result = nullptr;

                for(auto slot: schema.slots.contents(global)) {
                    if(slot.kind == GenSlotKind::Property && slot.index == projection.index) {
                        result = slot.result;
                    }
                }

                if(!result) return module.scalar.error;
                type = result;
                break;
            }
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
                // A function value is two addresses, and they are projected into rather than
                // being a representation only lowering knows about - which is what lets the same
                // Init, LoadPlace and Drop machinery build one, read one and tear one down. The
                // third index is the closure header, which is a projection some targets answer and
                // no source can write - see FunValueLayout::kHeader.
                if(global[type]->kind == Type::Fun) {
                    if(projection.index >= FunValueLayout::kProjectionCount) return module.scalar.error;

                    type = funValueFieldType(module, projection.index);
                    break;
                }

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

TypePtr ExprResolver::placeRootType(const Place& place) {
    return ::placeRootType(module, function, place);
}

TypePtr ExprResolver::placeType(const Place& place) {
    return ::placeType(module, function, place);
}

ModulePtr<Value> ExprResolver::load(Place place, LocationId source, StringId valueName) {
    auto type = placeType(place);
    if(isUnit(global, type)) return nullptr;

    return ref(emit<InstLoadPlace>(source, valueName, type, place));
}

void ExprResolver::initialize(Place place, ModulePtr<Value> value, LocationId source) {
    write(place, value, source, Value::Init);
}

// Overwriting storage that already held a live value. The difference from initialize() is entirely
// about what the old value was owed - see InstInit - so the two share everything else.
void ExprResolver::assign(Place place, ModulePtr<Value> value, LocationId source) {
    write(place, value, source, Value::Assign);
}

void ExprResolver::write(Place place, ModulePtr<Value> value, LocationId source, Value::Kind kind) {
    if(isUnit(global, placeType(place))) return;

    if(!value) {
        context.diagnostics.error("cannot initialize aggregate field without a value"_v, source);
        return;
    }

    emit<InstInit>(source, 0, module.scalar.unit, place, value, kind);
}

/*
 * The argument of a `&` parameter.
 *
 * There is no sigil at the call site: the convention is part of the callee's signature, so `f(x)`
 * mutably borrows exactly when `f` declared `&x`. That is what lets `x += 41` mean anything at
 * all, since an infix operator has nowhere to put a sigil and `+=` is an ordinary function whose
 * first parameter is `&`.
 *
 * What is checked here is what a mutable borrow needs before liveness has anything to say: the
 * argument has to name storage, and that storage has to be writable. Note that no conversion is
 * attempted - converting would borrow a temporary and write back into nothing, so a mismatched
 * type is an ordinary argument error rather than something to paper over.
 */
ModulePtr<Value> ExprResolver::borrowArgument(ModulePtr<Value> value, TypePtr expected, LocationId source) {
    if(!value) return nullptr;

    if(!sameType(valueType(value), expected)) {
        context.diagnostics.error("a `&` argument must have exactly type %@, but this is %@ - a conversion would borrow a temporary"_v,
                                  source, describeType(context, global, expected),
                                  describeType(context, global, valueType(value)));
        return nullptr;
    }

    auto place = findPlace(value);
    if(!place) {
        context.diagnostics.error("a `&` argument must name storage that can be written back to"_v, source);
        return nullptr;
    }

    if(!isWritablePlace(place.unwrap())) {
        context.diagnostics.error("a `&` argument must name mutable storage - declare it with `let &`"_v, source);
        return nullptr;
    }

    return ref(emit<InstBorrow>(source, 0, resolveBorrowType(module, expected, true), place.unwrap(), true));
}

/*
 * What a `->` binding or a `->` argument produces.
 *
 * Design.md gives sink two behaviours rather than one, and which applies is decided here by the
 * source's type:
 *
 *  - not TrivialCopy: ownership is taken out of the source's storage, which becomes inaccessible.
 *    That is InstMove, and it is the whole reason `->` exists. Whether relocating the value is its
 *    bytes or a call is decided here too, and recorded on the instruction: a type that is not
 *    TrivialSink has a `Sink` of its own or a member with one, and either way the relocation is
 *    that function rather than a memcpy.
 *  - TrivialCopy: an independent copy, leaving the source valid - "`let ->z = x` (with `x: Int`)
 *    leaves `x` valid too, since `->` is the other TrivialCopy-affected convention alongside
 *    default". For a scalar that copy is free: the value is already in a register and the storage
 *    it was read out of was never touched. For an aggregate it is a real duplicate, because
 *    binding the same address twice would give two names one storage - which is exactly what
 *    TrivialCopy promises does not happen.
 *
 * A source that is in no storage at all - a call result, an arithmetic expression, a construction -
 * is already a temporary nothing else can reach, so there is nothing to take it out of and it
 * passes through unchanged.
 */
ModulePtr<Value> ExprResolver::sinkValue(ModulePtr<Value> value, LocationId source) {
    if(!value) return nullptr;

    auto type = valueType(value);
    auto place = findPlace(value);
    if(!place) return value;

    // Asked of the *context* rather than of the type, because a type variable's answer belongs to
    // the signature that introduced it: an unconstrained `a` is non-TrivialCopy inside this body
    // however a caller later substitutes it, and a declared `TrivialCopy(a)` is what makes the copy
    // legal. See ownershipIn.
    auto ownership = ownershipIn(module, functionGen(global, function), type);
    auto name = local[value]->name;

    if(!ownership.trivialCopy) {
        auto moved = create<InstMove>(source, name, type, place.unwrap());

        // Asked of the type rather than of the context, unlike TrivialCopy above: relocating an
        // unknown `a` is the erased path's business - the descriptor its caller passed carries the
        // moveInit - and nothing concrete to call exists here to name. ownershipIn never reports a
        // generic type as non-TrivialSink for a reason it could act on, so this asks what it can.
        if(!ownership.trivialSink) moved->sink = sinkFor(module, type, source);

        append(moved);
        return ref(moved);
    }

    if(!isMemoryType(global, type)) return value;

    auto duplicate = create<InstCopy>(source, name, type, place.unwrap());
    append(duplicate);

    auto result = ref(duplicate);
    duplicate->local = function.addLocal(module, type, name, result);
    return result;
}

/*
 * Storage for a `->` binding of an aggregate.
 *
 * Every other consumer of a move already has a destination to offer - a captured field, a
 * constructed member and a returned value are all storage somebody else provided, and lowering
 * relocates straight into it. A `let ->g = h` is the one that does not, so it provides its own here.
 * The InstInit in between is what lowering turns into the relocation, which is a call for a type
 * whose `Sink` says so and a block copy for one whose bytes are the whole story; nothing is copied
 * twice either way.
 *
 * A bitwise move needs the destination just as much, even though its bytes could have stayed where
 * they were: what a move produces is a value, and a value has no address, so the name that follows
 * one would have nothing to read a field out of. Letting it name the *source's* storage instead is
 * the thing that cannot work - the source has been moved out of, and every later use of that slot
 * is rejected as one, so `let ->g = h` would bind a name nothing may be read through.
 *
 * A scalar is not this. It came out of storage as a value in a register, which is where the binding
 * wants it, and giving it a slot would be putting it back for nobody.
 *
 * A `->` *argument* deliberately does not pass through this either. The callee takes over the
 * storage the argument already sits in, so there is no relocation to make - which is also the best
 * answer a self-referential type can get, since bytes that never move never break.
 */
ModulePtr<Value> ExprResolver::rootSink(ModulePtr<Value> value, LocationId source) {
    if(!value || local[value]->kind != Value::Move) return value;
    if(!isMemoryType(global, valueType(value))) return value;

    // Deliberately unnamed. The source's name is already on the move, and giving the destination
    // the same one would print two allocations and a call between them under one name.
    auto storage = allocate(valueType(value), source);
    if(auto place = findPlace(storage)) initialize(place.unwrap(), value, source);

    return storage;
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
            fields.push(Field { valueType(value), arg.name });
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
 * `{->v | ...}` is the same expression consuming its source instead of copying it. It is the same
 * shape with the relocation sinkValue() picks in place of the copy: ownership is taken out of `v`,
 * which is dead afterwards, and lands in the result's storage. What that buys is a type whose
 * relocation is not a copy at all - a `Sink` runs once instead of a `Copy`, and a type that has no
 * `Copy` to run can be updated at all, which is the whole of it. `v` being TrivialCopy makes `->`
 * the copy again, by Design.md's copy-on-read rule, and a source that is in no storage - a call
 * result, another update - has nothing to be taken out of and passes through as it does anywhere
 * else.
 *
 * What it does not yet buy is writing the new fields into `v`'s own storage rather than into a
 * fresh allocation. That is a storage-class decision rather than a checking one, and the relocation
 * the honest version emits is the thing that makes it worth making later.
 *
 * The replacements are resolved before the source is moved, so that they may still read it:
 * `{->c | n: c.n + 1}` is what a move update is *for*, and it is the source's own field it reads.
 * Reading before the move is also the order the expression is written in - the source's own effects
 * happen first, then each replacement's, and the relocation is the last thing before the writes.
 */
ModulePtr<Value> ExprResolver::resolveTupUpdate(const ast::Expr& expr, const ast::TupUpdateExpr& update,
                                                TypePtr target) {
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

    auto sink = update.bind == ast::BindType::Sink;
    auto result = allocate(type, expr.source);
    auto root = placeFor(result, expr.source);

    // A copy is made before the replacements are resolved, and a relocation after them. Both are
    // as early as they can be: the copy leaves the source readable, so nothing is gained by
    // delaying it, and the move does not, so it waits for everything that still reads the source.
    if(!sink) initialize(root, value, expr.source);

    // Where a replacement of a moved-from source goes, held until the relocation has happened.
    // Writing one before that would put it in storage the relocation is about to overwrite.
    struct Replacement {
        Place place;
        ModulePtr<Value> value;
        LocationId source;
    };

    Array<Replacement> replacements;

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

        if(sink) replacements.push(Replacement { place, replacement, arg.value.source });
        else initialize(place, replacement, arg.value.source);
    }

    if(sink) {
        // Asked of the context rather than of the type for the reason sinkValue is - see there.
        // A TrivialCopy source's `->` is the copy the other branch already performs, and asking
        // sinkValue for it as well would duplicate it into a local nothing else reads.
        auto ownership = ownershipIn(module, functionGen(global, function), type);
        if(!ownership.trivialCopy) value = sinkValue(value, update.value.source);

        initialize(root, value, expr.source);

        // `assign` rather than `initialize`, because the relocation just put a live value in every
        // one of these places - which is the whole difference between the two. What it does not yet
        // buy is the drop of the value replaced: a write to a *field* is tracked as a use of the
        // slot rather than as a definition of part of it, so the drop it owes is the one an
        // ordinary `v.f = x` owes and does not run either. Both are the same missing thing - the
        // per-field ownership state that "cannot move a part of a value out of it" is also about -
        // and marking the write for what it is, is what makes this one land when that arrives.
        for(auto& replacement: replacements) {
            assign(replacement.place, replacement.value, replacement.source);
        }
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

    /*
     * A field of a type this body cannot see.
     *
     * Not an error and not a guess: the access becomes a *requirement* that the context has such a
     * field, and the projection names the slot that records it. Where the field actually is depends
     * on the owner's Repr, which is not decided until the owner is - so this is the one projection
     * that is resolved later, when specialization turns `a` into a type with a layout.
     */
    if(global[type]->kind == Type::Gen) {
        auto slot = requireProperty(module, function, type, field, source);
        if(slot == maxLimit<U16>) return Nothing();

        return Just(project(place, ProjectionKind::Property, slot));
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

/*
 * Array literals and subscripts.
 *
 * `[1, 2, 3]` is not a primitive: it builds a Collections `Array(T)`, whose elements live in a
 * buffer this expression allocates. The buffer is an ordinary allocation and therefore an ordinary
 * subject of storage-class selection - it stays on the frame when the array provably does not
 * outlive it, and goes to the heap when it does, with `onHeap` recording which so that the array's
 * `Drop` knows whether it has anything to free.
 *
 * The buffer's type is an anonymous tuple of n fields rather than a fixed-size array type, because
 * that is a type the compiler already has: it has a Repr, its fields have offsets, and a field
 * projection into it is the projection the rest of the resolver already builds. What it costs is
 * that a literal with a thousand elements is a tuple with a thousand fields, which is a real limit
 * and the reason `[T *n]` will want to be a type of its own eventually.
 */

// The element type of an `Array(T)`, or null for anything else.
static TypePtr arrayElement(Module& module, GlobalBase global, TypePtr type) {
    if(!type || global[type]->kind != Type::Record) return nullptr;

    auto record = (RecordType*)global[type];
    if(record->instanceOf != module.program.arrayType || record->instanceArgs.size() != 1) return nullptr;

    return record->instanceArgs.get(global, 0);
}

ModulePtr<Value> ExprResolver::resolveArray(const ast::Expr& expr, ast::ParseList<ast::Expr> items,
                                            TypePtr target) {
    auto source = expr.source;

    if(!module.program.arrayType) {
        context.diagnostics.error("arrays are not available in this module"_v, source);
        return nullptr;
    }

    // The expected type decides the element type where there is one, so that `[] :: [Int]` and a
    // literal in an argument position both work; otherwise the first element decides and the rest
    // are converted to it.
    auto element = arrayElement(module, global, target);

    Array<ModulePtr<Value>> values;
    for(auto item: items.contents(parse)) {
        auto value = resolve(item, element);
        if(!element && value) element = settleType(valueType(value));
        values.push(value);
    }

    if(!element) {
        context.diagnostics.error("cannot tell what this empty array holds - give it an expected type"_v, source);
        return nullptr;
    }

    auto arrayType = instantiateRecord(module, module.program.arrayType, { &element, 1 }, source);
    if(global[arrayType]->kind != Type::Record) return nullptr;

    /*
     * An element whose lifetime ends in something is one this array would have to drop, and
     * dropping the elements means walking the buffer at run time - which needs the element's drop
     * to be reachable from a generic body, and that is Milestone 7's. Rejected rather than leaked.
     */
    if(needsTeardown(module, element)) {
        context.diagnostics.error("an array of %@ is not available yet - its elements have a teardown, and releasing them needs the erased generic model"_v,
                                  source, describeType(context, global, element));
        return nullptr;
    }

    auto record = (RecordType*)global[arrayType];
    if(record->constructors.isEmpty()) return nullptr;

    auto content = record->constructors.get(global, 0).content;
    if(!content || global[content]->kind != Type::Tup) return nullptr;

    auto fields = (TupType*)global[content];
    if(fields->fields.size() < 4) return nullptr;
    auto pointerField = fields->fields.get(global, 0).type;
    auto countField = fields->fields.get(global, 1).type;
    auto flagField = fields->fields.get(global, 3).type;

    // The buffer, and the elements written into it.
    ModulePtr<Value> buffer = nullptr;
    ModulePtr<Value> items_ = nullptr;

    if(values.isNotEmpty()) {
        Array<Field> layout;
        for(Size i = 0; i < values.size(); i++) layout.push(Field { element, 0 });

        auto bufferType = (Type*)resolveTupleType(module, toBuffer(layout), source) - global;
        buffer = allocate(bufferType, source);
        auto bufferPlace = placeFor(buffer, source);

        for(Size i = 0; i < values.size(); i++) {
            auto value = convert(values[i], element, source);
            if(value) initialize(project(bufferPlace, ProjectionKind::Field, U16(i)), value, source);
        }

        // The address of the first element, typed as a pointer to one rather than to the buffer:
        // what the array holds is where its elements start, and the buffer is only how they got
        // somewhere. Nothing about the address changes, which is why this is not a cast.
        items_ = ref(emit<InstAddress>(source, 0, pointerField, bufferPlace));
    } else {
        items_ = constantBits(pointerField, 0, source);
    }

    // The flag the storage decision is written into, once it has been made. It starts as "not the
    // heap" because that is what the analysis answers unless it proves otherwise, and because a
    // literal that never reaches selectStorage - inside a generic body, say - is then still right.
    auto onHeap = constant<ConstInt>(source, flagField, 0);
    if(buffer) ((InstAlloc*)local[buffer])->storageFlag = onHeap;

    auto storage = allocate(arrayType, source, 0);
    auto place = project(placeFor(storage, source), ProjectionKind::Downcast, 0);
    auto count = makeInt(source, countField, values.size());

    initialize(project(place, ProjectionKind::Field, 0), items_, source);
    initialize(project(place, ProjectionKind::Field, 1), count, source);
    initialize(project(place, ProjectionKind::Field, 2), count, source);
    initialize(project(place, ProjectionKind::Field, 3), onHeap, source);

    return storage;
}

/*
 * `xs[i]`.
 *
 * Sugar for the accessor, in both directions: a read is `get`, and an assignment target is
 * `getMut`, whose result is a mutable borrow the assignment writes through. Nothing about either is
 * special-cased in the checker - the return-root markers on those two signatures are what keep the
 * array borrowed for as long as the element is, exactly as they would for an accessor a program
 * wrote itself.
 */
ModulePtr<Value> ExprResolver::resolveSubscript(const ast::Expr& expr, const ast::AppExpr& subscript,
                                                bool mutable_) {
    auto source = expr.source;
    auto args = subscript.args;

    if(args.size() != 1) {
        context.diagnostics.error("an array subscript takes exactly one index"_v, source);
        return nullptr;
    }

    auto target = resolve(subscript.callee);
    if(!target) return nullptr;

    if(!arrayElement(module, global, valueType(target))) {
        context.diagnostics.error("cannot index %@ - only an array may be subscripted"_v, source,
                                  describeType(context, global, valueType(target)));
        return nullptr;
    }

    ModulePtr<Value> index = nullptr;
    for(auto arg: args.contents(parse)) index = resolve(arg.value);
    if(!index) return nullptr;

    ModulePtr<Value> values[] = { target, index };
    auto name = context.addUnqualifiedName(mutable_ ? "getMut" : "get", mutable_ ? 6 : 3);

    return emitCall(name, { values, 2 }, source);
}
