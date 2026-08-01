#include "type.h"
#include "generic.h"
#include "module.h"
#include "name.h"

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

    TypeList args;
    for(auto arg: instance.instanceArgs.contents(global)) args.push(arg);

    for(Size i = 0; i < declaration->constructors.size(); i++) {
        auto constructor = declaration->constructors.get(global, i);
        constructor.content = constructor.content
            ? substituteType(module, constructor.content, toBuffer(args), kNullLocation)
            : nullptr;

        instance.constructors.set(global, i, constructor);
    }

    instance.layout = declaration->layout;
    instance.pinned = declaration->pinned;
    instance.definitionReady = true;

    /*
     * An instantiation is the first point at which a generic declaration has a layout to be cyclic:
     * `Tree(a)` is a shape, and it is `Tree(Int)` that contains a `Maybe(Tree(Int))` that contains a
     * `Tree(Int)`. So the indirection is chosen here rather than on the declaration.
     *
     * Reaching this while some other declaration is still being defined is normal - the substitution
     * above runs during the module's type phase - and such a walk finds nothing and remembers
     * nothing. The declaration pass asks again once everything is resolved.
     */
    breakLayoutCycles(module, (Type*)&instance - global, kNullLocation);
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

// One instantiation's argument, when it is an instantiation of `of` at all.
static TypePtr instanceArgument(Module& module, GlobalPtr<RecordType> of, TypePtr type) {
    auto global = *module.types;
    if(!of || !type || global[type]->kind != Type::Record) return nullptr;

    auto record = (RecordType*)global[type];
    if(record->instanceOf != of || record->instanceArgs.size() != 1) return nullptr;

    return record->instanceArgs.get(global, 0);
}

TypePtr arrayElement(Module& module, TypePtr type) {
    return instanceArgument(module, module.program.arrayType, type);
}

TypePtr sliceElement(Module& module, TypePtr type) {
    return instanceArgument(module, module.program.sliceType, type);
}

TypePtr sliceOf(Module& module, TypePtr type) {
    if(!module.program.sliceType) return nullptr;

    // A borrow of a slice is that slice: `Flat(T)` owns nothing, so there is nothing for a second
    // level of borrowing to describe and no conversion to perform.
    if(sliceElement(module, type)) return type;

    auto element = arrayElement(module, type);
    if(!element) return nullptr;

    return instantiateRecord(module, module.program.sliceType, { &element, 1 }, kNullLocation);
}

bool isBorrowLike(Module& module, TypePtr type) {
    return isBorrow(*module.types, type) || sliceElement(module, type) != nullptr;
}

static bool containsBorrowLikeAt(Module& module, TypePtr type, U32 depth) {
    if(!type || !depth) return false;
    if(isBorrowLike(module, type)) return true;

    auto base = *module.types;
    auto value = base[type];

    // A raw pointer is not descended into and neither is what it points at - `%T` is where this
    // analysis stops by construction. An owned container therefore does not contain a reference:
    // `Array(T)` holds a `Run(T)` holds a `%T`, and nothing on that path is a borrow.
    if(value->kind == Type::Tup) {
        for(auto field: ((TupType*)value)->fields.contents(base)) {
            if(containsBorrowLikeAt(module, field.type, depth - 1)) return true;
        }
    } else if(value->kind == Type::Record && ((RecordType*)value)->layout != RecordType::Enum) {
        for(auto constructor: ((RecordType*)value)->constructors.contents(base)) {
            if(containsBorrowLikeAt(module, constructor.content, depth - 1)) return true;
        }
    }

    return false;
}

bool containsBorrowLike(Module& module, TypePtr type) {
    return containsBorrowLikeAt(module, type, 8);
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

            TypeList substituted;
            for(auto arg: record->instanceArgs.contents(global)) {
                substituted.push(substituteType(module, arg, args, source));
            }

            return instantiateRecord(module, record->instanceOf, toBuffer(substituted), source);
        }
        case Type::Tup: {
            auto tuple = (TupType*)global[type];
            SmallArray<Field, 8> fields;

            for(Size i = 0; i < tuple->fields.size(); i++) {
                auto field = tuple->fields.get(global, i);
                // The indirection survives substitution: it is a property of the field rather than
                // of what the field holds, exactly as the pinned layout below is.
                fields.push(Field { substituteType(module, field.type, args, source), field.name,
                                    field.boxed });
            }

            // The layout the tuple was pinned to survives substitution: `@layout(c)` on a generic
            // record is a promise about every instantiation of it, not about the declaration.
            return (Type*)resolveTupleType(module, toBuffer(fields), source, tuple->layout) - global;
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
                    arg.lazy,
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

    /*
     * A `@bits` refinement matches what it refines.
     *
     * This is the load-bearing half of "`@bits(n)` never participates in typeclass dispatch": every
     * instance head is matched through here, so `instance Num(U64)` answers `Num(Id)` and nobody
     * writes an instance per width. It is deliberately one-directional in effect rather than in
     * form - both sides canonicalize, so `Id` also matches an instance written for `Id`, and the
     * refinement is invisible to selection either way.
     */
    if(global[pattern]->kind == Type::Int) {
        return canonicalType(global, pattern) == canonicalType(global, concrete);
    }

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
                if(patternArg.lazy != concreteArg.lazy) return false;
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

    for(auto constraint: env.dispatched.contents(global)) {
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

void requireClassSlot(Module& module, GenEnv& env, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args,
                      LocationId source) {
    if(!typeClass) return;

    // A witness the context can already reach needs no slot of its own, whether it sits in one
    // directly or inside one that names it as a superclass. The second is what keeps
    // `fn (Num(a)) inc(x: a) = x + 1` to a single witness: the literal's `FromInt(a)` is loaded out
    // of the `Num` witness the caller already passed.
    Array<U32> supers;
    if(genWitnessPath(module, env, typeClass, args, supers) != maxLimit<U16>) return;

    ClassConstraint constraint;
    constraint.typeClass = typeClass;
    constraint.name = (*module.types)[typeClass]->name;
    constraint.source = source;
    for(auto arg: args) constraint.args.push(module.types, arg);

    env.dispatched.push(module.types, constraint);
    invalidateGenSchema(env);
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

static bool readBoxAttribute(Module& module, const ast::Type& type);
static bool hasAttribute(Module& module, ast::ParsePtr<ast::AttrList> attributes, const char* name, U32 length);

static TypePtr resolveTupleAst(Module& module, const ast::Type& type, GenEnv* env) {
    auto parseBase = module.parse;
    Array<Field> fields;
    auto astFields = type.tup.fields;

    for(auto astField: astFields.contents(parseBase)) {
        auto boxed = readBoxAttribute(module, astField.type);
        auto declared = astField.type;

        /*
         * The attribute is spent here, so the type is resolved without it.
         *
         * That is the whole of why `@box` is not a type refinement the way `@bits` is: the field's
         * declared type is what the field holds, and everything downstream - `f(cfg.cold)`, a
         * pattern binding, a diagnostic - is entitled to see exactly what was written. Stripping
         * the list rather than only the one attribute is safe because readBoxAttribute has already
         * rejected the one combination that could have been in it.
         */
        if(hasAttribute(module, declared.attributes, "box", 3)) declared.attributes = nullptr;

        fields.push(Field { resolveType(module, declared, env), astField.name, boxed });
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
/*
 * What a parameter's written type means, which is not always what the same syntax means elsewhere.
 *
 * `[T]` in a *binding* position is a slice - Implementation-Containers.md §4. The default binding
 * convention is an immutable borrow and `&` makes it a mutable one, and what a borrow of a
 * contiguous container *is* is a `{base, length}` descriptor rather than an address of the owner.
 * That is the one fixed and universal thing in the container design: a borrow of `[T]` has one
 * concrete representation and never dispatches, which is what makes "no polymorphic calls by
 * default" true by construction.
 *
 * Three positions deliberately keep the owner:
 *
 *  - `->xs: [T]`, which consumes the container rather than looking at it;
 *  - a field, a `::` ascription and a return type, which are *type* positions and have no
 *    convention to read - `data F {xs: [T]}` owns an array, and `data F {xs: &[T]}` is how a stored
 *    slice is spelled (see resolveType's Borrow case);
 *  - `xs: Array(T)` written out, which is how Collections' own operations name the growable type.
 *    Growth is nominal, because only the growable type can grow: `push` says `Array(T)` and `sort`
 *    says `[T]`, and the difference between them is exactly this function.
 */
TypePtr bindingType(Module& module, const ast::Type& written, ast::BindType bind, GenEnv* env) {
    auto type = resolveType(module, written, env);
    if(bind == ast::BindType::Sink) return type;
    if(written.kind != ast::Type::Arr || written.arr.length) return type;

    auto slice = sliceOf(module, type);
    return slice ? slice : type;
}

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
        arg.type = bindingType(module, declared.type, declared.bind, env);
        arg.name = declared.name;
        arg.convention = declared.bind;
        arg.lazy = declared.lazy && checkLazyArgument(module, declared.bind, declared.returnRoot, source);

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

    TypeList args;
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

/*
 * The `@bits(n)` an attribute list carries, or zero.
 *
 * Everything else written as an attribute on a type is left alone rather than rejected, because the
 * grammar accepts `@name(args)` in this position for features that do not exist yet and turning
 * "not implemented" into "not allowed" here would have to be undone by each of them. `@box` is the
 * one that has landed, and it is read by readBoxAttribute below rather than here, because it
 * refines the *field* and not the type.
 */
static bool readBitsAttribute(Module& module, ast::ParsePtr<ast::AttrList> attributes, LocationId source,
                              U32& bits) {
    if(!attributes) return false;

    auto parse = module.parse;
    for(auto attribute: module.parse[attributes]->contents(parse)) {
        if(attribute.name != module.context.addUnqualifiedName("bits", 4)) continue;

        auto args = attribute.args;
        if(args.size() != 1) {
            module.context.diagnostics.error("@bits takes one argument: the width in bits"_v, source);
            return false;
        }

        // The literal's own kind is encoded in the expression kind - see ast::Expr::Lit - so an
        // integer literal is exactly `Lit + Literal::Int`.
        auto argument = args.get(parse, 0).value;
        if(argument.kind != ast::Expr::Kind(ast::Expr::Lit + ast::Literal::Int)) {
            module.context.diagnostics.error("@bits takes a literal width"_v, attribute.source);
            return false;
        }

        // Reported separately from "there is no attribute" so that `@bits(0)` reaches the range
        // check rather than being read as an absent refinement.
        bits = U32(argument.lit.i());
        return true;
    }

    return false;
}

/*
 * `@box` on a field, which is a statement about the field's storage rather than about its type.
 *
 * It is read here, next to `@bits`, because the two are written in the same position and are the
 * same shape of thing - a declaration-site annotation that changes a field's physical
 * representation and that generic code sees straight through. What they differ in is the axis:
 * `@bits` narrows the width and produces a distinct type, `@box` moves the storage out of line and
 * produces a distinct *field*. So this one never reaches `resolveType`, and `cfg.cold` keeps
 * whatever type was written after the attribute.
 *
 * Rejecting the pair is not tidiness. A `@bits` field lives inside a word shared with its
 * neighbours and has no address of its own; a boxed one *is* an address. There is no representation
 * that is both, so a program asking for both is asking for something that does not exist.
 */
static bool hasAttribute(Module& module, ast::ParsePtr<ast::AttrList> attributes, const char* name,
                         U32 length) {
    if(!attributes) return false;

    auto parse = module.parse;
    auto wanted = module.context.addUnqualifiedName(name, length);

    for(auto attribute: parse[attributes]->contents(parse)) {
        if(attribute.name == wanted) return true;
    }

    return false;
}

static bool readBoxAttribute(Module& module, const ast::Type& type) {
    auto attributes = type.attributes;
    if(!attributes) return false;

    auto parse = module.parse;
    auto box = module.context.addUnqualifiedName("box", 3);
    auto bits = module.context.addUnqualifiedName("bits", 4);
    auto boxed = false;
    auto narrowed = false;

    for(auto attribute: parse[attributes]->contents(parse)) {
        if(attribute.name == bits) narrowed = true;
        if(attribute.name != box) continue;

        if(attribute.args.size()) {
            module.context.diagnostics.error("`@box` takes no arguments"_v, attribute.source);
            continue;
        }

        boxed = true;
    }

    if(boxed && narrowed) {
        module.context.diagnostics.error("`@box` and `@bits` cannot both apply to one field - a narrowed field shares a word with its neighbours and has no address of its own, and a boxed field is one"_v,
                                         type.source);
        return false;
    }

    return boxed;
}

TypePtr resolveType(Module& module, const ast::Type& type, GenEnv* env) {
    /*
     * Anything reaching here still carrying `@box` is one written somewhere a field is not, since
     * resolveTupleAst strips the attribute off the fields it consumed.
     *
     * Reported rather than ignored because the two readings of `let x: @box T` are far apart and
     * neither is this: it is either a boxed *local*, which is a real feature nothing implements yet,
     * or a boxed type, which is precisely what an edge annotation exists not to be. Silently
     * dropping it would compile a program to something other than what it says.
     */
    if(hasAttribute(module, type.attributes, "box", 3)) {
        module.context.diagnostics.error("`@box` can only be written on a field of a record or tuple - it is a statement about where a field's storage lives, not a type"_v,
                                         type.source);

        auto plain = type;
        plain.attributes = nullptr;
        return resolveType(module, plain, env);
    }

    U32 bits = 0;
    if(readBitsAttribute(module, type.attributes, type.source, bits)) {
        // Resolved without the attribute first, so that `@bits(4) UInt` narrows whatever `UInt`
        // turned out to be rather than needing a case per way of spelling an integer.
        auto plain = type;
        plain.attributes = nullptr;
        return resolveBitsType(module, resolveType(module, plain, env), bits, type.source);
    }

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
        case ast::Type::Borrow: {
            auto to = resolveType(module, *module.parse[type.to], env);

            /*
             * `&[T]` is a slice - Implementation-Containers.md §4.2.
             *
             * A field has only a type, so this is the spelling a *stored* borrow of a container has,
             * and what is stored is the descriptor rather than an address of the owner. It is the
             * shape zero-copy parsing is written in - Design-Memory §5.3's
             * `data Parser {input: &String, pos: Int}` with an array in it - and it is tracked by
             * ordinary last-use liveness with no lifetime parameter on the record.
             *
             * There is deliberately no spelling for a stored *mutable* slice: a field has no
             * return-root group to confer exclusivity, which is the existing borrow model's rule
             * rather than anything about arrays.
             */
            if(auto slice = sliceOf(module, to)) return slice;

            // Immutable until the signature it belongs to says otherwise: what makes a returned
            // borrow exclusive is the return-root group being entirely `return &`, which is not
            // known until every argument of the declaration has been read.
            return resolveBorrowType(module, to, false);
        }
        case ast::Type::Fun:
            return resolveFunTypeAst(module, *module.parse[type.fun], env, type.source);
        default:
            return errorType(module, type.source, "type is not available in this milestone"_v);
    }
}

TypePtr canonicalType(GlobalBase base, TypePtr type) {
    if(!type || base[type]->kind != Type::Int) return type;

    auto canonical = ((IntType*)base[type])->canonical;
    return canonical ? canonical : type;
}

/*
 * `@bits(n) T` - Design.md's bit-width refinements.
 *
 * The refinement narrows what the value can *hold* and leaves what a load *produces* alone, which is
 * the split IntType was already built around: `width` is recomputed from `n` by the ordinary rule,
 * so a `@bits(13) UInt` still arrives in a 32-bit register and still does 32-bit arithmetic. Only
 * `bits` moves, and only layout reads it.
 *
 * Refining an already-refined type re-refines the original rather than nesting, so
 * `@bits(4) @bits(8) UInt` is `@bits(4) UInt` and there is one canonical form per width.
 */
TypePtr resolveBitsType(Module& module, TypePtr base_, U32 bits, LocationId source) {
    auto base = *module.types;
    if(!base_ || base[base_]->kind == Type::Error) return base_;

    if(base[base_]->kind != Type::Int) {
        module.context.diagnostics.error(
            "@bits can only refine an integer type, and %@ is not one"_v, source,
            describeType(module.context, base, base_));
        return base_;
    }

    auto original = (IntType*)base[canonicalType(base, base_)];

    // The upper bound is the unrefined type's own width rather than the target's, which is the
    // stricter and more useful of the two: `@bits(40) U32` is a mistake on every target, and saying
    // so here means the diagnostic names the type the programmer wrote.
    if(bits == 0 || bits > original->bits) {
        module.context.diagnostics.error(
            "@bits(%@) is out of range for %@, which holds %@ bits"_v, source, bits,
            describeType(module.context, base, (Type*)original - base), U32(original->bits));
        return (Type*)original - base;
    }

    // A refinement to the full width is the type itself. Worth collapsing rather than interning,
    // so that `@bits(32) Int` and `Int` are one type and not two that behave identically.
    if(bits == original->bits) return (Type*)original - base;

    auto canonical = (Type*)original - base;

    for(auto refined: module.program.refinedIntTypes.contents(base)) {
        auto& candidate = *(IntType*)base[refined];
        if(candidate.canonical == canonical && candidate.bits == bits) return (Type*)base[refined] - base;
    }

    // `width` is recomputed from the narrowed size by the same rule the primitives use, so the
    // refinement picks the smallest natural class that can hold it rather than inheriting a wider
    // one from the type it refines. Literally the same rule - see IntType::widthFor.
    auto type = new (module.types) IntType(U16(bits), IntType::widthFor(U16(bits)),
                                           original->isSigned, original->name, canonical);

    module.program.refinedIntTypes.push(module.types, type - base);
    return (Type*)type - base;
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
               existing.returnRoot != args[i].returnRoot || existing.lazy != args[i].lazy) {
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

TypePtr resolveThunkType(Module& module, TypePtr result) {
    return resolveFunType(module, {}, result, ast::FunKind::Plain);
}

/*
 * What may carry the `@lazy` marker.
 *
 * Both rules follow from the argument not being evaluated. A `&` or `->` parameter is a statement
 * about storage the caller already has - one to write through, one to hand over - and there is no
 * such storage until the expression runs, which the callee may decline to do. `return` says a
 * borrow in the result may be rooted in the argument, and an argument that may never exist cannot
 * root anything.
 */
bool checkLazyArgument(Module& module, ast::BindType convention, bool returnRoot, LocationId source) {
    if(convention != ast::BindType::Borrow) {
        module.context.diagnostics.error("`@lazy` cannot be combined with `&` or `->` - the argument is an expression the callee may never run, so there is no caller storage to borrow or to consume"_v,
                                         source);
        return false;
    }

    if(returnRoot) {
        module.context.diagnostics.error("`@lazy` cannot be combined with `return` - an argument that may never be evaluated cannot be what a borrow in the result is rooted in"_v,
                                         source);
        return false;
    }

    return true;
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

    // The one rule directness decides - see arrivesAsCopy, which carries the whole of why. `return %a`
    // is how Native's `borrow` says that its result points into whatever it was given, which is the
    // one bridge from unchecked memory back into checked borrows, and is why a raw pointer is exempt.
    if(convention != ast::BindType::Ref && arrivesAsCopy(base, type)) {
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

TupType* resolveTupleType(Module& module, Buffer<Field> fields, LocationId source, TypeLayout layout) {
    auto base = *module.types;

    /*
     * Normalized before anything is compared against it, because a box around nothing is nothing.
     *
     * A unit field occupies no storage, so there is nothing to allocate, nothing to point at and no
     * address for anything to be stable at - and a write to one is elided, which would leave the
     * pointer this field was going to hold whatever the frame contained while the derived teardown
     * handed it to `freeHeap`. `@box ()` asks for indirect storage for a value that has none, and
     * generic code substituting `a := ()` asks for it without meaning to.
     *
     * It happens *here* rather than at the creation below so that the flag never enters the interned
     * identity: `{@box ()}` and `{()}` have to be one type, or `sameType` stops being pointer
     * equality for two spellings of the same thing.
     */
    SmallArray<Field, 8> normalized;
    for(auto field: fields) {
        if(field.boxed && isUnit(base, field.type)) field.boxed = false;
        normalized.push(field);
    }

    auto requested = toBuffer(normalized);

    for(auto tuplePointer: module.program.tupleTypes.contents(base)) {
        auto tuple = base[tuplePointer];
        if(tuple->fields.size() != requested.length || tuple->layout != layout) continue;

        auto equal = true;
        for(Size i = 0; i < requested.length; i++) {
            auto existing = tuple->fields.get(base, i);

            // `boxed` is part of the identity, not a decoration on it: `{@box Tree}` and `{Tree}`
            // have different layouts, different ownership classes and different access paths, and
            // the Repr cache is keyed on the type alone. See Field.
            if(existing.type != requested[i].type || existing.name != requested[i].name ||
               existing.boxed != requested[i].boxed) {
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
    tuple->layout = layout;
    module.program.tupleTypes.push(module.types, tuple - base);

    /*
     * No cycle check here, deliberately.
     *
     * Interning happens while the declarations that would close a cycle are still being defined, so
     * the answer at this point depends on how far through the module the walk has got: `data A {b:
     * B}` / `data B {a: A}` sees nothing while B's content is null, and a third declaration naming
     * the same interned tuple later sees the whole loop. Both are the same program.
     *
     * The question is asked once, against the declaration, after every content type in the module is
     * resolved - see breakLayoutCycles and its callers - which is also the earliest point at which
     * an indirection could be *inserted* rather than only reported.
     */
    (void)source;
    return tuple;
}

/*
 * Automatic indirection - Design.md's "Representation and layout" and doc/spec/repr.md.
 *
 * A type whose layout is cyclic through *inline containment* cannot be laid out at all, so the
 * compiler breaks the cycle with an indirection and nothing in the source names it. This is where
 * the edge is chosen, and it is in resolve rather than in a backend for the reason the whole file
 * is: which edge gets the pointer is a fact about the program, true of every target at once, and two
 * code generators picking it separately could pick differently.
 *
 * ## What is not an edge
 *
 * A field whose type is a *handle* has a size independent of its target, so it breaks the cycle
 * before this ever sees it: a pointer, a borrow and a function value are the three the walk stops
 * at, and an already-boxed field or constructor is a fourth - `@box` written by the programmer is
 * the manual override on the cut below, and the compiler's own box is what a previous walk left.
 *
 * ## Where the cut lands
 *
 * At the back edge - the type reference naming a declaration that is already on the layout stack -
 * *at whatever depth it appears inside generic arguments*. For
 *
 *     data Tree(a) = Branch {left: Maybe(Tree(a)), right: Maybe(Tree(a))} | Leaf(a)
 *
 * the reference to `Tree` inside `Tree`'s own body is that edge, and it is `Maybe(Tree)`'s `Just`
 * payload rather than `Branch.left`. Cutting there is what produces a one-word child - the box is a
 * non-null pointer, so `Nothing` folds into its null niche by the ordinary niche search - where
 * cutting at `Branch.left` would make an *absent* child cost a heap-allocated `Maybe`.
 *
 * That is why there are two flags rather than one. `Field::boxed` is the edge inside a tuple, which
 * is what `data List(a) = Nil | Cons {head: a, tail: List(a)}` needs; `Constructor::boxed` is the
 * edge that *is* a constructor's whole payload, which is what `Just(a)` is and what a positional
 * `data Loop = L(Loop)` is.
 *
 * ## Why the answer is uniform, and therefore not part of the type
 *
 * `Maybe(Tree)` has infinite inline size, so every `Maybe(Tree)` in the program - local, field,
 * argument, generic instantiation - must be a pointer to a `Tree`. There is no context that could
 * want it otherwise, so there is no variant and nothing for two contexts to disagree about. That is
 * what makes it legal to leave out of the type: type identity exists to keep two things that
 * *differ* from being confused, and these do not differ. `Maybe(Tree)` is one logical type with one
 * Repr, and passing `node.left` to `fn f(m: Maybe(Tree))` needs no coercion.
 *
 * A *tuple* is rewritten rather than mutated, because tuples are interned structurally and `{Tree}`
 * boxed has to be a different type from `{Tree}` unboxed. A *record* is mutated in place, because it
 * is nominal: `Maybe(Tree)` is one instance and the only thing that could observe the change is
 * `Maybe(Tree)` itself.
 */

// One walk's state. `incomplete` is what decides whether the answer may be remembered - see
// RecordType::layoutBroken.
struct LayoutWalk {
    TypeList stack;
    bool incomplete = false;
};

static TypePtr breakCycles(Module& module, TypePtr type, LayoutWalk& walk, LocationId source);

// Whether this type is one the walk is currently inside, which is the whole definition of a back
// edge: reaching it again means laying it out needs its own size.
static bool onLayoutStack(LayoutWalk& walk, TypePtr type) {
    for(auto entry: walk.stack) {
        if(entry == type) return true;
    }

    return false;
}

// A tuple, with whichever of its fields turned out to be back edges boxed. Returns the interned
// tuple to use in place of this one, which is this one where nothing changed.
static TypePtr breakTupleCycles(Module& module, TupType& tuple, LayoutWalk& walk, LocationId source) {
    auto base = *module.types;
    auto self = (Type*)&tuple - base;

    // A tuple cannot be its own back edge - reaching one means a record on the way round, and the
    // record is where the cut belongs - but the guard keeps the recursion bounded regardless.
    if(onLayoutStack(walk, self)) return self;

    walk.stack.push(self);

    Array<Field> fields;
    auto changed = false;

    for(auto field: tuple.fields.contents(base)) {
        auto updated = field;

        if(field.boxed) {
            // Already an indirection, whether the programmer wrote `@box` or a previous walk did.
            // Either way this edge is not part of any cycle.
        } else if(onLayoutStack(walk, field.type)) {
            updated.boxed = true;
            changed = true;
        } else {
            auto rewritten = breakCycles(module, field.type, walk, source);
            if(rewritten != field.type) {
                updated.type = rewritten;
                changed = true;
            }
        }

        fields.push(updated);
    }

    walk.stack.pop();
    if(!changed) return self;

    // The pin travels with the fields: a `@layout(c)` tuple that needed an indirection is still a
    // `@layout(c)` tuple, and a boxed field under that pin is a pointer member, which is exactly how
    // a C struct with one is modelled.
    return (Type*)resolveTupleType(module, toBuffer(fields), source, tuple.layout) - base;
}

static void breakRecordCycles(Module& module, RecordType& record, LayoutWalk& walk, LocationId source) {
    auto base = *module.types;
    auto self = (Type*)&record - base;

    if(record.layoutBroken || onLayoutStack(walk, self)) return;
    if(!record.definitionReady) walk.incomplete = true;

    walk.stack.push(self);

    for(Size i = 0; i < record.constructors.size(); i++) {
        auto constructor = record.constructors.get(base, i);
        if(!constructor.content || constructor.boxed) continue;

        if(onLayoutStack(walk, constructor.content)) {
            // The payload *is* the back edge - `Just(Tree)` inside `Tree`. There is no field to
            // mark, so the constructor carries it.
            constructor.boxed = true;
            record.constructors.set(base, i, constructor);
            continue;
        }

        auto rewritten = breakCycles(module, constructor.content, walk, source);
        if(rewritten == constructor.content) continue;

        constructor.content = rewritten;
        record.constructors.set(base, i, constructor);
    }

    walk.stack.pop();

    // Only where the walk saw the whole graph. A record reached while another declaration was still
    // being defined has not been checked against that declaration, and remembering it would make the
    // cut depend on which module-level phase happened to reach it first.
    if(!walk.incomplete) record.layoutBroken = true;
}

static TypePtr breakCycles(Module& module, TypePtr type, LayoutWalk& walk, LocationId source) {
    if(!type) return type;

    auto base = *module.types;
    auto value = base[type];

    // Reaching a value through one of these costs a load rather than containment, so the layout of
    // what is on the other side cannot make this one infinite.
    if(value->kind == Type::Ptr || value->kind == Type::Borrow || value->kind == Type::Fun) {
        return type;
    }

    if(value->kind == Type::Tup) return breakTupleCycles(module, *(TupType*)value, walk, source);

    if(value->kind == Type::Record) {
        breakRecordCycles(module, *(RecordType*)value, walk, source);
        return type;
    }

    return type;
}

void breakLayoutCycles(Module& module, TypePtr type, LocationId source) {
    // A generic declaration has no layout to be cyclic: `List(a)` is a shape rather than a type, and
    // the indirection belongs to `List(Int)`, which is where the walk will find it.
    if(!type || isGeneric(*module.types, type)) return;

    LayoutWalk walk;
    breakCycles(module, type, walk, source);
}

/*
 * The backstop, and the one layout question that could still be a *source* error.
 *
 * Everything reachable is expected to have been broken by the walk above, so this reports what the
 * walk could not fix rather than what the programmer wrote. It is kept because the alternative to a
 * diagnostic here is an infinite recursion in whichever pass asks for a size next, and because the
 * two walks share the definition of what an edge is - the same three handle kinds, plus a boxed
 * field or constructor, which is a pointer the compiler or the programmer already inserted.
 */
static bool checkAcyclic(Module& module, TypePtr type, TypeList& stack, LocationId source) {
    if(!type) return true;

    auto base = *module.types;
    auto value = base[type];

    // Reaching a value through one of these costs a load rather than containment, so the layout of
    // what is on the other side cannot make this one infinite.
    if(value->kind == Type::Ptr || value->kind == Type::Borrow || value->kind == Type::Fun) {
        return true;
    }

    if(value->kind != Type::Tup && value->kind != Type::Record) return true;

    for(auto entry: stack) {
        if(entry != type) continue;

        module.context.diagnostics.error(
            "%@ contains itself without an indirection, so it has no finite size"_v, source,
            describeType(module.context, base, type));
        return false;
    }

    stack.push(type);
    auto ok = true;

    if(value->kind == Type::Tup) {
        for(auto field: ((TupType*)value)->fields.contents(base)) {
            if(field.boxed) continue;
            ok = checkAcyclic(module, field.type, stack, source) && ok;
        }
    } else {
        for(auto constructor: ((RecordType*)value)->constructors.contents(base)) {
            if(constructor.boxed) continue;
            ok = checkAcyclic(module, constructor.content, stack, source) && ok;
        }
    }

    stack.pop();
    return ok;
}

bool checkTypeAcyclic(Module& module, TypePtr type, LocationId source) {
    if(!type || isGeneric(*module.types, type)) return true;

    TypeList stack;
    return checkAcyclic(module, type, stack, source);
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
/*
 * A boxed member - see Field::boxed.
 *
 * The three answers all change, and each of them is the box rather than what is in it:
 *
 *  - **TrivialCopy is lost.** A bitwise duplicate would copy the pointer and leave two owners of one
 *    allocation. `Copy` is still writable by hand - allocate a new box, copy the target - so what
 *    boxing does is demote TrivialCopy to Copy rather than remove copying, which is the one
 *    non-transparent consequence of `@box` and the reason it is an API change.
 *  - **TrivialSink is preserved, and sometimes gained.** Relocating the owner moves a pointer and
 *    the target keeps its address, so a type that was self-referential - and therefore not
 *    TrivialSink - can become one by boxing the self-referential edge. That is why the inner value's
 *    answer is *not* folded in here.
 *  - **Reclaim is always derived**, because the box itself has to be handed back even where its
 *    target releases nothing. Drop follows the target: a box around something with no effect at last
 *    use has none either.
 */
static void includeBoxedMember(Module& module, TypePtr member, Ownership& target) {
    auto inner = ownershipOf(module, member);

    target.trivialCopy = false;
    target.reclaim = TeardownKind::Derived;

    if(inner.drop != TeardownKind::None && target.drop == TeardownKind::None) {
        target.drop = TeardownKind::Derived;
    }
}

// Folds one member into an aggregate's classification. A member that is not trivial makes the whole
// aggregate not trivial, and a member with either half of a teardown gives the aggregate a derived
// half of the same kind - which is the whole of "recurse into each field, then release this type's
// own storage". The two halves are folded independently, which is the point of splitting them: a
// member with only a `Reclaim` must not give its container a `Drop` and cost it region eligibility.
static void includeMember(Module& module, TypePtr member, Ownership& target, bool boxed = false) {
    if(boxed) return includeBoxedMember(module, member, target);

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
            for(auto field: tuple->fields.contents(base)) {
                includeMember(module, field.type, result, field.boxed);
            }
            break;
        }

        case Type::Record: {
            auto record = (RecordType*)value;

            // An enum-layout record is a discriminant and nothing else, so there are no member
            // types to fold and the scalar answer is already right.
            if(record->layout != RecordType::Enum) {
                for(auto constructor: record->constructors.contents(base)) {
                    if(!constructor.content) continue;
                    includeMember(module, constructor.content, result, constructor.boxed);
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
        if(hasInstance(module, module.coreClasses.reclaim, type)) {
            result.reclaim = TeardownKind::Authored;

            /*
             * A container's teardown is computed from its elements - Implementation-Containers.md
             * §13.
             *
             * An authored `Reclaim` over a *parametric* head is a container's one traversal over its
             * live elements, and the author is trusted about "I call nothing else" - which
             * checkReclaimShape verifies - and never about "my members are effect-free", which is
             * this. Whether that traversal has effects is decided by whether the type arguments have
             * a `Drop`, so `Array(Int)`'s is a reclaim and nothing more while `Array(Buffer)`'s is
             * also a drop, and the two differ in region eligibility rather than in code.
             *
             * Not derivable structurally, and that is the whole reason this rule exists: the run's
             * members are a raw pointer and two counts, so a fold over them says a container of
             * connections has no teardown. Which slots hold values is private to the container, and
             * the only thing the compiler can see about them is the type they are of.
             */
            if(value->kind == Type::Record) {
                for(auto arg: ((RecordType*)value)->instanceArgs.contents(base)) {
                    if(ownershipOf(module, arg).drop != TeardownKind::None) {
                        result.drop = TeardownKind::Authored;
                    }
                }
            }
        }

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

    // And TrivialCopy implies TrivialSink, because a duplicate is strictly more than a relocation:
    // a type whose bytes cannot even be *moved* without a call - it refers to its own address -
    // certainly cannot have those bytes duplicated into a second live value. Saying so here is what
    // makes an authored `Sink` reachable at all, since `->` copies rather than moves a TrivialCopy
    // source and a type left in both classes would never take the move path its instance is for.
    if(!result.trivialSink) result.trivialCopy = false;

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

                // The boxed rules, stated the same way includeBoxedMember states them: the pointer
                // is what is copied, moved and released, so TrivialCopy goes, TrivialSink stays, the
                // reclaim is the box's own, and only the drop follows what is inside it.
                if(field.boxed) {
                    result.trivialCopy = false;
                    result.reclaim = TeardownKind::Derived;
                    if(inner.drop != TeardownKind::None) result.drop = TeardownKind::Derived;
                    continue;
                }

                result.trivialCopy = result.trivialCopy && inner.trivialCopy;
                result.trivialSink = result.trivialSink && inner.trivialSink;
                if(inner.reclaim != TeardownKind::None) result.reclaim = TeardownKind::Derived;
                if(inner.drop != TeardownKind::None) result.drop = TeardownKind::Derived;
            }

            if(result.needsTeardown() || !result.trivialSink) result.trivialCopy = false;
            return result;
        }

        case Type::Record: {
            auto record = (RecordType*)value;
            Ownership result;

            if(record->layout != RecordType::Enum) {
                for(auto constructor: record->constructors.contents(base)) {
                    if(!constructor.content) continue;

                    auto inner = ownershipInAt(module, env, constructor.content, depth - 1);

                    if(constructor.boxed) {
                        result.trivialCopy = false;
                        result.reclaim = TeardownKind::Derived;
                        if(inner.drop != TeardownKind::None) result.drop = TeardownKind::Derived;
                        continue;
                    }

                    result.trivialCopy = result.trivialCopy && inner.trivialCopy;
                    result.trivialSink = result.trivialSink && inner.trivialSink;
                    if(inner.reclaim != TeardownKind::None) result.reclaim = TeardownKind::Derived;
                    if(inner.drop != TeardownKind::None) result.drop = TeardownKind::Derived;
                }
            }

            // A generic instantiation cannot be asked for an authored instance - `Maybe(a)` is not
            // a type anything writes one for - so the structural answer is the whole answer here,
            // exactly as it is in ownershipOf.
            if(result.needsTeardown() || !result.trivialSink) result.trivialCopy = false;
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

bool arrivesAsCopy(GlobalBase base, TypePtr type) {
    return isDirectType(base, type) && !isPointer(base, type);
}

U32 naturalStorageBits(U32 bits) {
    if(bits <= 8) return 8;
    if(bits <= 16) return 16;
    if(bits <= 32) return 32;
    return 64;
}

static U32 alignBitsTo(U32 value, U32 unit) {
    return unit ? (value + unit - 1) & ~(unit - 1) : value;
}

/*
 * The recursion, with a depth limit.
 *
 * An aggregate's width is its fields' widths, so this walks the type graph, and a type that reaches
 * itself by inline containment would walk it forever. Resolve reports such a type against its
 * declaration (see checkAcyclic) but resolution continues afterwards so that the rest of the module
 * still produces diagnostics, so this can be asked about one. Answering "not narrow" past a depth no
 * real declaration reaches is what keeps that a reported error rather than a hang - the same reason
 * ReprTable::of carries an in-progress set.
 */
static ValueWidth valueWidthAt(GlobalBase base, TypePtr type, U32 depth);

/*
 * Whether a value of this type may be co-packed at all, before asking whether it is narrow enough.
 *
 * A type whose lifetime or whose copy is a *call* may not: every one of those calls takes the address
 * of the value, and a field that shares a word has no address of its own to give. Handing over the
 * word's would call the operation on the neighbour's bits - and for a record of two such fields, twice
 * on the same ones. So an authored `Copy`, `Sink`, `Reclaim` or `Drop` anywhere inside a type keeps it
 * out of a shared word, and costs it the byte it would have saved.
 *
 * `ownershipReady` is what makes this askable here at all: ownership is a whole-program property
 * cached on the type, and by the time a target lays anything out every reachable type has one. A
 * caller during resolution may see one that does not yet, and gets the permissive answer - which is
 * the safe direction, since a target may always pack fewer fields than it was offered.
 */
static bool packableValue(GlobalBase base, TypePtr type) {
    if(!type) return false;

    auto value = base[type];
    if(!value->ownershipReady) return true;

    auto& ownership = value->ownership;
    return ownership.trivialCopy && ownership.trivialSink && !ownership.needsTeardown();
}


// The bit offsets of every field of an aggregate that has a scalar form, or nothing where it has
// none. Shared by `valueWidth`, which needs only the span, and by repr, which needs the offsets.
static bool scalarBits(GlobalBase base, TupType& tuple, U32 depth, PackedRun& run, Array<U32>* offsets) {
    // A pinned layout has no scalar form. Its whole purpose is that its fields sit where a C
    // compiler put them, and a scalar is the compiler choosing.
    if(tuple.layout != TypeLayout::Auto) return false;

    auto count = tuple.fields.size();
    if(!count) return false;

    // Every field has to be narrow, because a scalar aggregate *is* a run of co-packed fields and a
    // field that fills its own storage is not one. That is what keeps `{a: U8, b: U8}` two bytes
    // rather than two bit-fields of a word, and it is why `data Small = A(U8) | B(U8)` - a tag bit
    // over a full-width payload - is a separate feature rather than this one.
    Array<U16> order;
    for(U16 i = 0; i < count; i++) {
        auto field = tuple.fields.get(base, i);

        // A boxed field is a whole pointer, which is not narrow on any target this compiler emits
        // for - so an aggregate holding one has no scalar form. Answering here rather than through
        // `valueWidthAt` is also what keeps this walk finite over a recursive type: what is on the
        // other side of a box has no bearing on the width of the thing holding it.
        if(field.boxed) return false;
        if(!packableValue(base, field.type)) return false;
        if(!valueWidthAt(base, field.type, depth + 1).isNarrow()) return false;

        order.push(i);
    }

    packOrder(base, tuple, order);

    Array<U32> placed;
    run = packBits(base, tuple, toBuffer(order), kMaxPackBits, offsets ? &placed : nullptr);

    // A run too long for one word is not a scalar, and one that exactly fills its storage is not a
    // *narrow* one - it has no bits left to be packed into a neighbour with, so calling it narrow
    // would only cost every borrow of it a shift it does not need.
    if(run.count != count || run.span >= naturalStorageBits(run.span)) return false;

    // Reported by field index rather than in placement order, so that a caller laying the fields out
    // needs nothing from the ordering above. Two places deciding the same permutation is two places
    // that can disagree about where a field went.
    if(offsets) {
        for(Size i = 0; i < count; i++) offsets->push(0);
        for(Size at = 0; at < placed.size(); at++) (*offsets)[order[at]] = placed[at];
    }

    return true;
}

static ValueWidth valueWidthAt(GlobalBase base, TypePtr type, U32 depth) {
    if(!type || depth > 8) return {};

    auto value = base[type];
    switch(value->kind) {
        case Type::Int: {
            auto bits = U32(((IntType*)value)->bits);
            return ValueWidth { bits, naturalStorageBits(bits) };
        }
        case Type::Tup: {
            PackedRun run;
            auto tuple = (TupType*)value;
            if(!scalarBits(base, *tuple, depth, run, nullptr)) return {};

            return ValueWidth { run.span, naturalStorageBits(run.span) };
        }
        case Type::Record: {
            auto record = (RecordType*)value;

            // A payload-free sum *is* its discriminant, so what it needs is the bits its constructor
            // count needs against the storage those bits are held in - one byte for a `Bool`, the
            // same rule an integer of that width answers by, and the same one Repr sizes an enum at.
            if(record->layout == RecordType::Enum) {
                auto count = record->constructors.size();
                U32 bits = 1;
                while((Size(1) << bits) < count) bits++;

                return ValueWidth { bits, naturalStorageBits(bits) };
            }

            /*
             * A single-constructor record is its content, which is what makes a record of two
             * `Bool`s a two-bit value and `Maybe(Flags)` one byte.
             *
             * A record with several *payload-carrying* constructors is not: its discriminant would
             * have to become a bit range of the same word and its constructors would have to overlap
             * inside it, which is a second feature and not this one. A payload-free sum took the
             * branch above, so what falls through here is the case with something to overlap.
             */
            if(record->layout != RecordType::Single) return {};

            auto constructors = record->constructors.contents(base);
            if(!constructors.size()) return {};

            // A boxed payload makes the newtype a pointer, which is not narrow - and asking about
            // its target would walk a recursive declaration forever.
            if(constructors[0].boxed) return {};

            return valueWidthAt(base, constructors[0].content, depth + 1);
        }
        default:
            return {};
    }
}

ValueWidth valueWidth(GlobalBase base, TypePtr type) {
    return valueWidthAt(base, type, 0);
}

/*
 * Whether a field of a pinned aggregate is a bit-field at all.
 *
 * Under `C` only a written refinement is one - `@bits(4) Int` is `int x: 4`, and a `Bool` is a whole
 * `_Bool` rather than one bit of a byte. That distinction does not exist under `Auto`, where every
 * narrow value is a candidate and a `Bool` costing a bit rather than a byte is the point; it exists
 * here because a C header has both spellings and they lay out differently.
 */
static bool isBitField(GlobalBase base, TypePtr type) {
    return type && base[type]->kind == Type::Int && ((IntType*)base[type])->canonical != nullptr;
}

U32 declaredUnitBits(GlobalBase base, TypePtr type) {
    if(!isBitField(base, type)) return 0;

    auto canonical = canonicalType(base, type);
    if(base[canonical]->kind != Type::Int) return 0;

    return naturalStorageBits(U32(((IntType*)base[canonical])->bits));
}

// The unit a bit-field is allocated in. Its own natural storage, or - under a pinned layout - the
// storage of the type it was written as a refinement of, which is the unit C uses.
static U32 packUnitBits(GlobalBase base, TupType& tuple, TypePtr type, U32 bits) {
    if(tuple.layout != TypeLayout::C) return naturalStorageBits(bits);

    auto declared = declaredUnitBits(base, type);
    return declared ? declared : naturalStorageBits(bits);
}

PackedRun packBits(GlobalBase base, TupType& tuple, Buffer<const U16> order, U32 maxBits,
                   Array<U32>* offsets) {
    PackedRun run;

    for(auto index: order) {
        auto type = tuple.fields.get(base, index).type;
        auto width = valueWidth(base, type);
        if(!width.logical) break;

        auto unit = packUnitBits(base, tuple, type, width.logical);
        auto at = run.span;

        // Bumped to the next unit boundary where it would otherwise cross one. Measured before the
        // budget check, since the bump is what decides whether the field still fits.
        if(at / unit != (at + width.logical - 1) / unit) at = alignBitsTo(at, unit);
        if(at + width.logical > maxBits) break;

        if(offsets) offsets->push(at);
        run.span = at + width.logical;
        run.count++;
    }

    return run;
}

void packOrder(GlobalBase base, TupType& tuple, Array<U16>& into) {
    if(tuple.layout != TypeLayout::Auto) return;

    // Insertion sort, descending by width and stable within one - the lists are a handful of fields
    // long, and stability is what makes the layout of two same-width fields the declaration's
    // business rather than the sort's.
    for(Size i = 1; i < into.size(); i++) {
        auto index = into[i];
        auto width = valueWidth(base, tuple.fields.get(base, index).type).logical;
        auto at = i;

        while(at > 0 && valueWidth(base, tuple.fields.get(base, into[at - 1]).type).logical < width) {
            into[at] = into[at - 1];
            at--;
        }

        into[at] = index;
    }
}

bool packCandidate(GlobalBase base, TupType& tuple, U16 index) {
    auto count = tuple.fields.size();
    if(index >= count) return false;

    auto narrowAt = [&](Size at) {
        auto field = tuple.fields.get(base, at);

        // A boxed field is a pointer with an address of its own, which is most of what boxing one is
        // for. Co-packing it would take that address away, and there is nothing narrow about it to
        // pack in the first place.
        if(field.boxed) return false;
        return packableValue(base, field.type) && valueWidth(base, field.type).isNarrow();
    };

    if(!narrowAt(index)) return false;

    /*
     * A pinned layout keeps the declaration's order, so a bit-field's neighbours are the ones it was
     * written next to - `{a: @bits(4), b: U64, c: @bits(4)}` allocates two units, as C does - and only
     * a written refinement shares a unit with anything at all.
     */
    if(tuple.layout == TypeLayout::C) {
        auto fieldAt = [&](Size at) {
            auto type = tuple.fields.get(base, at).type;
            return isBitField(base, type) && valueWidth(base, type).isNarrow();
        };

        if(!fieldAt(index)) return false;
        return (index > 0 && fieldAt(index - 1)) || (Size(index) + 1 < count && fieldAt(index + 1));
    }

    // A `@layout(js)` record keeps one property per field, which is the whole content of the pin, so
    // nothing in it shares with anything. `placementOrder` already declines to group a pinned tuple,
    // but this is the answer the *borrow* tier asks for - see expr_construct's use - and a field that
    // is not packed must not be borrowed as though it were.
    if(tuple.layout == TypeLayout::Js) return false;

    // An auto layout reorders, so anything else narrow in the tuple is a neighbour.
    for(Size at = 0; at < count; at++) {
        if(at != index && narrowAt(at)) return true;
    }

    return false;
}

// The scalar form of an aggregate, for whoever is laying it out. The span and the offsets come from
// the same placement `valueWidth` reported, which is what makes the mask a callee applies to a
// reference to the whole aggregate the same width the fields were placed within.
bool scalarLayout(GlobalBase base, TupType& tuple, PackedRun& run, Array<U32>* offsets) {
    return scalarBits(base, tuple, 0, run, offsets);
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
            // A refinement says so, and then says what it refines. Without this a diagnostic about
            // `Id` and `U64` reads "cannot convert U64 to U64", which names the problem twice and
            // identifies it not at all.
            auto integer = (IntType*)base[type];
            if(integer->canonical) {
                // appendValue rather than `<<`, which takes a character and would quietly append
                // the one with that code - `@bits(53)` came out as `@bits(5)`.
                target << "@bits(";
                target.appendValue(integer->bits);
                target << ") ";
                describeType(context, base, integer->canonical, target);
                return;
            }

            // Core's Int and Native's I32 have the same shape and different names, so an integer
            // type says which one it is rather than describing its width.
            auto name = integer->name;
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

                // Printed even where nothing in the source wrote it, because an automatic
                // indirection is the difference between a type that has a layout and one that does
                // not, and a diagnostic naming the two the same way would be unreadable.
                if(field.boxed) target << "@box ";
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
                if(arg.lazy) target << "@lazy ";
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

StringId builtName(Context& context, StringBuilder& text) {
    return context.addQualifiedName(text.pointer(), text.size(), 1);
}

StringId derivedName(Module& module, StringView prefix, TypePtr type) {
    StringBuilder text;
    text << prefix;
    describeType(module.context, *module.types, type, text);
    return builtName(module.context, text);
}

U64 floatBits(GlobalBase base, TypePtr type, F64 value) {
    U64 bits = 0;

    if(isFloat(base, type) && ((FloatType*)base[type])->width == FloatType::Float) {
        auto single = F32(value);
        copy((const Byte*)&single, (Byte*)&bits, sizeof(single));
    } else {
        copy((const Byte*)&value, (Byte*)&bits, sizeof(value));
    }

    return bits;
}

F64 floatFromBits(GlobalBase base, TypePtr type, U64 bits) {
    if(isFloat(base, type) && ((FloatType*)base[type])->width == FloatType::Float) {
        F32 single;
        copy((const Byte*)&bits, (Byte*)&single, sizeof(single));
        return F64(single);
    }

    F64 number;
    copy((const Byte*)&bits, (Byte*)&number, sizeof(number));
    return number;
}
