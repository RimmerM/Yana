/*
 * Conversion: making a value the type a position wants.
 *
 * Two things that are easy to confuse and are separated here. A *literal* has no type until a
 * position gives it one, so settling one is a choice rather than a conversion - `mergeLiterals` and
 * `literalDefault` are where that choice is made, and `materializeLiteral` is where it becomes a
 * value. A conversion is the other: both types are known, and what is asked is whether one may
 * stand for the other, and what has to be emitted for it to.
 *
 * `convertible` is the same question asked without emitting anything, which is what overload
 * resolution needs - see the solver in solve.cpp, which asks it rather than answering it itself.
 */

#include "expr.h"
#include "complete.h"
#include "generic.h"
#include "name.h"
#include "index.h"

/*
 * Literal variables.
 *
 * A literal is a class-polymorphic value: `1` means `FromInt.fromInt(1)` and `1.5` means
 * `FromDecimal.fromDecimal(1.5)`, so which type it has is decided by where it flows. Where a
 * position already says - an argument of a known parameter type, a declared return, an
 * ascription - it is built there and then, which is the common case and costs nothing. Where
 * nothing says, it becomes a literal variable that survives the round trip through overload
 * selection and is settled afterwards, because the type `1` should have in `x + 1` is not known
 * until the call is selected and selecting the call needs the operand types.
 */

TypePtr ExprResolver::literalVariable(GlobalPtr<TypeClass> literalClass) {
    auto type = new (module.types) LiteralType(module.program.literalCounter++);
    type->classes.push(module.types, literalClass);
    return (Type*)type - global;
}

TypePtr ExprResolver::mergeLiterals(TypePtr lhs, TypePtr rhs) {
    auto left = ((LiteralType*)global[lhs])->classes.contents(global);
    auto right = ((LiteralType*)global[rhs])->classes.contents(global);

    auto isNew = [&](GlobalPtr<TypeClass> candidate) { return !left.containsValue(candidate); };

    // Two literals of the same class - `1 + 2` - are already one question, so the left one serves.
    if(!right.contains(isNew)) return lhs;

    auto merged = new (module.types) LiteralType(module.program.literalCounter++);
    for(auto candidate: left) merged->classes.push(module.types, candidate);

    for(auto candidate: right) {
        if(isNew(candidate)) merged->classes.push(module.types, candidate);
    }

    return (Type*)merged - global;
}

TypePtr ExprResolver::literalDefault(TypePtr type) {
    auto classes = ((LiteralType*)global[type])->classes.contents(global);

    // Each class offers its own default, and the one taken is the first that also satisfies every
    // other class the variable collected. `1 + 2.5` is what needs the second half: FromInt's Int
    // has no FromDecimal instance, FromDecimal's Float has a FromInt instance, so Float wins.
    for(auto candidate: classes) {
        auto declared = global[candidate]->defaultType;
        if(!declared) continue;

        auto unmet = classes.contains([&](GlobalPtr<TypeClass> other) {
            return !findInstance(module, other, { &declared, 1 });
        });

        if(!unmet) return declared;
    }

    return nullptr;
}

TypePtr ExprResolver::settleType(TypePtr type) {
    if(!isLiteral(global, type)) return type;
    return literalDefault(type);
}

bool ExprResolver::literalFits(TypePtr literal, TypePtr target) {
    if(!target) return false;
    if(global[target]->kind == Type::Error) return true;

    // A type variable has no instances to look at. What answers for it is a requirement of the
    // enclosing function - declared, like the `FromInt(a)` that `Num(a)` implies through its
    // superclass, or recorded by this call the way an undeclared `Ord(a)` is recorded by a
    // comparison in the body. A generic type built over one - `Maybe(a)` - could be served by a
    // parametric instance, but there is no requirement shaped like `FromInt(Maybe(a))` to record
    // for it, so a literal is not built at one here.
    if(isGeneric(global, target)) {
        return global[target]->kind == Type::Gen && functionGen(global, function) != nullptr;
    }

    auto classes = ((LiteralType*)global[literal])->classes.contents(global);

    return !classes.contains([&](GlobalPtr<TypeClass> candidate) {
        return !findInstance(module, candidate, { &target, 1 });
    });
}

ModulePtr<Value> ExprResolver::materializeLiteral(ModulePtr<Value> value, TypePtr target, LocationId source) {
    // A literal variable can reach a position that has one of its own - `1 + 2`, where neither
    // operand says anything the other did not - and then the default is what both take.
    if(isLiteral(global, target)) target = literalDefault(target);

    // A literal that could not be built is reported once, here, and then carries the error type so
    // that the positions it flows through afterwards - an ascription's own conversion, a return -
    // say nothing more about the same mistake.
    auto failed = [&]() { return constant<ConstInt>(source, module.scalar.error, 0); };

    if(!target) {
        context.diagnostics.error("nothing decides the type of this literal, and its class has no default"_v, source);
        return failed();
    }

    if(global[target]->kind == Type::Error) return failed();

    auto integral = local[value]->kind == Value::ConstInt;

    // A primitive target is the literal itself. Taking it directly rather than through the
    // instance keeps the common path to one constant, and is the same shortcut a literal written
    // where its type is already known takes.
    if(integral) {
        auto written = ((ConstInt*)local[value])->value;

        if(isInteger(global, target)) {
            checkLiteralRange(source, target, written);
            return makeInt(source, target, written);
        }

        if(isFloat(global, target)) return makeFloat(source, target, F64(written));
    } else if(isFloat(global, target)) {
        return makeFloat(source, target, ((ConstDouble*)local[value])->value);
    }

    auto typeClass = integral ? module.coreClasses.fromInt : module.coreClasses.fromDecimal;
    if(!typeClass || global[typeClass]->functions.isEmpty()) return failed();

    // The class function takes the literal at its widest precision, so a `Long`/`Double` constant
    // is what an instance is handed and what its type has to be able to represent.
    ResolvedArg args[] = {
        integral ? makeInt(source, module.scalar.long_, ((ConstInt*)local[value])->value)
                 : makeFloat(source, module.scalar.double_, ((ConstDouble*)local[value])->value),
    };

    // Selected against the class directly rather than by the name it happens to have: which
    // function builds a literal is not something a module that defines its own `fromInt` gets to
    // answer, and R5 would otherwise let a plain function of that name take over every literal in
    // the module that wrote it.
    ClassFunRef reference { typeClass, global[typeClass]->functions.get(global, 0).name, 0 };
    ClassMatch match;

    if(matchClassFun(reference, { args, 1 }, {}, target, match)) {
        if(match.instance) {
            if(local[match.instance]->functions.get(local, match.index)) {
                return emitInstanceCall(module, match.instance, toBuffer(match.instanceArgs), match.index,
                                        { args, 1 }, source);
            }
        } else if(isGeneric(global, target)) {
            // Inside a generic body the instance is the caller's to supply, exactly as it is for
            // any other class call the body's own type variables decide.
            return emitGenericDispatch(match, { args, 1 }, source, StringId());
        }
    }

    context.diagnostics.error("no instance of %@ for %@ - this literal cannot be written as that type"_v, source,
                              context.findName(global[typeClass]->name), describeType(context, global, target));
    return failed();
}

/*
 * What a condition means.
 *
 * `if x` is `Truth(typeof x).truthy(x)`, consulted for x's own type and never through a
 * conversion. That one rule is what separates this from JavaScript's truthiness: the criticized
 * part there is not that values have a truth value, it is that implicit coercion decides which
 * one, so the same expression means different things in different contexts. Here the instance is
 * selected for the type the condition already has - no Widen step is tried first - so `if x`
 * depends on nothing but x's type, and coherence gives that type exactly one answer.
 */
ModulePtr<Value> ExprResolver::truthy(ModulePtr<Value> value, LocationId source) {
    if(!value) return nullptr;

    auto type = valueType(value);
    if(global[type]->kind == Type::Error) return nullptr;

    auto typeClass = module.coreClasses.truth;
    if(!typeClass || global[typeClass]->functions.isEmpty()) return nullptr;

    // Selected against the class rather than by the name `truthy`, for the same reason
    // materializeLiteral selects `fromInt` that way: going through emitCall would put R5 in the
    // way, and a module that happens to define a plain function of that name would take over every
    // condition written in it.
    ClassFunRef reference { typeClass, global[typeClass]->functions.get(global, 0).name, 0 };
    ClassMatch match;
    ResolvedArg args[] = { value };

    if(matchClassFun(reference, { args, 1 }, {}, module.scalar.bool_, match)) {
        if(match.instance) {
            // Through emitInstanceCall rather than straight to the implementation, because what
            // stands in the slot may be a parametric head's generic body or the class's own
            // default, neither of which is a function about this type until it is specialized.
            if(local[match.instance]->functions.get(local, match.index)) {
                return emitInstanceCall(module, match.instance, toBuffer(match.instanceArgs), match.index,
                                        { args, 1 }, source);
            }
        } else if(isGeneric(global, type)) {
            // In a generic body the instance is the caller's to supply, exactly as it is for any
            // other class call this body's own type variables decide.
            return emitGenericDispatch(match, { args, 1 }, source, StringId());
        }
    }

    context.diagnostics.error("%@ cannot be used as a condition - it has no Truth instance"_v, source,
                              describeType(context, global, type));
    return nullptr;
}

ModulePtr<Value> ExprResolver::settle(ModulePtr<Value> value, LocationId source) {
    if(!value || !isLiteral(global, valueType(value))) return value;
    return materializeLiteral(value, literalDefault(valueType(value)), source);
}

/*
 * Conversion is a class operation.
 *
 * `Widen(a, b)` is lossless and applied implicitly; `Narrow(a, b)` is lossy and has to be
 * written. Which of the two relates a pair of types is the whole of the rule - there is no table
 * of the primitives anywhere - so a user type joins either ladder by writing an instance, and the
 * precision diagnostic is derived from the pair of classes rather than special-cased.
 *
 * Two guardrails keep this from becoming a conversion soup: one step is tried, never a chain, and
 * widening applies in conversion positions only. The single exception is commonWiden(), which
 * unifies the positions bound to one class variable so that `1 + 2.5` has an instance to reach.
 *
 * The primitive instances are intrinsics, so this still produces exactly one `cast` in the IR.
 */
ModulePtr<Value> ExprResolver::emitConversion(GlobalPtr<TypeClass> typeClass, StringId method,
                                              ModulePtr<Value> value, TypePtr target, LocationId source) {
    if(!typeClass) return nullptr;

    ClassFunList candidates;
    findClassFunctions(module, method, source, candidates);

    for(auto& candidate: candidates) {
        if(candidate.typeClass != typeClass) continue;

        ResolvedArg args[] = { value };

        ClassMatch match;
        if(!matchClassFun(candidate, { args, 1 }, {}, target, match) || !match.instance) continue;

        if(!local[match.instance]->functions.get(local, match.index)) continue;

        return emitInstanceCall(module, match.instance, toBuffer(match.instanceArgs), match.index,
                                { args, 1 }, source);
    }

    return nullptr;
}

TypePtr ExprResolver::commonWiden(TypePtr lhs, TypePtr rhs) {
    if(!lhs || !rhs) return nullptr;
    if(sameType(lhs, rhs)) return lhs;

    TypePtr up[] = { lhs, rhs };
    if(findInstance(module, module.coreClasses.widen, { up, 2 })) return rhs;

    TypePtr down[] = { rhs, lhs };
    if(findInstance(module, module.coreClasses.widen, { down, 2 })) return lhs;

    return nullptr;
}

/*
 * The three conversions a borrow takes part in (Design.md's "Borrows in return position").
 *
 * Taking one is not written at the call site any more than a `&` argument's sigil is: a position
 * that wants `&T` and is handed something that names storage of type `T` borrows it, which is what
 * makes `fn get(return self: Array(a), index: I64) -> &a = *(self.data + index)` an ordinary body
 * rather than one that has to say what it is doing twice.
 *
 * Reading through one is the mirror image, and is what lets a returned borrow be used as the value
 * it refers to without the caller ever naming the borrow.
 *
 * Weakening a mutable borrow to an immutable one is allowed because it hands back capability rather
 * than taking it: the borrow checker still sees the original exclusive loan, since the reborrow is
 * rooted in it.
 */
ModulePtr<Value> ExprResolver::convertBorrow(ModulePtr<Value> value, TypePtr from, TypePtr target,
                                             LocationId source) {
    if(isBorrow(global, target)) {
        auto wanted = (BorrowType*)global[target];

        if(isBorrow(global, from)) {
            auto held = (BorrowType*)global[from];

            if(held->to != wanted->to || wanted->mut) {
                context.diagnostics.error("cannot convert %@ to %@"_v, source,
                                          describeType(context, global, from),
                                          describeType(context, global, target));
                return value;
            }

            return ref(emit<InstBorrow>(source, StringId(), target, Place::inBorrow(value), false));
        }

        if(!sameType(from, wanted->to)) {
            context.diagnostics.error("cannot convert %@ to %@"_v, source,
                                      describeType(context, global, from),
                                      describeType(context, global, target));
            return value;
        }

        // Only something that names storage can be borrowed. A computed value names none, and
        // borrowing a temporary this expression created would hand out a reference to storage
        // whose lifetime ends before the caller can look at it.
        auto place = findPlace(value);
        if(!place) {
            context.diagnostics.error("cannot borrow this - a borrow must name storage, and this is a value with none"_v,
                                      source);
            return value;
        }

        if(wanted->mut && !isWritablePlace(place.unwrap())) {
            context.diagnostics.error("cannot borrow this mutably - it does not name storage that may be written"_v,
                                      source);
            return value;
        }

        return borrowPlace(place.unwrap(), target, source);
    }

    if(sameType(((BorrowType*)global[from])->to, target)) {
        return load(Place::inBorrow(value), source, local[value]->name);
    }

    context.diagnostics.error("cannot convert %@ to %@"_v, source,
                              describeType(context, global, from),
                              describeType(context, global, target));
    return value;
}

/*
 * Between a `@bits` refinement and what it refines.
 *
 * Neither direction is a class instance, and that is the point: `@bits` exists so that a program can
 * narrow storage without acquiring a family of arithmetic types, so if the conversion needed a
 * `Widen(Id, U64)` somebody would have to write one per refinement and the feature would cost more
 * than it saves.
 *
 * The same reasoning reaches one type further, and §0.1.1 is what made it necessary rather than
 * merely true: a refinement whose range fits inside an integer of *another* canonical type widens
 * into it. Without this the library cannot be written at all once `::` stops narrowing - every
 * `self.length :: Int` over a `Count` is a lossless conversion the ladder calls lossy, and retyping
 * those lengths as `Size` does not help, since `Size` is `Int` on JS and `Count` does not widen into
 * that either. The cast it emits is the same no-op the same-canonical case emits, and for the same
 * reason: a value in a register is already in its type's normal form.
 *
 * Widening is free. The two types have the same `width`, so the value is already the right bits in
 * the right register and only the *type* of the IR value changes - which is what the cast says, and
 * lowering emits nothing for a same-width one.
 *
 * Narrowing reduces to the refinement's range. A refined value has to satisfy its own range or the
 * patterns above it are not free after all, and a `Maybe(Id)` that folded its discriminant into
 * those patterns would read a `Just` holding 2^60 as a `Nothing`. So this is not an optimization to
 * skip - it is what makes the niche true. Per this pass's scope it truncates silently; the
 * debug-build range check that reports instead is a later addition, and this is the one place it
 * will go.
 *
 * **An unsigned refinement masks and a signed one sign-extends**, which is the same pair
 * `decodePackedField` and `truncateToWidth` use and not a choice this function gets to make
 * independently. The invariant the rest of the compiler is written against is that a value in a
 * register is in its canonical type's normal form: `encodeBits` masks what it is handed precisely
 * because "a signed one arrives sign-extended, so its high bits are ones exactly when they must not
 * be stored". Masking here broke that - a `@bits(4) I32` holding -4 became 12 and stayed 12, since
 * widening back is a cast that lowers to nothing and so had no way to undo it. Design.md's rule is
 * that a load widens by zero- *or sign*-extension as the type asks.
 */
ModulePtr<Value> ExprResolver::convertRefinement(ModulePtr<Value> value, TypePtr from, TypePtr target,
                                                 LocationId source) {
    if(global[from]->kind != Type::Int || global[target]->kind != Type::Int) return nullptr;

    auto& wanted = *(IntType*)global[target];
    auto& held = *(IntType*)global[from];

    auto canonical = canonicalType(global, from);

    if(canonical != canonicalType(global, target)) {
        if(!held.canonical || !integerRangeFits(held, wanted)) return nullptr;
        return ref(emit<InstUnary>(source, local[value]->name, target, Value::Cast, value));
    }

    // Widening, including a refinement to a narrower one that already fits inside the target.
    if(held.bits <= wanted.bits) {
        return ref(emit<InstUnary>(source, local[value]->name, target, Value::Cast, value));
    }

    if(wanted.isSigned) {
        /*
         * The canonical type's *own* width rather than the register's.
         *
         * The pair below is a shift at the canonical type, and a shift at a type wraps at that
         * type's width - which is the whole of "arithmetic at native size", and is emitted as a
         * truncation on both targets for a primitive narrower than the register it lowers into. So
         * for `WideInt`, whose 53 bits sit in a 64-bit register, shifting up by `64 - n` throws the
         * value out the top and shifting back brings nothing useful down: narrowing `2^39 - 1` to
         * `@bits(40) WideInt` answered zero, on both targets and therefore silently.
         *
         * `bits` is `registerBits` for every primitive that fills its register, so this is the same
         * distance everything else was already getting.
         */
        auto distance = U32(((IntType*)global[canonical])->bits) - wanted.bits;
        auto up = ref(emit<InstBinary>(source, StringId(), from, Value::Shl, value,
                                       makeInt(source, from, distance)));
        auto down = ref(emit<InstBinary>(source, StringId(), from, Value::Sar, up,
                                         makeInt(source, from, distance)));

        return ref(emit<InstUnary>(source, local[value]->name, target, Value::Cast, down));
    }

    auto mask = wanted.bits >= 64 ? maxLimit<U64> : (U64(1) << wanted.bits) - 1;
    auto masked = ref(emit<InstBinary>(source, StringId(), from, Value::And, value,
                                       makeInt(source, from, mask)));

    return ref(emit<InstUnary>(source, local[value]->name, target, Value::Cast, masked));
}

/*
 * A host array's length, as the one instruction it is - `arr.length`.
 *
 * Built here rather than by calling `Host`'s `hostLength`, because this is compiler code and a call
 * it would then have to inline is a worse way to say one property read. It is the same node the
 * declaration expands to.
 */
ModulePtr<Value> ExprResolver::hostArrayLength(ModulePtr<Value> items, LocationId source) {
    auto instruction = create<InstNative>(source, StringId(), module.scalar.size, NativeOp::HostField,
                                          context.addUnqualifiedName("length", 6));

    instruction->args.push(module.arena, items);
    append(instruction);

    return ref(instruction);
}

/*
 * Borrowing a container on JS - Implementation-Containers.md §4.3 and §14.
 *
 * The same descriptor over the same three questions, and every one of them has a different answer
 * because a host array is indexed rather than addressed:
 *
 *  - **the base** is the array itself rather than a computed address, so a window into it is the
 *    whole array plus where the window starts. That is the third field, and it is the whole of what
 *    §4.3's three-component slice is;
 *  - **the length** is `arr.length` for a growable container, which is why there is no count field
 *    on the JS `Array(a)` to read: the host already keeps one and keeping a second would be two
 *    numbers that can disagree;
 *  - **a `[T *n]`** is a host array too - `zeroValue` builds one of `n` elements - so its arm is the
 *    array itself and a constant, which is the same shape the native arm has and for once the same
 *    cost. This is the gap §6 recorded as "not done here: the JS half", and the reinterpretation is
 *    what closes it: a fixed array and a run of elements are one host value, so the conversion
 *    between their types moves nothing.
 *
 * The loan, the `viewOf` and the writability rule are the native path's, unchanged - none of them is
 * about how the elements are reached.
 */
ModulePtr<Value> ExprResolver::convertSliceJs(ModulePtr<Value> value, const Place& array,
                                              const Place& owner, TypePtr from, TypePtr target,
                                              TypePtr element, TypePtr fixed, LocationId source,
                                              bool mut) {
    ModulePtr<Value> items = nullptr;
    ModulePtr<Value> count = nullptr;

    if(fixed) {
        auto pointer = resolvePointerType(module, element);
        items = ref(emit<InstUnary>(source, StringId(), pointer, Value::Cast, load(array, source)));
        count = countOf(((ArrayType*)global[from])->count, module.scalar.size, source);
    } else {
        auto held = projectField(array, context.addUnqualifiedName("items", 5), source, source);
        auto stored = projectField(array, context.addUnqualifiedName("length", 6), source, source);
        if(!held || !stored) return nullptr;

        items = load(held.unwrap(), source);

        // The container's own count and not the host array's length, which is its capacity - see
        // Implementation-Containers.md §14's typed row. A slice is a window on what the container
        // holds, and what it holds is what it says it holds on both targets.
        count = load(stored.unwrap(), source);
    }

    auto storage = allocate(target, source, local[value]->name,
                            mut ? ast::BindType::Ref : ast::BindType::Borrow);
    auto descriptor = placeFor(storage, source);
    auto slice = project(descriptor, ProjectionKind::Downcast, 0);

    if(owner.root == PlaceRoot::Local && owner.local < function.localCount()) {
        auto entry = function.localAt(local, descriptor.local);
        entry.viewOf = owner.local;
        function.locals.set(local, descriptor.local, entry);
    }

    if(auto declared = sliceLengthType(module, target)) count = convert(count, declared, source, false);

    initialize(project(slice, ProjectionKind::Field, 0), items, source);
    initialize(project(slice, ProjectionKind::Field, 1), count, source);

    // The window's start, which is zero for every conversion *from an owner*: a borrow of a whole
    // container begins at its beginning. A sub-window is `slice`'s, and that is written in the
    // language rather than here.
    initialize(project(slice, ProjectionKind::Field, 2), makeInt(source, module.scalar.size, 0), source);

    return storage;
}

/*
 * Borrowing a container - Implementation-Containers.md §4.
 *
 * `f(xs)` where `f` said `[T]` hands over a `{base, length}` descriptor rather than the array, and
 * the two instructions in front of that are the whole of what makes it sound:
 *
 *  - an **InstBorrow of the array's own place**, which is the loan. Without it the last thing this
 *    frame does with `xs` is read its run pointer, so the drop pass is entitled to release the run
 *    *before* the call that is about to read through it. With it, the borrow checker sees an extent
 *    covering the call and rejects a conflicting write inside it, which is the ordinary rule rather
 *    than one about arrays.
 *  - the descriptor built into a temporary, read through the borrow. What travels is a copy of the
 *    run's base address and the array's length, so the callee cannot grow it and never learns where
 *    the owner is - which is exactly the capability `[T]` names.
 *
 * A slice source needs neither: a `Flat(T)` is already the descriptor, and a borrow of one is
 * itself (see sliceOf). The residual gap is that the descriptor holds a `%T`, which is outside the
 * ownership graph - so a slice *stored* past the loan is not caught. That is Native's documented
 * seam and not a new one; see analyze.cpp's note on places rooted in a raw pointer.
 */
/*
 * The capability half of a slice conversion, asked where a whole conversion is not - §4.5.
 *
 * A memory-typed destination is filled in place: `fillTuple` resolves the field's expression *at*
 * the field's type and then writes it, without calling convert(), because for a record or an array
 * the value was built in the destination's own storage and a second conversion would copy it. That
 * is right about the bytes and was silent about the one thing a slice adds - two types over one
 * layout - so a shared window stored into a `&[T]` field went in unchallenged, and a write through
 * it would then reach a place some other view is still watching.
 *
 * So the two directions are asked here and nowhere else. Giving up the write is free and is the
 * value unchanged; taking one is refused, because there is nothing to build it out of - the
 * descriptor was made over a shared borrow of the owner, and no later conversion can make that
 * borrow exclusive after the fact.
 */
ModulePtr<Value> ExprResolver::convertSliceCapability(ModulePtr<Value> value, TypePtr expected,
                                                      LocationId source) {
    if(!value || !expected) return value;

    auto held = valueType(value);
    if(!sliceElement(module, expected) || !sliceElement(module, held)) return value;
    if(unrefined(global, held) != unrefined(global, expected)) return value;
    if(isMutableSlice(global, held) == isMutableSlice(global, expected)) return value;

    if(isMutableSlice(global, expected)) {
        context.diagnostics.error("cannot store %@ where %@ is wanted - an exclusive window has to be built from an exclusive borrow of the owner, and this one was not. Take the owner with `&` where the window is made"_v,
                                  source, describeType(context, global, held),
                                  describeType(context, global, expected));
        return value;
    }

    return value;
}

ModulePtr<Value> ExprResolver::convertSlice(ModulePtr<Value> value, TypePtr from, TypePtr target,
                                            LocationId source, bool mut) {
    auto element = sliceElement(module, target);
    auto owned = ownedElement(module, from);
    if(!element || !owned) return nullptr;

    /*
     * Three owners, one descriptor, and one element type across all three -
     * Implementation-Containers.md §6-§7.
     *
     * A growable `Array(T)`, a fixed `[T *n]` and an `@inline(n) @capacity(n)` array all borrow as
     * the same `{base, length}` pair, and what separates them is only where the two halves come
     * from: the growable one loads both out of fields, the fixed one *is* them, and the refined one
     * is half of each - its slots are addressed the way a `[T *n]`'s are, while its length is stored,
     * because it grows within its bound. No fourth descriptor shape exists, which is §1's whole
     * point: what varies is the owner and never the borrow.
     *
     * So one question decides for all three, and it is the element type. A container whose elements
     * are not the window's is not a borrow of it at all - `[Int *4]` and `Flat(Long)` are two
     * unrelated types the ladder is walking, and so are `Array(Int)` and `Flat(I64)`.
     *
     * **This is the only place that asks.** A parameter written `xs: [I64]` folds to `Flat(I64)`,
     * and a call to a non-generic function reaches convert() directly - a concrete signature never
     * goes through `bindInto` - so nothing between the argument and this line compares anything. The
     * growable owner used to be exempt here, on the grounds that `Array(T)` and `Flat(U)` are
     * already different types; which is true, and is the *reason* a conversion is attempted rather
     * than a reason one cannot be built. It was built: same base, same count, elements read at the
     * wrong width, so `[7, 8] :: [Int]` at an `[I64]` parameter answered one 64-bit element and then
     * a load past the end of the run. Under `&` it is the same descriptor with a store through it,
     * since borrowArgument's mutable arm comes through here as well.
     *
     * `convertibleType` has compared them all along, which is what makes overload selection agree
     * with this function; this is that same sentence said on the path that performs the conversion.
     */
    if(owned != element) return nullptr;

    auto fixed = fixedElement(module, from);
    auto inlineOwner = inlineRefinement(module, from);

    auto place = findPlace(value);
    if(!place) {
        context.diagnostics.error("cannot borrow this array - a slice must name storage, and this is a value with none"_v,
                                  source);
        return nullptr;
    }

    if(mut && !isWritablePlace(place.unwrap())) {
        context.diagnostics.error("cannot borrow this array mutably - it does not name storage that may be written"_v,
                                  source);
        return nullptr;
    }

    auto borrowed = borrowPlace(place.unwrap(), resolveBorrowType(module, from, mut), source);
    if(!borrowed) return nullptr;

    auto array = Place::inBorrow(borrowed);

    if(isJsMode(context.settings.mode)) {
        return convertSliceJs(value, array, place.unwrap(), from, target, element, fixed, source, mut);
    }

    /*
     * Where the two halves of the descriptor come from, which is the whole of the difference between
     * the two owners - Implementation-Containers.md §6.
     *
     * A growable array holds them: the run's base and the array's count are two fields, projected
     * here and loaded below. A `[T *n]` *is* them: the base is the array's own storage and the
     * length is in the type, so its arm reads nothing at run time at all - one address computation
     * and one constant, against two loads.
     *
     * Left as places rather than loaded here so that the instructions stay in the order they were
     * in before a second owner existed. The descriptor's storage comes first, then the reads that
     * fill it, which is what a reader of the IR expects and what every fixture already says.
     */
    Maybe<Place> base = Nothing();
    Maybe<Place> length = Nothing();
    Maybe<Place> slots = Nothing();

    if(!fixed) {
        auto items = projectField(array, context.addUnqualifiedName("run", 3), source, source);
        length = projectField(array, context.addUnqualifiedName("length", 6), source, source);
        if(!items || !length) return nullptr;

        // The refined owner stops here: its run *is* the slots, so what the plain one loads out of a
        // field is what this one takes the address of.
        if(inlineOwner) {
            slots = items;
        } else {
            base = projectField(items.unwrap(), context.addUnqualifiedName("items", 5), source, source);
            if(!base) return nullptr;
        }
    }

    // Writable exactly when the borrow was, because that is what a `&` slice argument needs to
    // borrow the temporary back out of - and nothing else ever writes a descriptor.
    auto storage = allocate(target, source, local[value]->name,
                            mut ? ast::BindType::Ref : ast::BindType::Borrow);
    auto descriptor = placeFor(storage, source);
    auto slice = project(descriptor, ProjectionKind::Downcast, 0);

    /*
     * What this descriptor is a view of, so that liveness reads it as one - see Local::viewOf.
     *
     * Without it the array's last use is the read above, and the drop pass is entitled to release
     * the run before the call this descriptor was built for. With it, the array is live wherever the
     * slice is, which is conservative in the safe direction: a slice never outlives its array, and a
     * slice that dies early only keeps the array a little longer than it had to.
     */
    auto borrowedPlace = place.unwrap();

    if(borrowedPlace.root == PlaceRoot::Local && borrowedPlace.local < function.localCount()) {
        auto entry = function.localAt(local, descriptor.local);
        entry.viewOf = borrowedPlace.local;
        function.locals.set(local, descriptor.local, entry);
    }

    /*
     * The length, converted rather than copied across.
     *
     * The owner's count and the descriptor's are two fields of two types, and they only *happened* to
     * be one type while both were `Int`. An owner's is now a `Count` - narrow and unsigned, so that
     * it packs beside the run's placement flag (§10.2) - while a `Flat`'s is an `Int`, because `Flat`
     * is `Native`'s public representation type and a refinement there would put a `::` in front of
     * every comparison a decoder writes.
     *
     * Initializing one from the other without asking is what that difference costs if nobody asks:
     * the value is right for any count either type can hold, so it works, and the IR is ill-typed and
     * stays that way until one of the two widths moves. `sliceLengthType` is the question being asked
     * out loud.
     *
     * **Explicit**, and it has to be explicit on one target rather than both: a `Count` widens into a
     * native `Size` for free and *narrows* into a JS one, since `Size` is `WideInt` there and `Int`
     * here (see the alias). Neither loses anything, and the bound is why - a `Count` is thirty-one
     * bits, so every value one can hold is inside `Int`'s positive range by construction - but only
     * the widening direction is a conversion the ladder performs on its own. This is the same `::`
     * that `capacity` writes by hand in `Native`, at the one boundary the compiler builds rather than
     * the program.
     *
     * **A fixed array's count is built at the descriptor's own type rather than converted into it**,
     * which is a conversion removed rather than a special case added. It used to be made at `Long`
     * and narrowed here, and that only worked while `Size` was `I64`: once the width became the
     * target's (Move 2) a `Long` no longer fits a `Size`, and the count of a `[T *n]` is a number
     * this compiler chose - it is not a 64-bit value that has to be got down to a word, it is `n`.
     */
    auto items = fixed
        ? fixedArrayBase(array, element, source)
        : (inlineOwner ? fixedArrayBase(slots.unwrap(), element, source) : load(base.unwrap(), source));

    initialize(project(slice, ProjectionKind::Field, 0), items, source);

    auto declared = sliceLengthType(module, target);

    auto count = fixed
        ? countOf(((ArrayType*)global[from])->count, declared ? declared : module.scalar.long_, source)
        : load(length.unwrap(), source);

    if(declared && !fixed) count = convert(count, declared, source, false);

    initialize(project(slice, ProjectionKind::Field, 1), count, source);

    return storage;
}

/*
 * Why a conversion that looks free was refused, where one side's width is the *target's*.
 *
 * `let n: Size = someU32` is the case, and the refusal is correct and surprising in the same
 * breath: on the only native machine there is, `Size` is a signed 64-bit integer and a `U32` fits
 * it twice over. What decides the ladder is not that machine but the *bound* - `Size` is whatever
 * word the target picked, so the language guarantees it 32 bits, and a signed 32 holds 31 positive
 * ones. A `U32` needs 32. The refusal is what stops the same source being correct here and lossy on
 * a JavaScript build, which is the one thing an abstract width exists to prevent.
 *
 * So the message above is true and answers the wrong question, and the note answers the one that was
 * asked. It names `USize` first because that is nearly always the right destination: it is the same
 * target word read unsigned, so every `U32` fits it on every target and no conversion is written at
 * all. `truncate` stays the answer where a *signed index* is genuinely what was wanted.
 *
 * Only where an abstract width is involved. Between two fixed widths the ordinary message already
 * says everything - a `U64` does not fit an `I32` on any machine, and nobody is surprised.
 */
void ExprResolver::explainAbstractWidth(TypePtr from, TypePtr target, LocationId source) {
    if(!from || !target) return;
    if(global[from]->kind != Type::Int || global[target]->kind != Type::Int) return;

    auto& source_ = *(IntType*)global[from];
    auto& wanted = *(IntType*)global[target];
    if(wanted.target == TargetInt::None) return;
    if(source_.target != TargetInt::None) return;

    // The pair this is about: it fits the *widest* the target may be and not the narrowest, which
    // is exactly the gap between "works on my machine" and "works". A source that fits neither is
    // an ordinary narrowing and needs no explaining.
    IntType widest = wanted;
    widest.bits = wanted.maxBits();
    widest.target = TargetInt::None;

    if(!integerRangeFits(source_, widest)) return;

    auto positive = U32(wanted.minBits()) - (wanted.isSigned ? 1u : 0u);

    context.diagnostics.message(Diagnostics::MessageLevel,
                                "%@ is the width the *target* picks, so what the language guarantees is %@ bits%@ - %@ of them for a positive value, and %@ needs %@. `USize` is the same word read unsigned and holds every %@ on every target; `truncate(x) :: %@` is the low bits where a signed index really is what was wanted"_v,
                                source, describeType(context, global, target), wanted.minBits(),
                                wanted.isSigned ? " and signed"_v : ""_v, positive,
                                describeType(context, global, from), source_.maxBits(),
                                describeType(context, global, from),
                                describeType(context, global, target));
}

/*
 * And why an array did not become the window a parameter asked for, where the elements are the only
 * thing between them - see convertSlice, which is where the refusal is.
 *
 * Worth its own sentence for the same reason the one above is: the general message is true and
 * answers a question nobody asked. What the author sees is `[Int]` and `[I64]`, two spellings that
 * look like a width the compiler widens for free - and the difference is not a width at all. A
 * window is the array's own buffer addressed at the window's element type, so there is nothing here
 * to convert: making the two agree would mean building a second buffer at an argument position
 * nobody wrote a call at, which is the same refusal §5 makes for a `Chunked` container.
 *
 * Both argument positions ask. A `&` one goes through borrowArgument, which commits to the slice
 * conversion as soon as it sees an owner against a window and has nothing of its own to say when it
 * does not happen - so without this the exclusive path reported an argument that was not there.
 */
bool ExprResolver::explainSliceElements(TypePtr from, TypePtr target, LocationId source) {
    if(!from || !target) return false;

    auto element = sliceElement(module, target);
    auto owned = ownedElement(module, from);
    if(!element || !owned || owned == element) return false;

    context.diagnostics.error("cannot pass %@ where %@ is wanted: its elements are %@ and the window's are %@. A window is the array's own buffer read at the window's element type, so the two have to be the same type - a difference in width is not something a window can convert across, because it does not copy anything"_v,
                              source, describeType(context, global, from),
                              describeType(context, global, target),
                              describeType(context, global, owned),
                              describeType(context, global, element));
    return true;
}

ModulePtr<Value> ExprResolver::convert(ModulePtr<Value> value, TypePtr target, LocationId source, bool implicit) {
    if(!value || !target) return value;

    auto from = local[value]->type;

    // A literal has no type to convert from: it is built at whatever type this position asks for,
    // through its own class, which is also how it reaches a user type that has an instance.
    if(isLiteral(global, from)) return materializeLiteral(value, target, source);

    if(sameType(from, target)) return value;
    if(global[from]->kind == Type::Error || global[target]->kind == Type::Error) return value;
    if(auto refined = convertRefinement(value, from, target, source)) return refined;

    /*
     * A refined container at a parameter written `Array(a)` - Implementation-Containers.md §7.2.
     *
     * Before the slice conversion, because both are available and this one is what the position
     * asked for: `elements(xs)` takes `Array(a)` and would silently become `elements(slice(xs))` if
     * the slice route won, which is a different overload rather than a different representation.
     * Reading only, since a value argument has nothing to write back - a `&` one comes through
     * borrowArgument instead, and that is the path that queues the count.
     */
    if(inlineRefinement(module, from) && unrefined(global, from) == target) {
        if(auto place = findPlace(value)) {
            if(auto descriptor = inlineArrayDescriptor(place.unwrap(), from, source, false)) return descriptor;
        }
    }

    /*
     * An exclusive window where a shared one is wanted - Analysis-Borrows.md §4.5.
     *
     * `&[T]` and `[T]` are the same pointer and the same length and differ only in what may be done
     * through them, so giving up the write is free and is the one direction that is sound. It is the
     * slice's spelling of what `&T` to `'T` is for a direct reference, and it is why a `data Cursor
     * {buf: &[U8]}` can still be read by every function that takes a plain window.
     *
     * Before `convertSlice`, because that one asks `sliceOf` and would answer for this pair too -
     * and its answer is a *conversion* where this is a retype: an owner becoming a window builds a
     * descriptor, and a window losing its capability builds nothing at all.
     */
    if(unrefined(global, from) == target && isMutableSlice(global, from)) return value;

    /*
     * And the other direction, which is sound exactly where the place says it is.
     *
     * A `&xs: [T]` parameter is an exclusive window and its *type* has forgotten it: the fold put
     * the capability on the convention, which is what keeps one interned `Flat(T)` and one set of
     * library signatures. So a body that has to hand the window on at its declared capability -
     * `fn elementsMut(&self: Flat(a)) -> &Flat(a) = self`, the identity instance of `Writable` - has
     * a value whose bytes are exactly right and whose type says less than the frame knows.
     *
     * `isWritablePlace` is what it knows, and it is §2.5's rule read in the other direction: the
     * innermost reference the path crosses decides, so a Ref-convention slot and a `&[T]` field both
     * answer yes and a shared window in a record answers no. That makes this a *retype* rather than
     * a promotion - the capability is being read off the place it was already recorded on, not
     * invented from the fact that some root happens to be mutable, which is what §2.3 removed.
     */
    if(unrefined(global, target) == from && isMutableSlice(global, target)) {
        if(auto place = findPlace(value)) {
            if(isWritablePlace(place.unwrap())) return value;
        }
    }

    /*
     * And an owner becoming one, at whichever capability the destination asks for.
     *
     * `mut` used to be false here always, which was right while there was one slice type: the
     * exclusive window was reached only through `borrowArgument`, and a field could not ask for one.
     * Now it can, and passing the target's capability through is the whole of what makes
     * `data Cursor {buf: &[U8]}` sound - the descriptor is built over an exclusive borrow of the
     * owner, so the place has to be writable and no second view of it may be live.
     */
    if(auto sliced = convertSlice(value, from, target, source, isMutableSlice(global, target))) return sliced;

    /*
     * A container of the program's own, reaching `[T]` through its `Contiguous` instance -
     * Implementation-Containers.md §5.
     *
     * `Contiguous` is the promise that this type has a buffer address, so `elements` is the whole of
     * the conversion and there is nothing here to build: one call, whose result is a view rooted in
     * the argument by the `return` marker the class declares. Which makes this the *only* implicit
     * conversion into a slice a program can grant itself, and deliberately so - a `Chunked` container
     * would need an O(n) copy to become one, and §5 refuses to hide one behind an argument position.
     * See the diagnostic at the end of this function, which says that where it happens.
     */
    if(auto element = sliceElement(module, target)) {
        auto contiguous = contiguousElement(module, from);

        // Not to an exclusive window. `elements` hands back what its own signature says, which is a
        // shared view, and a conversion may not promote one - that is the same rule that stopped
        // `applyReturnRootMutability` promoting a result from its roots (§2.3), arriving at the one
        // place a slice could still be widened silently.
        if(contiguous && sameType(contiguous, element) && !isMutableSlice(global, target)) {
            auto converted = emitConversion(module.coreClasses.contiguous,
                                            context.addUnqualifiedName("elements", 8), value, target, source);
            if(converted) return converted;
        }
    }

    // A borrow converts to and from exactly one thing - the type it refers to - so when either side
    // is one, that is the whole of the decision and there is no widening path to fall through to.
    if(isBorrow(global, from) || isBorrow(global, target)) {
        return convertBorrow(value, from, target, source);
    }

    if(auto widened = emitConversion(module.coreClasses.widen, context.addUnqualifiedName("widen", 5),
                                     value, target, source)) {
        return widened;
    }

    /*
     * A narrowing conversion exists, and neither an implicit position nor an ascription may take
     * it: `::` selects a target type or widens, and nothing else. A conversion that loses
     * information has to be written as a call.
     *
     * This is where the two arms used to differ. `::` passed `implicit == false` and took the
     * `Narrow` instance, which made an ascription the one construct in the language that could
     * silently remove data - `length(xs) :: Int` truncating a 64-bit length to 32 on one target and
     * being free on the other. The rest of what `::` does is untouched, because none of it comes
     * through here: pushing the ascribed type into a literal, a constructor, an array literal or a
     * lambda resolves *against* the type, and a call keeps its own result so that return-type
     * overloading still selects by ascription.
     *
     * Asking the instance table rather than building the conversion first keeps the diagnostic
     * about precision instead of about an instance the author never mentioned, and leaves no
     * half-built conversion behind.
     */
    TypePtr pair[] = { from, target };

    if(findInstance(module, module.coreClasses.narrow, { pair, 2 })) {
        if(implicit) {
            context.diagnostics.error("implicit conversion from %@ to %@ would lose precision"_v, source,
                                      describeType(context, global, from),
                                      describeType(context, global, target));
        } else {
            context.diagnostics.error("cannot ascribe %@ to %@: the conversion loses precision. `::` may widen but not narrow - write `truncate(x)` for the low bits, or `bitcast(x)` to reinterpret them"_v,
                                      source, describeType(context, global, from),
                                      describeType(context, global, target));
        }

        explainAbstractWidth(from, target, source);
        return value;
    }

    // The neighbouring refusal, and the commoner one: a container that *is* contiguous and simply
    // holds something else - see explainSliceElements, and convertSlice, which is what declined.
    if(explainSliceElements(from, target, source)) return value;

    /*
     * The refusal §5 is built around: a container that is `Chunked` and not `Contiguous`, where a
     * `[T]` was expected.
     *
     * `[T]` is an address and a length, and a chunked container has no single one of either - so
     * making this work would mean copying every element into a fresh buffer at an argument position
     * nobody wrote a call at. What the author changes is the parameter: a function that only reads
     * elements should ask for `Chunked`, and then it accepts this container *and* every contiguous
     * one, with no dispatch left after specialization.
     */
    if(sliceElement(module, target) && chunkedElement(module, from)) {
        context.diagnostics.error("%@ is `Chunked` and not `Contiguous`, so it cannot be passed as %@ - its elements are not one buffer, and flattening them would be a copy this position does not say it makes. A function that only reads elements should take `fn (Chunked(c, a)) f(xs: c)` instead, which this container satisfies"_v,
                                  source, describeType(context, global, from),
                                  describeType(context, global, target));
        return value;
    }

    context.diagnostics.error("cannot convert %@ to %@"_v, source,
                              describeType(context, global, from), describeType(context, global, target));
    return value;
}

bool ExprResolver::convertible(ModulePtr<Value> value, TypePtr target, LocationId source) {
    return value && convertibleType(valueType(value), target);
}

// The same question convert() answers, asked without answering it. Overload selection has to know
// whether a candidate accepts an argument before it commits to that candidate, and convert()
// cannot be used for that: reporting the mismatch is its job, and a candidate that does not fit is
// not an error while another member of the overload set may still serve the call.
bool ExprResolver::convertibleType(TypePtr from, TypePtr target) {
    if(!from || !target) return false;

    if(isLiteral(global, from)) return literalFits(from, target);
    if(sameType(from, target)) return true;

    // An error type has already been reported once, so it fits anything rather than producing a
    // second diagnostic about the same mistake.
    if(global[from]->kind == Type::Error || global[target]->kind == Type::Error) return true;

    // The same three cases convertBorrow emits, asked without emitting. A borrow of a value with
    // no place is left to convert() to report, since a candidate rejected here would instead be
    // reported as no matching overload, which says less about what is wrong.
    if(isBorrow(global, target)) {
        auto wanted = ((BorrowType*)global[target])->to;
        return sameType(from, wanted) ||
               (isBorrow(global, from) && ((BorrowType*)global[from])->to == wanted);
    }

    if(isBorrow(global, from)) return sameType(((BorrowType*)global[from])->to, target);

    // An owned container fits a `[T]` parameter, which is what makes `sum(xs)` select an overload
    // declared over the slice - see convertSlice. A container with a `Contiguous` instance fits the
    // same position through the call convert() emits, and selection has to agree about that or a
    // candidate taking `[T]` is rejected for an argument convert() would have accepted.
    if(auto element = sliceElement(module, target)) {
        if(ownedElement(module, from) == element) return true;
        if(contiguousElement(module, from) == element) return true;
    }

    // A `@bits` refinement converts to and from what it refines without an instance - see
    // convertRefinement. Overload selection has to agree with convert() about that, or a candidate
    // taking a `U64` would be rejected for an `Id` argument convert() would have accepted.
    if(global[from]->kind == Type::Int && global[target]->kind == Type::Int) {
        if(canonicalType(global, from) == canonicalType(global, target)) return true;

        // And the same about a refinement whose range fits an integer of another canonical type,
        // which convertRefinement widens.
        auto& held = *(IntType*)global[from];
        if(held.canonical && integerRangeFits(held, *(IntType*)global[target])) return true;
    }

    TypePtr args[] = { from, target };
    return findInstance(module, module.coreClasses.widen, { args, 2 }) != nullptr;
}
