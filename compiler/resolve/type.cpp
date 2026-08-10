/*
 * The type universe: how a type comes into existence and when two of them are the same one.
 *
 * Every constructed type is interned on what it is made of, which is what keeps `sameType` pointer
 * equality - the tuple written in a signature and the one an expression builds are one TypePtr, and
 * so are `Maybe(Int)` written in two modules. Instantiation, substitution and structural matching
 * are here for the same reason: each of them produces a type, and each has to produce the *same*
 * type the other two would have.
 */

#include "type_internal.h"
#include "generic.h"
#include "module.h"
#include "name.h"
#include "index.h"

TypePtr errorType(Module& module, LocationId source, StringView message) {
    module.context.diagnostics.error(message, source);
    return module.scalar.error;
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

        // A Repr refinement is a second instantiation at the same arguments (RecordType::canonical),
        // so the plain one is the one without a refinement rather than the first one found.
        if(instance->isRefined()) continue;

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

/*
 * `@inline(i) @capacity(c) [T]` - Implementation-Containers.md §7.
 *
 * A second instantiation of the same declaration at the same arguments, differing only in what its
 * Repr does with them. Interned beside the plain one on the declaration's own instance list, which
 * is what makes two mentions of `@inline(4) @capacity(4) [Int]` in two modules one type - the same
 * property `Array(Int)` has and for the same reason, since a layout that is part of a record field's
 * ABI has to be a function of what was written and nothing else.
 *
 * The constructors are copied from the plain instantiation rather than substituted again, because
 * they are the same constructors: a refinement changes no field's type. That is also what makes the
 * *ownership* answer the same one - `ownershipOf` walks the members - so a refined array of handles
 * has the same `Drop` the plain one does, which is §9's rule stated as an implementation fact.
 */
TypePtr refineContainerType(Module& module, TypePtr plain, U32 inlineSlots, U32 capacityBound,
                            LocationId source) {
    auto global = *module.types;
    if(!plain || global[plain]->kind != Type::Record) return plain;

    auto record = (RecordType*)global[plain];
    auto pointer = record->instanceOf;
    if(!pointer) return plain;

    auto declaration = (RecordType*)global[pointer];

    /*
     * The plain instantiation's contents are what this one is built out of, so a refinement written
     * before they exist has nothing to refine. That is only reachable from inside the module that
     * declares the array, where a refinement would be circular anyway.
     */
    if(!record->definitionReady) {
        module.context.diagnostics.error("`@inline`/`@capacity` cannot be written here - the container's own declaration is not complete yet"_v,
                                         source);
        return plain;
    }

    for(auto existing: declaration->instances.contents(global)) {
        auto instance = (RecordType*)global[existing];
        if(instance->canonical != record - global) continue;
        if(instance->inlineSlots != inlineSlots || instance->capacityBound != capacityBound) continue;

        return (Type*)instance - global;
    }

    auto instance = new (module.types) RecordType(declaration->name);
    instance->instanceOf = pointer;
    instance->qualified = declaration->qualified;
    instance->generic = record->generic;
    instance->canonical = record - global;
    instance->inlineSlots = inlineSlots;
    instance->capacityBound = capacityBound;

    for(auto arg: record->instanceArgs.contents(global)) instance->instanceArgs.push(module.types, arg);

    /*
     * The constructors are the plain one's, with the content tuple re-interned carrying the
     * refinement - see TupType::inlineSlots for why it has to be on the tuple as well.
     *
     * The *fields* are identical, which is what keeps every other answer about this type identical:
     * `ownershipOf` walks members, `matchType` compares `instanceOf` and `instanceArgs`, and neither
     * can tell the two tuples apart. What differs is the one thing a Repr refinement is allowed to
     * differ in, which is where those fields sit.
     */
    for(Size i = 0; i < record->constructors.size(); i++) {
        auto constructor = record->constructors.get(global, i);

        if(constructor.content && global[constructor.content]->kind == Type::Tup) {
            auto content = (TupType*)global[constructor.content];
            SmallArray<Field, 4> fields;
            for(auto field: content->fields.contents(global)) fields.push(field);

            auto refinedContent = resolveTupleType(module, toBuffer(fields), source, content->layout,
                                                   inlineSlots, capacityBound);
            constructor.content = (Type*)refinedContent - global;
        }

        instance->constructors.push(module.types, constructor);
    }

    instance->layout = record->layout;
    instance->pinned = record->pinned;
    instance->definitionReady = record->definitionReady;
    instance->layoutBroken = record->layoutBroken;

    declaration->instances.push(module.types, instance - global);

    // The plain instantiation may still be waiting for its declaration, in which case this one is
    // waiting for the same thing and its copied constructor contents are the nulls it copied.
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
                // of what the field holds, exactly as the pinned layout below is. So does the host
                // elision, which is a statement about the declaration and not about the element.
                fields.push(Field { substituteType(module, field.type, args, source), field.name,
                                    field.boxed, field.host });
            }

            // The layout the tuple was pinned to survives substitution: `@layout(c)` on a generic
            // record is a promise about every instantiation of it, not about the declaration.
            return (Type*)resolveTupleType(module, toBuffer(fields), source, tuple->layout) - global;
        }
        case Type::Ptr:
            return resolvePointerType(module, substituteType(module, ((PtrType*)global[type])->to, args, source));
        case Type::Array: {
            // The length is not substituted because there is nothing it could be substituted *by*:
            // `n` is a literal in every position that mentions it (§6), so a generic `[a *4]` is
            // generic in exactly one place and the four travels through unchanged.
            auto array = (ArrayType*)global[type];
            return resolveFixedArrayType(module, substituteType(module, array->content, args, source),
                                         array->length, source);
        }
        case Type::Borrow: {
            auto borrow = (BorrowType*)global[type];
            return resolveBorrowType(module, substituteType(module, borrow->to, args, source), borrow->mut);
        }
        case Type::Fun: {
            auto function = (FunType*)global[type];
            SmallArray<FunArg, 8> substituted;

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
        case Type::Array: {
            // The length is matched and never bound: there is no kind for it to be a variable of.
            // §6 is explicit that `[T *n]` never appears in an instance head, so what this serves is
            // a *member* signature mentioning one - `fn f(xs: [a *4])` inside an instance - where the
            // element still has to bind and the four still has to agree.
            auto patternArray = (ArrayType*)global[pattern];
            auto concreteArray = (ArrayType*)global[concrete];

            if(patternArray->length != concreteArray->length) return false;
            return matchType(global, patternArray->content, concreteArray->content, bindings);
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

/*
 * `[T *n]` - Implementation-Containers.md §6.
 *
 * Interned on the pair the way a pointer is interned on its target, because the length is part of
 * what the type *is* rather than something written about it: `[Int *3]` and `[Int *4]` differ in
 * size, in teardown and in which literals they accept, and every one of those follows from `n`.
 *
 * A zero-length one is legal and is a value occupying nothing, which is what makes `[] :: [Int *0]`
 * an ordinary literal rather than a case. What is refused is a length that will not fit the
 * arithmetic every element access does - the check is against the count rather than against the
 * byte size, since the latter is a target's answer and this type is not.
 */
TypePtr resolveFixedArrayType(Module& module, TypePtr content, U32 length, LocationId source) {
    auto base = *module.types;
    if(!content || base[content]->kind == Type::Error) return module.scalar.error;

    // The bound is reported where the length was written - see fixedArrayLength - so what is left
    // here is the backstop for a caller that built one without going through the surface syntax.
    if(length > kMaxFixedArrayLength) return module.scalar.error;

    for(auto array: module.program.fixedArrayTypes.contents(base)) {
        if(base[array]->content == content && base[array]->length == length) {
            return (Type*)base[array] - base;
        }
    }

    auto type = new (module.types) ArrayType(content, length);
    type->generic = isGeneric(base, content);

    module.program.fixedArrayTypes.push(module.types, type - base);
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

TupType* resolveTupleType(Module& module, Buffer<Field> fields, LocationId source, TypeLayout layout,
                          U32 inlineSlots, U32 capacityBound) {
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
        if(tuple->inlineSlots != inlineSlots || tuple->capacityBound != capacityBound) continue;

        auto equal = true;
        for(Size i = 0; i < requested.length; i++) {
            auto existing = tuple->fields.get(base, i);

            // `boxed` is part of the identity, not a decoration on it: `{@box Tree}` and `{Tree}`
            // have different layouts, different ownership classes and different access paths, and
            // the Repr cache is keyed on the type alone. `host` is there for the stronger version
            // of the same reason - two values of them do not have the same properties. See Field.
            if(existing.type != requested[i].type || existing.name != requested[i].name ||
               existing.boxed != requested[i].boxed || existing.host != requested[i].host) {
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
    tuple->inlineSlots = inlineSlots;
    tuple->capacityBound = capacityBound;
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
