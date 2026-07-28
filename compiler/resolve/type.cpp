#include "type.h"
#include "generic.h"
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
        case Type::Borrow: {
            auto borrow = (BorrowType*)global[type];
            return resolveBorrowType(module, substituteType(module, borrow->to, args, source), borrow->mut);
        }
        case Type::Fun: {
            auto function = (FunType*)global[type];
            Array<FunArg> substituted;

            for(auto arg: function->args.contents(global)) {
                substituted.push(FunArg {
                    substituteType(module, arg.type, args, source), arg.name, arg.convention, arg.returnRoot,
                });
            }

            return resolveFunType(module, toBuffer(substituted),
                                  substituteType(module, function->result, args, source), function->kind);
        }
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
        case Type::Borrow: {
            auto patternBorrow = (BorrowType*)global[pattern];
            auto concreteBorrow = (BorrowType*)global[concrete];

            if(patternBorrow->mut != concreteBorrow->mut) return false;
            return matchType(global, patternBorrow->to, concreteBorrow->to, bindings);
        }
        case Type::Fun: {
            auto patternFun = (FunType*)global[pattern];
            auto concreteFun = (FunType*)global[concrete];

            // The conventions and the `return` group have to agree exactly rather than being
            // inferred from the match, for the reason FunArg gives: they are the contract a caller
            // reading only the type has, so a match that ignored them would let a `&` parameter
            // bind to a signature that takes a copy.
            if(patternFun->kind != concreteFun->kind) return false;
            if(patternFun->args.size() != concreteFun->args.size()) return false;
            if(patternFun->returnRoots != concreteFun->returnRoots) return false;

            for(Size i = 0; i < patternFun->args.size(); i++) {
                auto patternArg = patternFun->args.get(global, i);
                auto concreteArg = concreteFun->args.get(global, i);

                if(patternArg.convention != concreteArg.convention) return false;
                if(!matchType(global, patternArg.type, concreteArg.type, bindings)) return false;
            }

            return matchType(global, patternFun->result, concreteFun->result, bindings);
        }
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
    invalidateGenSchema(env);

    return pointer;
}

/*
 * The canonical numbering.
 *
 * Built in the order Implementation-Generics.md part 2 gives, and the order matters for exactly one
 * reason: emitted code loads slot N, so the caller filling the environment and the callee reading it
 * have to agree on what N is without either of them consulting the other. Deriving the numbering
 * from the finished context is what makes them agree - two compilations of the same context produce
 * the same numbers, however the requirements were discovered.
 *
 * Within each group the source order is kept rather than sorted. The declared variables and the
 * explicit constraints have a written order that a diagnostic can point at, and the inferred ones
 * are deduplicated structurally as they are added (see requireClass), so what is left is already
 * the order in which the body first needed each of them.
 */
GenSchema& genSchemaOf(Module& module, GenEnv& env) {
    auto global = *module.types;
    if(env.schema) return *global[env.schema];

    auto schema = new (module.types) GenSchema;
    env.schema = schema - global;

    U16 index = 0;

    // 1. The type variables, in declaration order, then the derived type expressions the body
    //    needed. Both are TypeDesc slots and are numbered together, because what distinguishes them
    //    is where they came from rather than what a reader does with them.
    for(auto variable: env.types.contents(global)) {
        GenSlot slot;
        slot.kind = GenSlotKind::Type;
        slot.index = index++;
        slot.type = (Type*)global[variable] - global;
        slot.name = global[variable]->name;
        schema->slots.push(module.types, slot);
    }

    // Applied expressions the body constructs or matches - `Maybe(a)`, `Pair(a, b)`. Their
    // descriptors are built once by the caller rather than by applying a type constructor at run
    // time, which is what keeps ordinary rank-1 generics away from higher-kinded machinery.
    for(auto derived: env.derivedTypes.contents(global)) {
        GenSlot slot;
        slot.kind = GenSlotKind::Type;
        slot.index = index++;
        slot.type = derived;
        schema->slots.push(module.types, slot);
    }

    schema->typeCount = index;

    // 2. The class constraints, declared ones and inferred ones alike. By the time anything reads
    //    the numbering the two are the same entry, which is the point of recording them in one list.
    for(auto constraint: env.classes.contents(global)) {
        if(!constraint.typeClass) continue;

        GenSlot slot;
        slot.kind = GenSlotKind::Class;
        slot.index = index++;
        slot.typeClass = constraint.typeClass;
        slot.name = constraint.name;
        slot.source = constraint.source;

        for(auto arg: constraint.args.contents(global)) slot.args.push(module.types, arg);
        schema->slots.push(module.types, slot);
    }

    for(auto constraint: env.properties.contents(global)) {
        GenSlot slot;
        slot.kind = GenSlotKind::Property;
        slot.index = index++;
        slot.type = constraint.owner;
        slot.result = constraint.result;
        slot.name = constraint.field;
        slot.source = constraint.source;
        schema->slots.push(module.types, slot);
    }

    for(auto constraint: env.functions.contents(global)) {
        GenSlot slot;
        slot.kind = GenSlotKind::Function;
        slot.index = index++;
        slot.type = constraint.signature;
        slot.name = constraint.name;
        slot.source = constraint.source;
        schema->slots.push(module.types, slot);
    }

    return *schema;
}

U16 genTypeSlot(Module& module, GenEnv& env, TypePtr type) {
    auto global = *module.types;
    auto& schema = genSchemaOf(module, env);

    for(auto slot: schema.slots.contents(global)) {
        if(slot.kind == GenSlotKind::Type && slot.type == type) return slot.index;
    }

    return maxLimit<U16>;
}

U16 genClassSlot(Module& module, GenEnv& env, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args) {
    auto global = *module.types;
    auto& schema = genSchemaOf(module, env);

    for(auto slot: schema.slots.contents(global)) {
        if(slot.kind != GenSlotKind::Class || slot.typeClass != typeClass) continue;
        if(sameTypes(slot.args, global, args)) return slot.index;
    }

    return maxLimit<U16>;
}

U16 genPropertySlot(Module& module, GenEnv& env, TypePtr owner, StringId field) {
    auto global = *module.types;
    auto& schema = genSchemaOf(module, env);

    for(auto slot: schema.slots.contents(global)) {
        if(slot.kind == GenSlotKind::Property && slot.type == owner && slot.name == field) {
            return slot.index;
        }
    }

    return maxLimit<U16>;
}

U16 genFunctionSlot(Module& module, GenEnv& env, StringId name, TypePtr signature) {
    auto global = *module.types;
    auto& schema = genSchemaOf(module, env);

    for(auto slot: schema.slots.contents(global)) {
        if(slot.kind == GenSlotKind::Function && slot.name == name && slot.type == signature) {
            return slot.index;
        }
    }

    return maxLimit<U16>;
}

void requireTypeSlot(Module& module, GenEnv& env, TypePtr type) {
    auto global = *module.types;
    if(!type || !isGeneric(global, type)) return;

    // A bare type variable already has a slot from the declaration, and a duplicate applied
    // expression is one slot however many times the body writes it.
    if(genTypeSlot(module, env, type) != maxLimit<U16>) return;
    if(global[type]->kind == Type::Gen) return;

    // Recorded on the context rather than on the schema, so that rebuilding the numbering produces
    // the same answer however many times it happens - see genSchemaOf. Derived expressions stay
    // inside the leading run of TypeDesc slots, which is what keeps `typeCount` meaningful.
    env.derivedTypes.push(module.types, type);
    invalidateGenSchema(env);
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

/*
 * A written function type - `(&a: Int, return b: T) -> &T`.
 *
 * The conventions and the `return` markers are read here rather than dropped, which is the whole
 * point of Implementation-IR.md part 3's "the natural home is FunArg": a caller holding one of these
 * has to know what the callee does to each argument, and this is the only place it can find out.
 * The same validity rules apply as to a declaration, so both go through checkReturnRoot.
 */
static TypePtr resolveFunTypeAst(Module& module, const ast::FunType& type, GenEnv* env, LocationId source) {
    auto parseBase = module.parse;
    Array<FunArg> args;
    auto allRootsMutable = true;
    auto roots = 0u;
    auto written = 0u;
    U32 index = 0;

    auto declaredArgs = type.args;

    for(auto declared: declaredArgs.contents(parseBase)) {
        FunArg arg;
        arg.type = resolveType(module, declared.type, env);
        arg.name = declared.name;
        arg.convention = declared.bind;

        if(declared.returnRoot) {
            written++;

            if(checkReturnRoot(module, arg.type, declared.bind, index, source)) {
                arg.returnRoot = true;
                roots++;
                if(declared.bind != ast::BindType::Ref) allRootsMutable = false;
            }
        }

        args.push(arg);
        index++;
    }

    auto result = resolveType(module, type.ret, env);

    if(isBorrow(*module.types, result)) {
        if(!roots && !written) {
            module.context.diagnostics.error("a function type returning a borrow must mark the argument it is rooted in with `return`"_v,
                                             source);
        } else if(roots) {
            result = applyReturnRootMutability(module, result, allRootsMutable);
        }
    }

    return resolveFunType(module, toBuffer(args), result, type.kind);
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
        case ast::Type::Arr: {
            // `[T]` is the growable array, which is an ordinary generic record declared in
            // Collections rather than a type kind: the grammar has a spelling for it, and what the
            // spelling means is a library type. `[T *n]` - the fixed-size inline one - has no
            // implementation behind it yet, so it is rejected rather than silently made growable.
            if(type.arr.length) {
                return errorType(module, type.source, "fixed-size arrays are not available yet"_v);
            }

            if(!module.program.arrayType) {
                return errorType(module, type.source, "arrays are not available in this module"_v);
            }

            auto element = resolveType(module, *module.parse[type.arr.type], env);
            return instantiateRecord(module, module.program.arrayType, { &element, 1 }, type.source);
        }
        case ast::Type::Borrow:
            // Immutable until the signature it belongs to says otherwise: what makes a returned
            // borrow exclusive is the return-root group being entirely `return &`, which is not
            // known until every argument of the declaration has been read.
            return resolveBorrowType(module, resolveType(module, *module.parse[type.to], env), false);
        case ast::Type::Fun:
            return resolveFunTypeAst(module, *module.parse[type.fun], env, type.source);
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

// Borrows are interned the way pointers are, on their target and on the one bit that distinguishes
// an exclusive borrow from a shared one.
TypePtr resolveBorrowType(Module& module, TypePtr to, bool mut) {
    auto base = *module.types;
    if(!to) return module.scalar.error;

    for(auto pointer: module.program.borrowTypes.contents(base)) {
        auto borrow = base[pointer];
        if(borrow->to == to && borrow->mut == mut) return (Type*)borrow - base;
    }

    auto type = new (module.types) BorrowType(to, mut);
    type->generic = isGeneric(base, to);

    module.program.borrowTypes.push(module.types, type - base);
    return (Type*)type - base;
}

/*
 * Function types are interned on their whole signature, conventions and `return` markers included.
 *
 * The argument *names* are not part of the key, so the first spelling of a signature wins and every
 * later one shares it. That is deliberate: `(a: Int) -> Int` and `(Int) -> Int` accept exactly the
 * same calls, and making them two types would make a name in a signature an API commitment.
 */
TypePtr resolveFunType(Module& module, Buffer<FunArg> args, TypePtr result, ast::FunKind kind) {
    auto base = *module.types;
    if(!result) return module.scalar.error;

    for(auto arg: args) {
        if(!arg.type) return module.scalar.error;
    }

    for(auto pointer: module.program.funTypes.contents(base)) {
        auto candidate = base[pointer];
        if(candidate->result != result || candidate->kind != kind) continue;
        if(candidate->args.size() != args.length) continue;

        auto equal = true;
        for(Size i = 0; i < args.length; i++) {
            auto existing = candidate->args.get(base, i);
            if(existing.type != args[i].type || existing.convention != args[i].convention ||
               existing.returnRoot != args[i].returnRoot) {
                equal = false;
                break;
            }
        }

        if(equal) return (Type*)candidate - base;
    }

    auto type = new (module.types) FunType;
    type->result = result;
    type->kind = kind;
    type->generic = isGeneric(base, result);

    for(Size i = 0; i < args.length; i++) {
        type->args.push(module.types, args[i]);
        if(args[i].returnRoot && i < 64) type->returnRoots |= U64(1) << i;
        if(isGeneric(base, args[i].type)) type->generic = true;
    }

    module.program.funTypes.push(module.types, type - base);
    return (Type*)type - base;
}

/*
 * What may carry the `return` marker, per Design-Memory §5.2.
 *
 * All three rules are about the same thing: the marker says a borrow in the result may be rooted in
 * the caller's storage for this argument, so the argument has to *have* caller storage that
 * survives the call. A sunk one does not - the callee owns it - and a TrivialCopy one passed by the
 * default convention does not either, since what the body sees is a copy of its own.
 */
bool checkReturnRoot(Module& module, TypePtr type, ast::BindType convention, U32 index, LocationId source) {
    auto base = *module.types;

    if(convention == ast::BindType::Sink) {
        module.context.diagnostics.error("`return` cannot be written on a `->` argument - the callee owns what it was given, so there is no caller-side storage left for a result to be rooted in"_v,
                                         source);
        return false;
    }

    // Design-Memory states this rule over TrivialCopy, which is the same rule one step earlier:
    // what disqualifies a parameter is arriving as a copy rather than as an address. Here that is
    // exactly a direct type - a scalar in a register - while a TrivialCopy *aggregate* still
    // arrives as the caller's address and can root a borrow of it perfectly well.
    //
    // A raw pointer is the exception among direct types: the copy it arrives as *is* an address, so
    // what it names is still the caller's. `return %a` is how Native's `borrow` says that its
    // result points into whatever it was given, which is the one bridge from unchecked memory back
    // into checked borrows.
    if(convention != ast::BindType::Ref && isDirectType(base, type) && !isPointer(base, type)) {
        module.context.diagnostics.error("`return` on %@ has nothing to root a borrow in - it arrives in a register, so the body sees a copy of its own; write `return &` when the caller's storage must be the root"_v,
                                         source, describeType(module.context, base, type));
        return false;
    }

    // The group is a bit set, and a signature this wide has never been written. Saying so is better
    // than silently dropping a marker the caller would then rely on.
    if(index >= 64) {
        module.context.diagnostics.error("`return` cannot be written past the 64th argument"_v, source);
        return false;
    }

    return true;
}

TypePtr applyReturnRootMutability(Module& module, TypePtr result, bool allRootsMutable) {
    if(!allRootsMutable || !isBorrow(*module.types, result)) return result;
    return resolveBorrowType(module, ((BorrowType*)(*module.types)[result])->to, true);
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

/*
 * Ownership classification (Implementation-IR.md part 4).
 *
 * Three questions, answered from the shape of the type plus whatever instances the program wrote:
 * can this be duplicated by copying its bytes, can it be relocated by copying its bytes, and does
 * the end of its lifetime run anything. The order below matters - the authored instances are
 * looked for first, because writing one is exactly the statement that the structural answer is
 * wrong for this type.
 */

// Whether the program wrote `instance Class(type)`. A class the program never declared (no Core,
// or a module built before Core's classes exist) has no instances, which is the right answer
// rather than a reason to check.
static bool hasInstance(Module& module, GlobalPtr<TypeClass> typeClass, TypePtr type) {
    if(!typeClass) return false;

    TypePtr args[] = { type };
    return findInstance(module, typeClass, toBuffer(args)) != nullptr;
}

// Folds one member into an aggregate's classification. A member that is not trivial makes the whole
// aggregate not trivial, and a member with either half of a teardown gives the aggregate a derived
// half of the same kind - which is the whole of "recurse into each field, then release this type's
// own storage". The two halves are folded independently, which is the point of splitting them: a
// member with only a `Reclaim` must not give its container a `Drop` and cost it region eligibility.
static void includeMember(Module& module, TypePtr member, Ownership& target) {
    auto inner = ownershipOf(module, member);

    target.trivialCopy = target.trivialCopy && inner.trivialCopy;
    target.trivialSink = target.trivialSink && inner.trivialSink;

    if(inner.reclaim != TeardownKind::None && target.reclaim == TeardownKind::None) {
        target.reclaim = TeardownKind::Derived;
    }

    if(inner.drop != TeardownKind::None && target.drop == TeardownKind::None) {
        target.drop = TeardownKind::Derived;
    }
}

Ownership ownershipOf(Module& module, TypePtr type) {
    auto base = *module.types;
    if(!type) return Ownership {};

    auto value = base[type];
    if(value->ownershipReady) return value->ownership;

    // A type reached from inside its own classification can only be a declaration that contains
    // itself without an indirection, which has no finite value. Answering conservatively rather
    // than recursing leaves the diagnostic to whoever computes its Repr, which is where an
    // infinitely large type is already reported.
    if(value->resolvingOwnership) {
        return Ownership { false, false, false, false, TeardownKind::Derived, TeardownKind::Derived };
    }

    Ownership result;
    value->resolvingOwnership = true;

    switch(value->kind) {
        case Type::Error:
        case Type::Unit:
        case Type::Int:
        case Type::Float:
        case Type::Literal:
            // The scalars, and the literal variable that becomes one. Nothing to release, and
            // both duplication and relocation are the bytes themselves.
            break;

        case Type::Ptr:
            // A raw pointer is an address, and an address is TrivialCopy by Design.md's own list.
            // Whatever it points at is owned by something else - that is what makes `%T` unsafe
            // and what keeps it out of this analysis entirely.
            break;

        case Type::Borrow:
            // A borrow owns nothing, so it releases nothing and relocates by copying its address.
            // Only the exclusive one is kept out of TrivialCopy: duplicating a mutable borrow on
            // read would hand out a second exclusive access to one place, which is the one thing
            // exclusivity means. An immutable borrow may be duplicated freely, which is exactly
            // Design.md's "any number of immutable borrows can be alive simultaneously".
            result.trivialCopy = ((BorrowType*)value)->mut == false;
            break;

        case Type::Gen:
            // Design.md: an unconstrained generic parameter is treated as non-TrivialCopy inside
            // the body regardless of what a caller substitutes, so that a generic function's
            // accepted programs are fixed by its own signature. The same argument applies to the
            // other two: the body must be written as though the type owns something.
            result = Ownership { false, false, false, false, TeardownKind::Derived, TeardownKind::Derived };
            break;

        case Type::Tup: {
            auto tuple = (TupType*)value;
            for(auto field: tuple->fields.contents(base)) includeMember(module, field.type, result);
            break;
        }

        case Type::Record: {
            auto record = (RecordType*)value;

            // An enum-layout record is a discriminant and nothing else, so there are no member
            // types to fold and the scalar answer is already right.
            if(record->layout != RecordType::Enum) {
                for(auto constructor: record->constructors.contents(base)) {
                    if(constructor.content) includeMember(module, constructor.content, result);
                }
            }

            break;
        }

        case Type::Fun:
            // A function value owns the environment its captures live in (Design-Memory §8), so a
            // bitwise duplicate would alias that environment and it is not TrivialCopy. It *is*
            // TrivialSink - relocating it moves three words and the environment keeps its address -
            // and its teardown is derived: run the environment descriptor's drop, if it has one.
            //
            // That answer is the same for a non-capturing lambda, whose descriptor is null. Making
            // it depend on what one value captured would make ownership a property of a value
            // rather than of a type, which is exactly what the model does not allow.
            result = Ownership { false, true, false, false, TeardownKind::Derived, TeardownKind::Derived };
            break;

        default:
            // Ref, RegionPtr, Region, Array and Map. None of them are constructible yet;
            // classifying them conservatively is what makes adding one a decision rather than a
            // silently wrong default.
            result = Ownership { false, false, false, false, TeardownKind::Derived, TeardownKind::Derived };
            break;
    }

    // An authored instance overrides the structural answer, which is what writing one means. A
    // generic declaration is skipped: `Maybe(a)` is not a type anything can have an instance for,
    // and asking would match the instance of whatever `a` last resolved to.
    if(!value->generic) {
        if(hasInstance(module, module.coreClasses.reclaim, type)) result.reclaim = TeardownKind::Authored;
        if(hasInstance(module, module.coreClasses.drop, type)) result.drop = TeardownKind::Authored;
        if(hasInstance(module, module.coreClasses.copy, type)) result.authoredCopy = true;

        if(hasInstance(module, module.coreClasses.sink, type)) {
            result.authoredSink = true;
            result.trivialSink = false;
        }
    }

    // Duplicating a value whose lifetime releases something would release it twice, so a teardown
    // of either kind rules out TrivialCopy. This is stated once here rather than at each producer
    // of one above, because it holds for the authored cases as well as the derived ones.
    if(result.needsTeardown()) result.trivialCopy = false;

    value->resolvingOwnership = false;
    value->ownership = result;
    value->ownershipReady = true;
    return result;
}

/*
 * The context-sensitive half of the classification.
 *
 * This mirrors ownershipOf's structural fold, and differs from it in exactly one place: at a type
 * variable, where the answer comes from what the context declared rather than from the type. The
 * result is never cached, because two contexts can legitimately disagree about the same `a`.
 *
 * `depth` bounds the walk the way instance proving does. A type reachable from itself without an
 * indirection has no finite value and is reported by whoever computes its Repr.
 */
static Ownership ownershipInAt(Module& module, GenEnv* env, TypePtr type, U32 depth) {
    auto base = *module.types;
    if(!type || !isGeneric(base, type) || !depth) return ownershipOf(module, type);

    auto value = base[type];

    switch(value->kind) {
        case Type::Gen: {
            auto result = ownershipOf(module, type);
            TypePtr args[] = { type };

            if(env) {
                if(provesClass(module, *env, module.coreClasses.trivialCopy, toBuffer(args))) {
                    result.trivialCopy = true;
                    result.reclaim = TeardownKind::None;
                    result.drop = TeardownKind::None;
                }

                if(provesClass(module, *env, module.coreClasses.trivialSink, toBuffer(args))) {
                    result.trivialSink = true;
                }
            }

            return result;
        }

        case Type::Tup: {
            Ownership result;
            for(auto field: ((TupType*)value)->fields.contents(base)) {
                auto inner = ownershipInAt(module, env, field.type, depth - 1);
                result.trivialCopy = result.trivialCopy && inner.trivialCopy;
                result.trivialSink = result.trivialSink && inner.trivialSink;
                if(inner.reclaim != TeardownKind::None) result.reclaim = TeardownKind::Derived;
                if(inner.drop != TeardownKind::None) result.drop = TeardownKind::Derived;
            }

            if(result.needsTeardown()) result.trivialCopy = false;
            return result;
        }

        case Type::Record: {
            auto record = (RecordType*)value;
            Ownership result;

            if(record->layout != RecordType::Enum) {
                for(auto constructor: record->constructors.contents(base)) {
                    if(!constructor.content) continue;

                    auto inner = ownershipInAt(module, env, constructor.content, depth - 1);
                    result.trivialCopy = result.trivialCopy && inner.trivialCopy;
                    result.trivialSink = result.trivialSink && inner.trivialSink;
                    if(inner.reclaim != TeardownKind::None) result.reclaim = TeardownKind::Derived;
                    if(inner.drop != TeardownKind::None) result.drop = TeardownKind::Derived;
                }
            }

            // A generic instantiation cannot be asked for an authored instance - `Maybe(a)` is not
            // a type anything writes one for - so the structural answer is the whole answer here,
            // exactly as it is in ownershipOf.
            if(result.needsTeardown()) result.trivialCopy = false;
            return result;
        }

        default:
            return ownershipOf(module, type);
    }
}

Ownership ownershipIn(Module& module, GenEnv* env, TypePtr type) {
    return ownershipInAt(module, env, type, 8);
}

bool needsTeardown(Module& module, TypePtr type) {
    return ownershipOf(module, type).needsTeardown();
}

bool needsDrop(Module& module, TypePtr type) {
    return ownershipOf(module, type).drop != TeardownKind::None;
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

bool isBorrow(GlobalBase base, TypePtr type) {
    return type && base[type]->kind == Type::Borrow;
}

bool isFunction(GlobalBase base, TypePtr type) {
    return type && base[type]->kind == Type::Fun;
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
    // direct however large `T` is. The memory it names is reached through a place instead, and a
    // borrow is the same shape with checking attached.
    if(value->kind == Type::Int || value->kind == Type::Float || value->kind == Type::Ptr ||
       value->kind == Type::Borrow) {
        return true;
    }

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
        case Type::Borrow:
            // `&T` is how a borrow is written; `&mut T` is a printed form rather than a source one,
            // since what makes a returned borrow mutable is the group it is rooted in rather than
            // anything written on the result - see resolveSignature.
            target << (((BorrowType*)base[type])->mut ? "&mut " : "&");
            describeType(context, base, ((BorrowType*)base[type])->to, target);
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
        case Type::Fun: {
            // Printed the way it is written, conventions and markers included, because those are
            // what two otherwise identical signatures differ in and a diagnostic that dropped them
            // would name two types the same way.
            auto function = (FunType*)base[type];
            if(function->kind == ast::FunKind::Lens) target << "lens ";
            else if(function->kind == ast::FunKind::Iter) target << "iter ";

            target << '(';
            Size index = 0;

            for(auto arg: function->args.contents(base)) {
                if(index++) target << ", ";
                if(arg.returnRoot) target << "return ";
                if(arg.convention == ast::BindType::Ref) target << '&';
                else if(arg.convention == ast::BindType::Sink) target << "->";
                if(arg.name) target << context.findName(arg.name) << ": ";

                describeType(context, base, arg.type, target);
            }

            target << ") -> ";
            describeType(context, base, function->result, target);
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
