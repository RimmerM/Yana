#include "type.h"
#include "module.h"
#include "name.h"

static U32 alignTo(U32 value, U32 alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static TypePtr errorType(Module& module, LocationId source, StringView message) {
    module.context.diagnostics.error(message, source);
    return module.scalar.error;
}

/*
 * Generic instantiation.
 *
 * A generic declaration is one type; each set of arguments it is applied to is another, interned
 * against the declaration so that `Maybe(Int)` written twice is one TypePtr. That identity is
 * what lets sameType() stay pointer equality, and it is what instance lookup matches against.
 *
 * An argument may itself be generic - `data Pair(a) = P(Maybe(a))` instantiates `Maybe` with a
 * type variable - so an instantiation is not necessarily concrete. Those exist to be substituted
 * later and never reach the IR, which is why they get no Repr.
 */

bool isGeneric(GlobalBase base, TypePtr type) {
    return type && base[type]->generic;
}

static bool anyGeneric(GlobalBase base, Buffer<TypePtr> types) {
    return types.contains([&](TypePtr type) { return isGeneric(base, type); });
}

// Fills an instantiation's constructors by substituting the declaration's contents. Deferred
// when the declaration's own contents are not resolved yet, since a record may be used before
// the constructor list of the record it names has been read.
static void completeInstance(Module& module, RecordType& instance) {
    auto global = *module.types;
    auto declaration = (RecordType*)global[instance.instanceOf];
    if(!declaration->definitionReady || instance.definitionReady) return;

    Array<TypePtr> args;
    for(auto arg: instance.instanceArgs.contents(global)) args.push(arg);

    for(Size i = 0; i < declaration->constructors.size(); i++) {
        auto constructor = declaration->constructors.get(global, i);
        constructor.content = constructor.content
            ? substituteType(module, constructor.content, toBuffer(args), kNullLocation)
            : nullptr;

        instance.constructors.set(global, i, constructor);
    }

    instance.layout = declaration->layout;
    instance.definitionReady = true;
    if(!instance.generic) finishRecordRepr(module, instance, kNullLocation);
}

void completePendingInstances(Module& module) {
    auto global = *module.types;

    // Completing one instance can create another (a constructor content that names a further
    // instantiation), so the list is walked by index rather than by iterator.
    for(Size i = 0; i < module.program.pendingInstances.size(); i++) {
        completeInstance(module, *(RecordType*)global[module.program.pendingInstances[i]]);
    }

    module.program.pendingInstances.clear();
}

TypePtr instantiateRecord(Module& module, GlobalPtr<RecordType> pointer, Buffer<TypePtr> args, LocationId source) {
    auto global = *module.types;
    auto declaration = (RecordType*)global[pointer];

    if(declaration->instanceOf) {
        // Instantiating an instantiation cannot happen through source syntax, but substitution
        // reaches here; the declaration is what its arguments apply to.
        declaration = (RecordType*)global[declaration->instanceOf];
        pointer = declaration - global;
    }

    auto expected = declaration->gen ? global[declaration->gen]->types.size() : 0;
    if(expected != args.length) {
        module.context.diagnostics.error("type %@ takes %@ arguments but was given %@"_v, source,
                                         module.context.findName(declaration->name), U32(expected), U32(args.length));
        return module.scalar.error;
    }

    if(!expected) return (Type*)declaration - global;

    for(auto existing: declaration->instances.contents(global)) {
        auto instance = (RecordType*)global[existing];
        auto equal = true;

        for(Size i = 0; i < args.length; i++) {
            if(instance->instanceArgs.get(global, i) != args[i]) {
                equal = false;
                break;
            }
        }

        if(equal) return (Type*)instance - global;
    }

    auto instance = new (module.types) RecordType(declaration->name);
    instance->instanceOf = pointer;
    instance->qualified = declaration->qualified;
    instance->generic = anyGeneric(global, args);

    for(auto arg: args) instance->instanceArgs.push(module.types, arg);

    // The constructor names are known immediately; only their contents need the declaration to
    // be complete. Registering the instance before filling it is what makes a recursive generic
    // type - `data List(a) = Nil | Cons({a, List(a)})` - terminate.
    for(Size i = 0; i < declaration->constructors.size(); i++) {
        auto constructor = declaration->constructors.get(global, i);
        instance->constructors.push(module.types, Constructor { constructor.name, nullptr, constructor.index });
    }

    declaration->instances.push(module.types, instance - global);

    if(declaration->definitionReady) {
        completeInstance(module, *instance);
    } else {
        module.program.pendingInstances.push(instance - global);
    }

    return (Type*)instance - global;
}

TypePtr substituteType(Module& module, TypePtr type, Buffer<TypePtr> args, LocationId source) {
    auto global = *module.types;
    if(!type || !isGeneric(global, type)) return type;

    switch(global[type]->kind) {
        case Type::Gen: {
            auto index = ((GenType*)global[type])->index;
            return index < args.length ? args[index] : type;
        }
        case Type::Record: {
            auto record = (RecordType*)global[type];
            if(!record->instanceOf) return type;

            Array<TypePtr> substituted;
            for(auto arg: record->instanceArgs.contents(global)) {
                substituted.push(substituteType(module, arg, args, source));
            }

            return instantiateRecord(module, record->instanceOf, toBuffer(substituted), source);
        }
        case Type::Tup: {
            auto tuple = (TupType*)global[type];
            Array<Field> fields;

            for(Size i = 0; i < tuple->fields.size(); i++) {
                auto field = tuple->fields.get(global, i);
                fields.push(Field { substituteType(module, field.type, args, source), field.name, 0 });
            }

            return (Type*)resolveTupleType(module, toBuffer(fields), source) - global;
        }
        case Type::Ptr:
            return resolvePointerType(module, substituteType(module, ((PtrType*)global[type])->to, args, source));
        default:
            return type;
    }
}

bool matchType(GlobalBase global, TypePtr pattern, TypePtr concrete, Buffer<TypePtr> bindings) {
    if(!pattern || !concrete) return false;

    if(global[pattern]->kind == Type::Gen) {
        auto index = ((GenType*)global[pattern])->index;
        if(index >= bindings.length) return false;

        // A variable that is already bound constrains rather than rebinds: `fn f(a, a)` called
        // with two different types has no instance.
        if(bindings[index]) return bindings[index] == concrete;

        bindings[index] = concrete;
        return true;
    }

    // Two identical types match with nothing to say about it - unless the pattern is generic, in
    // which case what it has to say is exactly which variable bound which. `Maybe(a)` against
    // `Maybe(a)` binds `a` to itself rather than binding nothing, so a caller that then substitutes
    // with the bindings gets the type back instead of a hole.
    if(pattern == concrete && !isGeneric(global, pattern)) return true;
    if(global[pattern]->kind != global[concrete]->kind) return false;

    switch(global[pattern]->kind) {
        case Type::Record: {
            auto patternRecord = (RecordType*)global[pattern];
            auto concreteRecord = (RecordType*)global[concrete];

            if(!patternRecord->instanceOf || patternRecord->instanceOf != concreteRecord->instanceOf) return false;
            if(patternRecord->instanceArgs.size() != concreteRecord->instanceArgs.size()) return false;

            for(Size i = 0; i < patternRecord->instanceArgs.size(); i++) {
                if(!matchType(global, patternRecord->instanceArgs.get(global, i),
                              concreteRecord->instanceArgs.get(global, i), bindings)) {
                    return false;
                }
            }

            return true;
        }
        case Type::Tup: {
            auto patternTuple = (TupType*)global[pattern];
            auto concreteTuple = (TupType*)global[concrete];
            if(patternTuple->fields.size() != concreteTuple->fields.size()) return false;

            for(Size i = 0; i < patternTuple->fields.size(); i++) {
                auto patternField = patternTuple->fields.get(global, i);
                auto concreteField = concreteTuple->fields.get(global, i);

                if(patternField.name != concreteField.name) return false;
                if(!matchType(global, patternField.type, concreteField.type, bindings)) return false;
            }

            return true;
        }
        case Type::Ptr:
            return matchType(global, ((PtrType*)global[pattern])->to, ((PtrType*)global[concrete])->to, bindings);
        default:
            // A kind with no structure to walk into matches only itself, which is what the identity
            // above already answered for everything except a generic type of such a kind.
            return pattern == concrete;
    }
}

/*
 * Resolving written types.
 */

static GlobalPtr<GenType> findGen(GlobalBase global, GenEnv* env, StringId name) {
    if(!env) return nullptr;

    for(auto type: env->types.contents(global)) {
        if(global[type]->name == name) return type;
    }

    return nullptr;
}

GlobalPtr<GenType> genVariable(Module& module, GenEnv& env, StringId name) {
    auto global = *module.types;
    if(auto existing = findGen(global, &env, name)) return existing;
    if(!env.open) return nullptr;

    auto type = new (module.types) GenType(&env - global, name, U16(env.types.size()));
    auto pointer = type - global;
    env.types.push(module.types, pointer);

    return pointer;
}

static TypePtr resolveTupleAst(Module& module, const ast::Type& type, GenEnv* env) {
    auto parseBase = module.parse;
    Array<Field> fields;
    auto astFields = type.tup.fields;

    for(auto astField: astFields.contents(parseBase)) {
        fields.push(Field {
            resolveType(module, astField.type, env),
            astField.name,
            0,
        });
    }

    return (Type*)resolveTupleType(module, toBuffer(fields), type.source) - *module.types;
}

static TypePtr resolveAlias(Module& module, TypeAlias& alias, Buffer<TypePtr> args, LocationId source);

// A named type with no arguments. A generic declaration written bare is an error rather than a
// partial application: higher-kinded use is a Milestone 2 concern and silently accepting it here
// would produce a type with no Repr much later.
static TypePtr resolveNamed(Module& module, StringId name, LocationId source) {
    auto global = *module.types;

    if(auto alias = findAlias(module, name, source)) {
        return resolveAlias(module, *alias.unwrap(), {}, source);
    }

    auto type = findType(module, name, source);
    if(!type) {
        module.context.diagnostics.error("unknown type %@"_v, source, module.context.findName(name));
        return module.scalar.error;
    }

    if(global[type]->kind == Type::Record) {
        auto record = (RecordType*)global[type];
        if(record->gen && global[record->gen]->types.size()) {
            module.context.diagnostics.error("type %@ requires type arguments"_v, source, module.context.findName(name));
            return module.scalar.error;
        }
    }

    return type;
}

static TypePtr resolveApp(Module& module, const ast::AppType& app, GenEnv* env, LocationId source) {
    auto global = *module.types;

    if(app.base.kind != ast::Type::Con) {
        return errorType(module, source, "only a named type can be applied to type arguments"_v);
    }

    Array<TypePtr> args;
    auto appArgs = app.args;
    for(auto arg: appArgs.contents(module.parse)) args.push(resolveType(module, arg, env));

    if(auto alias = findAlias(module, app.base.name, source)) {
        return resolveAlias(module, *alias.unwrap(), toBuffer(args), source);
    }

    auto type = findType(module, app.base.name, source);
    if(!type) {
        module.context.diagnostics.error("unknown type %@"_v, source, module.context.findName(app.base.name));
        return module.scalar.error;
    }

    if(global[type]->kind != Type::Record) {
        return errorType(module, source, "only a data type can take type arguments"_v);
    }

    return instantiateRecord(module, ((RecordType*)global[type])->base(global), toBuffer(args), source);
}

static TypePtr resolveAlias(Module& module, TypeAlias& alias, Buffer<TypePtr> args, LocationId source) {
    auto global = *module.types;
    auto expected = alias.gen ? global[alias.gen]->types.size() : 0;

    if(expected != args.length) {
        module.context.diagnostics.error("alias %@ takes %@ arguments but was given %@"_v, source,
                                         module.context.findName(alias.name), U32(expected), U32(args.length));
        return module.scalar.error;
    }

    if(!alias.resolved) {
        if(alias.resolving) {
            module.context.diagnostics.error("alias %@ is defined in terms of itself"_v, source,
                                             module.context.findName(alias.name));
            return module.scalar.error;
        }

        // The target is resolved in the module that declared the alias, so the names it uses
        // mean what they meant there rather than what they happen to mean here.
        auto& owner = *alias.module;
        alias.resolving = true;
        alias.resolved = resolveType(owner, owner.parse[alias.ast]->alias.target,
                                     alias.gen ? global[alias.gen] : nullptr);
        alias.resolving = false;
    }

    return expected ? substituteType(module, alias.resolved, args, source) : alias.resolved;
}

TypePtr resolveType(Module& module, const ast::Type& type, GenEnv* env) {
    switch(type.kind) {
        case ast::Type::Error:
            return module.scalar.error;
        case ast::Type::Unit:
            return module.scalar.unit;
        case ast::Type::Con:
            return resolveNamed(module, type.name, type.source);
        case ast::Type::Gen: {
            auto found = env ? genVariable(module, *env, type.name) : nullptr;
            if(!found) {
                module.context.diagnostics.error("unknown type variable %@ - it is not declared in this context"_v,
                                                 type.source, module.context.findName(type.name));
                return module.scalar.error;
            }

            return (Type*)(*module.types)[found] - *module.types;
        }
        case ast::Type::App:
            return resolveApp(module, *module.parse[type.app], env, type.source);
        case ast::Type::Tup:
            return resolveTupleAst(module, type, env);
        case ast::Type::Ptr:
            return resolvePointerType(module, resolveType(module, *module.parse[type.to], env));
        default:
            return errorType(module, type.source, "type is not available in this milestone"_v);
    }
}

// Pointers are interned on their target the way tuples are interned on their fields, so that the
// pointer written in a signature and the one an `addressOf` produces are the same TypePtr.
TypePtr resolvePointerType(Module& module, TypePtr to) {
    auto base = *module.types;
    if(!to) return module.scalar.error;

    for(auto pointer: module.program.pointerTypes.contents(base)) {
        if(base[pointer]->to == to) return (Type*)base[pointer] - base;
    }

    auto type = new (module.types) PtrType(to);
    type->generic = isGeneric(base, to);

    auto pointer = type - base;
    module.program.pointerTypes.push(module.types, pointer);

    return (Type*)type - base;
}

TupType* resolveTupleType(Module& module, Buffer<Field> requested, LocationId source) {
    auto base = *module.types;

    for(auto tuplePointer: module.program.tupleTypes.contents(base)) {
        auto tuple = base[tuplePointer];
        if(tuple->fields.size() != requested.length) continue;

        auto equal = true;
        for(Size i = 0; i < requested.length; i++) {
            auto existing = tuple->fields.get(base, i);
            if(existing.type != requested[i].type || existing.name != requested[i].name) {
                equal = false;
                break;
            }
        }

        if(equal) return tuple;
    }

    auto tuple = new (module.types) TupType;
    auto named = false;

    for(auto field: requested) {
        named = named || field.name != 0;
        tuple->generic = tuple->generic || isGeneric(base, field.type);
        tuple->fields.push(module.types, field);
    }

    tuple->named = named;
    module.program.tupleTypes.push(module.types, tuple - base);
    if(!tuple->generic) finishTupleRepr(module, *tuple, source);

    return tuple;
}

bool finishTupleRepr(Module& module, TupType& tuple, LocationId source) {
    if(tuple.reprReady) return true;
    if(tuple.generic) return false;

    if(tuple.resolvingRepr) {
        module.context.diagnostics.error("recursive tuple representation requires indirection"_v, source);
        return false;
    }

    tuple.resolvingRepr = true;
    auto base = *module.types;
    U32 size = 0;
    U32 alignment = 1;
    auto ready = true;

    for(Size i = 0; i < tuple.fields.size(); i++) {
        auto field = tuple.fields.get(base, i);
        if(field.type && base[field.type]->kind == Type::Record) {
            ready = finishRecordRepr(module, *(RecordType*)base[field.type], source) && ready;
        } else if(field.type && base[field.type]->kind == Type::Tup) {
            ready = finishTupleRepr(module, *(TupType*)base[field.type], source) && ready;
        }

        if(!ready) continue;

        auto fieldAlign = typeAlign(base, field.type);
        size = alignTo(size, fieldAlign);
        field.offset = size;
        tuple.fields.set(base, i, field);
        size += typeSize(base, field.type);
        alignment = max(alignment, fieldAlign);
    }

    tuple.resolvingRepr = false;
    if(!ready) return false;
    tuple.repr = { alignTo(size, alignment), alignment };
    tuple.virtualSize = U16(tuple.repr.size);
    tuple.reprReady = true;

    return true;
}

/*
 * Layout is a property of the declaration, not of one instantiation.
 *
 * A generic body projects into `Maybe(a)` before any `a` is known, so the projection it emits has
 * to be the one every instantiation uses. Deciding Enum/Single/Multi from the constructor list
 * alone gives that: the answer does not move when the arguments are substituted.
 *
 * The one place this costs something is a type variable substituted by `()`. `Box(())` keeps its
 * declaration's Multi layout and a zero-sized payload rather than collapsing to a discriminant,
 * which is a slightly larger value in exchange for `Box(a)` meaning one thing everywhere.
 */
void computeRecordLayout(GlobalBase base, RecordType& record) {
    if(record.constructors.size() == 1) {
        record.layout = RecordType::Single;
        return;
    }

    for(auto constructor: record.constructors.contents(base)) {
        // A generic content counts as a payload: what it substitutes to cannot change the shape.
        if(constructor.content && !isUnit(base, constructor.content)) {
            record.layout = RecordType::Multi;
            return;
        }
    }

    record.layout = RecordType::Enum;
}

bool finishRecordRepr(Module& module, RecordType& record, LocationId source) {
    if(record.reprReady) return true;
    if(record.generic || !record.definitionReady) return false;

    if(record.resolvingRepr) {
        module.context.diagnostics.error("recursive records require indirection, which is not available yet"_v, source);
        record.repr = { 0, 1 };
        return false;
    }

    record.resolvingRepr = true;
    auto base = *module.types;
    auto constructors = record.constructors.contents(base);

    // An instantiation inherits the declaration's layout rather than deciding its own.
    if(record.instanceOf) {
        record.layout = ((RecordType*)base[record.instanceOf])->layout;
    } else {
        computeRecordLayout(base, record);
    }

    if(record.layout == RecordType::Single) {
        auto content = constructors[0].content;

        if(content && base[content]->kind == Type::Record) {
            if(!finishRecordRepr(module, *(RecordType*)base[content], source)) {
                record.resolvingRepr = false;
                return false;
            }
        } else if(content && base[content]->kind == Type::Tup) {
            if(!finishTupleRepr(module, *(TupType*)base[content], source)) {
                record.resolvingRepr = false;
                return false;
            }
        }

        record.repr = content ? base[content]->repr : Repr {};
        record.payloadOffset = 0;
    } else {
        U32 payloadSize = 0;
        U32 payloadAlign = 1;

        for(auto constructor: constructors) {
            if(!constructor.content || isUnit(base, constructor.content)) continue;
            if(base[constructor.content]->kind == Type::Record) {
                if(!finishRecordRepr(module, *(RecordType*)base[constructor.content], source)) {
                    record.resolvingRepr = false;
                    return false;
                }
            } else if(base[constructor.content]->kind == Type::Tup) {
                if(!finishTupleRepr(module, *(TupType*)base[constructor.content], source)) {
                    record.resolvingRepr = false;
                    return false;
                }
            }

            payloadSize = max(payloadSize, typeSize(base, constructor.content));
            payloadAlign = max(payloadAlign, typeAlign(base, constructor.content));
        }

        record.payloadOffset = alignTo(4, payloadAlign);
        record.repr.align = max(4u, payloadAlign);
        record.repr.size = alignTo(record.payloadOffset + payloadSize, record.repr.align);

        if(record.layout == RecordType::Enum) record.repr.size = 4;
    }

    record.virtualSize = U16(record.repr.size);
    record.resolvingRepr = false;
    record.reprReady = true;
    return true;
}

bool sameType(TypePtr lhs, TypePtr rhs) {
    return lhs == rhs;
}

bool sameTypes(Buffer<TypePtr> lhs, Buffer<TypePtr> rhs) {
    if(lhs.length != rhs.length) return false;

    for(Size i = 0; i < lhs.length; i++) {
        if(!sameType(lhs[i], rhs[i])) return false;
    }

    return true;
}

bool isUnit(GlobalBase base, TypePtr type) {
    return type && base[type]->kind == Type::Unit;
}

bool isLiteral(GlobalBase base, TypePtr type) {
    return type && base[type]->kind == Type::Literal;
}

bool isInteger(GlobalBase base, TypePtr type) {
    return type && base[type]->kind == Type::Int;
}

bool isFloat(GlobalBase base, TypePtr type) {
    return type && base[type]->kind == Type::Float;
}

bool isPointer(GlobalBase base, TypePtr type) {
    return type && base[type]->kind == Type::Ptr;
}

TypePtr pointeeType(GlobalBase base, TypePtr type) {
    return isPointer(base, type) ? ((PtrType*)base[type])->to : nullptr;
}

bool isNumeric(GlobalBase base, TypePtr type) {
    return isInteger(base, type) || isFloat(base, type);
}

bool isDirectType(GlobalBase base, TypePtr type) {
    if(!type || isUnit(base, type)) return false;

    auto value = base[type];

    // A raw pointer is an address held in a register, not something held in memory: `%T` is
    // direct however large `T` is. The memory it names is reached through a place instead.
    if(value->kind == Type::Int || value->kind == Type::Float || value->kind == Type::Ptr) return true;

    return value->kind == Type::Record && ((RecordType*)value)->layout == RecordType::Enum;
}

bool isMemoryType(GlobalBase base, TypePtr type) {
    return type && !isUnit(base, type) && !isDirectType(base, type);
}

U32 typeSize(GlobalBase base, TypePtr type) {
    return type ? base[type]->repr.size : 0;
}

U32 typeAlign(GlobalBase base, TypePtr type) {
    return type ? base[type]->repr.align : 1;
}

void describeType(Context& context, GlobalBase base, TypePtr type, StringBuilder& target) {
    if(!type) {
        target << "<none>";
        return;
    }

    switch(base[type]->kind) {
        case Type::Error:
            target << "<error>";
            return;
        case Type::Unit:
            target << "()";
            return;
        case Type::Int: {
            // Core's Int and Native's I32 have the same shape and different names, so an integer
            // type says which one it is rather than describing its width.
            auto name = ((IntType*)base[type])->name;
            if(name) {
                target << context.findName(name);
                return;
            }

            switch(((IntType*)base[type])->width) {
                case IntType::Bool: target << "Bool"; return;
                case IntType::Int: target << "Int"; return;
                case IntType::Long: target << "Long"; return;
            }
            return;
        }
        case Type::Ptr:
            target << '%';
            describeType(context, base, ((PtrType*)base[type])->to, target);
            return;
        case Type::Float:
            target << (((FloatType*)base[type])->width == FloatType::Float ? "Float" : "Double");
            return;
        case Type::Gen:
            target << context.findName(((GenType*)base[type])->name);
            return;
        case Type::Literal: {
            // A literal variable only ever appears in a diagnostic about a literal whose type
            // nothing decided, so it says which classes it was waiting to be satisfied by.
            auto literal = (LiteralType*)base[type];
            target << '?';
            target.appendValue(literal->index);

            for(Size i = 0; i < literal->classes.size(); i++) {
                target << (i ? ", " : " (");
                target << context.findName(((TypeClass*)base[literal->classes.get(base, i)])->name);
            }

            if(literal->classes.size()) target << ')';
            return;
        }
        case Type::Tup: {
            auto tuple = (TupType*)base[type];
            target << '{';

            for(Size i = 0; i < tuple->fields.size(); i++) {
                if(i) target << ", ";
                auto field = tuple->fields.get(base, i);

                if(field.name) target << context.findName(field.name) << ": ";
                describeType(context, base, field.type, target);
            }

            target << '}';
            return;
        }
        case Type::Record: {
            auto record = (RecordType*)base[type];
            target << context.findName(record->name);

            if(record->instanceArgs.isNotEmpty()) {
                target << '(';
                Size index = 0;

                for(auto arg: record->instanceArgs.contents(base)) {
                    if(index++) target << ", ";
                    describeType(context, base, arg, target);
                }

                target << ')';
            }

            return;
        }
        default:
            target << "<unsupported>";
            return;
    }
}

void describeTypes(Context& context, GlobalBase base, Buffer<TypePtr> types, StringBuilder& target) {
    for(Size i = 0; i < types.length; i++) {
        if(i) target << ", ";
        describeType(context, base, types[i], target);
    }
}

String describeType(Context& context, GlobalBase base, TypePtr type) {
    StringBuilder buffer;
    describeType(context, base, type, buffer);
    return buffer.string();
}
