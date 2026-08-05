#include "expr.h"
#include "complete.h"
#include "generic.h"
#include "name.h"
#include "index.h"

void ExprResolver::terminate(Inst* inst) {
    assertTrue(isTerminator(*inst));
    current = nullptr;
}

void ExprResolver::emitCheck(ModulePtr<Value> failed, LocationId source) {
    if(!failed || !checksEnabled()) return;

    ModulePtr<Value> condition[] = { failed };
    emitDirectCall(module.program.checkCondition, { condition, 1 }, source);
}

Binding* ExprResolver::findBinding(StringId name, LocationId source) {
    Binding* found = nullptr;

    for(Size i = bindings.size(); i > 0; i--) {
        if(bindings[i - 1].name == name) {
            found = &bindings[i - 1];
            break;
        }
    }

    // A name a lambda body does not bind itself may still belong to an enclosing one, and naming it
    // is what makes it a capture. Nothing is a capture until it is used, which is Design-Memory
    // §8's "there is no capture list" made literal.
    if(!found) found = captureBinding(name, source);

    // §1.2's function-local choke point. A null source is a caller asking whether the name is bound
    // at all rather than reading it - resolveCall does exactly that to tell a call of a binding from
    // a call of a declaration - and an occurrence nobody wrote is not one to record.
    if(found && source != kNullLocation) recordBinding(*this, *found, source);

    return found;
}

/*
 * One binding, as the index records it.
 *
 * Which of the three local kinds it is comes off the binding rather than being carried on it: a
 * capture says so, and an ordinary binding whose value is the parameter itself is an argument. The
 * payload is the slot the kind addresses - the local index, the environment field, or the argument
 * index - which is only meaningful together with the enclosing function, and that is what `function`
 * is for.
 */
Symbol bindingSymbol(ExprResolver& resolver, const Binding& binding) {
    Symbol symbol;
    symbol.kind = Symbol::Kind::Local;
    symbol.module = &resolver.module;
    symbol.function = &resolver.function - resolver.local;
    symbol.name = binding.name;
    symbol.definition = binding.definition;
    symbol.payload = binding.local;

    if(binding.captured) {
        symbol.kind = Symbol::Kind::Capture;
        symbol.payload = binding.captureField;
    } else if(binding.value && resolver.local[binding.value]->kind == Value::Arg) {
        symbol.kind = Symbol::Kind::Arg;
        symbol.payload = ((Arg*)resolver.local[binding.value])->index;
    }

    return symbol;
}

void recordBinding(ExprResolver& resolver, const Binding& binding, LocationId source) {
    if(!resolver.context.index) return;
    recordReference(resolver.context, source, bindingSymbol(resolver, binding), bindingType(resolver, binding));
}

/*
 * The binding itself, as a definition rather than as a use - §1.2's `expr_pat.cpp` row.
 *
 * Recorded where the name is *introduced*, which the references cannot stand in for: a `let` whose
 * name is never read afterwards is recorded nowhere at all otherwise, and it is exactly the one an
 * editor is asked about while it is being written. It is also what makes find-references work from
 * the declaration rather than only from a use.
 */
void recordBindingDefinition(ExprResolver& resolver, const Binding& binding) {
    if(!resolver.context.index || binding.definition == kNullLocation) return;

    auto symbol = bindingSymbol(resolver, binding);
    recordDefinition(resolver.context, symbol);

    /*
     * And as an occurrence of itself, which is not redundant: a Symbol says which slot a name is
     * and a Reference says what type it had there, and the type of a local is not reachable from
     * the slot - an immutable binding names an SSA value rather than a frame slot at all. So a
     * declaration nothing reads afterwards - which is exactly the one being written - would
     * otherwise have no type recorded anywhere.
     *
     * The two surfaces that list occurrences leave it out where it would be a duplicate of the
     * declaration they already write; see lsp/feature.cpp.
     */
    recordReference(resolver.context, binding.definition, symbol, bindingType(resolver, binding));
}

ModulePtr<Value> ExprResolver::find(StringId name) {
    auto binding = findBinding(name);
    return binding ? binding->value : nullptr;
}

// A value in the normal form of its type: the type's own bits, sign-extended if it is signed. The
// same form `convertRefinement` puts a runtime value in, and `truncateToWidth` an arithmetic result.
static U64 reduceToWidth(const IntType& integer, U64 value) {
    if(integer.bits >= 64) return value;

    auto mask = (U64(1) << integer.bits) - 1;
    value &= mask;

    // A signed type's high bit is its sign, so a value that has it set is the negative number it
    // stands for and not the small positive one the mask left behind.
    if(integer.isSigned && (value & (U64(1) << (integer.bits - 1)))) value |= ~mask;

    return value;
}

/*
 * An integer constant, reduced to its type's normal form on the way in.
 *
 * A literal written where its type is already known is built at that type directly rather than
 * converted into it, so it reached none of the narrowing a conversion emits: `Box {small: 20}` on a
 * `@bits(4)` field stored 20, while `Box {small: v}` for a runtime `v` of 20 stored 4. Whether a
 * store narrowed came down to whether it could be folded, which is the one thing constant folding
 * must never decide.
 *
 * Every integer constant funnels through here, so this is the one place the rule has to be stated -
 * and the reason the *warning* below cannot live here: most callers hand this bits that are already
 * at the type's width.
 */
ModulePtr<Value> ExprResolver::makeInt(LocationId source, TypePtr type, U64 value) {
    if(type && global[type]->kind == Type::Int) value = reduceToWidth(*(IntType*)global[type], value);
    return constant<ConstInt>(source, type, value);
}

/*
 * Reports a written literal that does not fit the type it is being built at.
 *
 * A warning rather than an error, because `makeInt` above gives the program a defined meaning either
 * way and because a full-width mask written at a signed type - `0xFFFFFFFF :: Int` - is a real idiom
 * rather than a mistake. What it catches is the case that has no other symptom at all: `Box {small:
 * 20}` on a `@bits(4)` field is 4, and nothing in the source says 4.
 *
 * Only where a *literal* is built. Every other caller of `makeInt` hands it bits that are already at
 * the type's width - a mask this file computed, a shift distance, the stored initializer of a global
 * - so checking there would report the compiler's own constants back at the author.
 *
 * Written literals are never negative: `-1` is `0 - 1`, two literals and an operator, so the range
 * that matters is one-sided and a signed type's is half an unsigned one's.
 */
void ExprResolver::checkLiteralRange(LocationId source, TypePtr type, U64 written) {
    if(!type || global[type]->kind != Type::Int) return;

    auto& integer = *(IntType*)global[type];
    auto bits = U32(integer.bits) - (integer.isSigned ? 1 : 0);
    if(bits >= 64 || written <= (U64(1) << bits) - 1) return;

    auto reduced = reduceToWidth(integer, written);
    auto described = describeType(context, global, type);

    // Printed the way the type reads it, since the point of the message is what the program will do
    // and a signed truncation that comes out negative is exactly the surprising case.
    if(integer.isSigned) {
        context.diagnostics.warning("the literal %@ does not fit in %@ and is truncated to %@"_v,
                                    source, written, described, I64(reduced));
    } else {
        context.diagnostics.warning("the literal %@ does not fit in %@ and is truncated to %@"_v,
                                    source, written, described, reduced);
    }
}

ModulePtr<Value> ExprResolver::makeFloat(LocationId source, TypePtr type, F64 value) {
    if(type == module.scalar.float_) return constant<ConstFloat>(source, type, F32(value));
    return constant<ConstDouble>(source, type, value);
}

/*
 * Reading a module-level name.
 *
 * A `let &` global is storage, so reading one is a load of its place exactly as a mutable local's
 * is. A plain one is not. Nothing in the program can assign to it - resolvePlace reports on any
 * attempt - so its value is forever the constant declareGlobal recorded, and the read is that
 * constant rather than a load of the bytes it would have been emitted as. Nothing then reads the
 * storage at all, so the global is not marked used and is not emitted: an immutable global is a
 * name for a constant and occupies nothing, which is what `let regionSize = 4194304 :: I64` should
 * cost and what makes it worth writing in place of a function returning the same number.
 *
 * Only a direct type folds. A memory type's value *is* its storage, and `initial` says only that
 * the storage starts zeroed, so for one of those the load stays.
 */
ModulePtr<Value> ExprResolver::globalValue(ModulePtr<Global> global_, LocationId source) {
    auto& definition = *local[global_];

    if(definition.mut || !isDirectType(global, definition.type)) {
        definition.used = true;
        return load(Place::inGlobal(global_), source);
    }

    return constantBits(definition.type, definition.initial, source);
}

// The constant a declared-once value holds, from the bits its storage would have held at the width
// of its own type - the form both a global's initializer and a field default are recorded in.
ModulePtr<Value> ExprResolver::constantBits(TypePtr type, U64 bits, LocationId source) {
    if(isFloat(global, type)) return makeFloat(source, type, floatFromBits(global, type, bits));

    // The resolve IR has no pointer immediate on purpose, so a pointer constant is its address as
    // an integer reinterpreted - which is the same thing `null()` expands to.
    if(isPointer(global, type)) {
        auto address = makeInt(source, module.scalar.long_, bits);
        return ref(emit<InstUnary>(source, 0, type, Value::Cast, address));
    }

    return makeInt(source, type, bits);
}

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
    ModulePtr<Value> args[] = {
        integral ? makeInt(source, module.scalar.long_, ((ConstInt*)local[value])->value)
                 : makeFloat(source, module.scalar.double_, ((ConstDouble*)local[value])->value),
    };

    // Selected against the class directly rather than by the name it happens to have: which
    // function builds a literal is not something a module that defines its own `fromInt` gets to
    // answer, and R5 would otherwise let a plain function of that name take over every literal in
    // the module that wrote it.
    ClassFunRef reference { typeClass, global[typeClass]->functions.get(global, 0).name, 0 };
    ClassMatch match;

    if(matchClassFun(reference, { args, 1 }, target, match)) {
        if(match.instance) {
            if(local[match.instance]->functions.get(local, match.index)) {
                return emitInstanceCall(module, match.instance, toBuffer(match.instanceArgs), match.index,
                                        { args, 1 }, source);
            }
        } else if(isGeneric(global, target)) {
            // Inside a generic body the instance is the caller's to supply, exactly as it is for
            // any other class call the body's own type variables decide.
            return emitGenericDispatch(match, { args, 1 }, source, 0);
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
    ModulePtr<Value> args[] = { value };

    if(matchClassFun(reference, { args, 1 }, module.scalar.bool_, match)) {
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
            return emitGenericDispatch(match, { args, 1 }, source, 0);
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

        ModulePtr<Value> args[] = { value };

        ClassMatch match;
        if(!matchClassFun(candidate, { args, 1 }, target, match) || !match.instance) continue;

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

            return ref(emit<InstBorrow>(source, 0, target, Place::inBorrow(value), false));
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

    auto canonical = canonicalType(global, from);
    if(canonical != canonicalType(global, target)) return nullptr;

    auto& wanted = *(IntType*)global[target];
    auto& held = *(IntType*)global[from];

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
        auto up = ref(emit<InstBinary>(source, 0, from, Value::Shl, value,
                                       makeInt(source, from, distance)));
        auto down = ref(emit<InstBinary>(source, 0, from, Value::Sar, up,
                                         makeInt(source, from, distance)));

        return ref(emit<InstUnary>(source, local[value]->name, target, Value::Cast, down));
    }

    auto mask = wanted.bits >= 64 ? maxLimit<U64> : (U64(1) << wanted.bits) - 1;
    auto masked = ref(emit<InstBinary>(source, 0, from, Value::And, value,
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
    auto instruction = create<InstNative>(source, 0, module.scalar.size, NativeOp::HostField,
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
        items = ref(emit<InstUnary>(source, 0, pointer, Value::Cast, load(array, source)));
        count = makeInt(source, module.scalar.size, ((ArrayType*)global[from])->length);
    } else {
        auto held = projectField(array, context.addUnqualifiedName("items", 5), source, source);
        if(!held) return nullptr;

        items = load(held.unwrap(), source);
        count = hostArrayLength(items, source);
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
ModulePtr<Value> ExprResolver::convertSlice(ModulePtr<Value> value, TypePtr from, TypePtr target,
                                            LocationId source, bool mut) {
    auto element = sliceElement(module, target);
    auto fixed = fixedElement(module, from);
    if(!element || (!fixed && !arrayElement(module, from))) return nullptr;

    /*
     * A third owner, and it borrows like the second - Implementation-Containers.md §7.
     *
     * An `@inline(n) @capacity(n)` array holds its slots the way a `[T *n]` does, so the base is an
     * address computation rather than a load; what it does *not* share with the fixed array is the
     * length, which is stored here because the array grows within its bound. So this arm is one half
     * of each of the two below it, and no third descriptor shape exists - which is §1's whole point,
     * that what varies is the owner and never the borrow.
     */
    auto inlineOwner = inlineRefinement(module, from);
    if(inlineOwner && arrayElement(module, from) != element) return nullptr;

    // Two containers, one descriptor. A `[T *n]` whose element type is not the slice's is not a
    // borrow of it at all, which the growable side gets for free - `Array(T)` and `Flat(U)` never
    // reach here because instantiateRecord already made them different types - and which this side
    // has to say, since `[Int *4]` and `Flat(Long)` are two unrelated types the ladder is walking.
    if(fixed && fixed != element) return nullptr;

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
     */
    auto items = fixed
        ? fixedArrayBase(array, element, source)
        : (inlineOwner ? fixedArrayBase(slots.unwrap(), element, source) : load(base.unwrap(), source));

    initialize(project(slice, ProjectionKind::Field, 0), items, source);

    auto count = fixed
        ? makeInt(source, module.scalar.long_, ((ArrayType*)global[from])->length)
        : load(length.unwrap(), source);

    if(auto declared = sliceLengthType(module, target)) count = convert(count, declared, source, false);

    initialize(project(slice, ProjectionKind::Field, 1), count, source);

    return storage;
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

    if(auto sliced = convertSlice(value, from, target, source)) return sliced;

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

        if(contiguous && sameType(contiguous, element)) {
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

    // A narrowing conversion exists but has to be asked for. Asking the instance table rather
    // than building the conversion first keeps the diagnostic about precision instead of about an
    // instance the author never mentioned, and leaves no half-built conversion behind.
    TypePtr pair[] = { from, target };

    if(findInstance(module, module.coreClasses.narrow, { pair, 2 })) {
        if(!implicit) {
            return emitConversion(module.coreClasses.narrow, context.addUnqualifiedName("narrow", 6),
                                  value, target, source);
        }

        context.diagnostics.error("implicit conversion from %@ to %@ would lose precision"_v, source,
                                  describeType(context, global, from), describeType(context, global, target));
        return value;
    }

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

// The same question convert() answers, asked without answering it. Overload selection has to know
// whether a candidate accepts an argument before it commits to that candidate, and convert()
// cannot be used for that: reporting the mismatch is its job, and a candidate that does not fit is
// not an error while another member of the overload set may still serve the call.
bool ExprResolver::convertible(ModulePtr<Value> value, TypePtr target, LocationId source) {
    if(!value || !target) return false;

    auto from = valueType(value);
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
    if(canonicalType(global, from) == canonicalType(global, target) &&
       global[from]->kind == Type::Int) {
        return true;
    }

    TypePtr args[] = { from, target };
    return findInstance(module, module.coreClasses.widen, { args, 2 }) != nullptr;
}

ModulePtr<Value> ExprResolver::finishBranches(BranchArmList& arms, LocationId source, bool used) {
    // Every arm that diverged - returned, or broke out of a loop - left no block behind. If none
    // of them did leave one, the expression as a whole never completes and there is no join.
    if(arms.isEmpty()) {
        current = nullptr;
        return nullptr;
    }

    // An arm with no value is one that could not produce one (a missing `else`, or an error
    // already reported); it makes the whole expression valueless rather than the phi partial.
    auto values = used;
    TypePtr resultType = nullptr;

    for(auto& arm: arms) {
        if(!values) break;
        if(!arm.value) {
            values = false;
            break;
        }

        // An arm that is a bare literal has no type of its own to join with; it takes the default
        // its class names, and the widening below then does what it would for any other pair. The
        // value itself is built in the arm's own block by the conversion loop underneath.
        auto type = settleType(valueType(arm.value));

        if(!type) {
            context.diagnostics.error("nothing decides the type of this literal, and its class has no default"_v,
                                      arm.source);
            values = false;
        } else if(!resultType) {
            resultType = type;
        } else if(global[type]->kind == Type::Error || global[resultType]->kind == Type::Error) {
            // One arm is already broken and said so. What its type disagrees with is not a second
            // fact about this expression.
            resultType = module.scalar.error;
        } else if(!sameType(resultType, type)) {
            if(auto common = commonWiden(resultType, type)) {
                resultType = common;
            } else {
                context.diagnostics.error("branches of this expression have different types"_v, arm.source);
                values = false;
            }
        }
    }

    auto join = addBlock();

    // Each arm's conversion goes at the end of that arm's own block: a phi input has to already
    // have the phi's type in the block it comes from, and the type to convert to is only known
    // once every arm has been seen.
    for(auto& arm: arms) {
        current = arm.end;
        if(values) arm.value = convert(arm.value, resultType, arm.source);
        terminate(emit<InstJmp>(arm.source, 0, module.scalar.unit, join));
    }

    current = join;
    if(!values) return nullptr;
    if(arms.size() == 1) return arms[0].value;

    auto phi = create<InstPhi>(source, 0, resultType);
    for(auto& arm: arms) phi->inputs.push(module.arena, PhiInput { arm.end, arm.value });
    append(phi);

    auto result = ref(phi);
    if(isMemoryType(global, resultType)) function.addLocal(module, resultType, 0, result);

    return result;
}

ModulePtr<Value> ExprResolver::resolveIf(const ast::Expr& expr, const ast::IfExpr& branch, TypePtr target, bool used, bool implicit) {
    auto bindingCount = bindings.size();
    ModulePtr<Block> elseBlock = nullptr;

    // The condition leaves `current` at the block where it held, which is where an `is` test's
    // bindings are live - so the `then` arm is resolved with them in scope and the resize below
    // takes them away again, exactly as the arms of a `match` scope what their patterns bind.
    if(resolveCondition(branch.cond, elseBlock) == PatternResult::Never) return nullptr;

    BranchArmList arms;

    auto thenValue = resolve(branch.then, target, used, implicit);
    if(current) arms.push(BranchArm { current, thenValue, branch.then.source });
    bindings.resize(bindingCount);

    current = elseBlock;
    ModulePtr<Value> elseValue = nullptr;
    auto elseSource = expr.source;

    if(branch.otherwise) {
        elseValue = resolve(branch.otherwise.unwrap(), target, used, implicit);
        elseSource = branch.otherwise.unwrap().source;
    } else if(used) {
        context.diagnostics.error("value-producing if requires an else branch"_v, expr.source);
    }

    if(current) arms.push(BranchArm { current, elseValue, elseSource });
    bindings.resize(bindingCount);

    return finishBranches(arms, expr.source, used);
}

ModulePtr<Value> ExprResolver::resolveMultiIf(const ast::Expr& expr, ast::ParseList<ast::IfCase> cases, TypePtr target, bool used, bool implicit) {
    auto contents = cases.contents(parse);
    if(contents.size() == 0) return nullptr;

    auto bindingCount = bindings.size();
    BranchArmList arms;
    auto hasElse = false;

    for(Size i = 0; i < contents.size() && current; i++) {
        // The parser writes a trailing `_`/`else` case as a `True` literal condition, so an
        // always-taken final case is recognized here rather than being tested at runtime.
        auto isElse = i + 1 == contents.size() &&
                      ast::isLiteral(contents[i].cond) &&
                      ast::Literal::Kind(contents[i].cond.kind - ast::Expr::Lit) == ast::Literal::Bool &&
                      contents[i].cond.lit.b;

        ModulePtr<Block> nextBlock = nullptr;

        if(isElse) {
            hasElse = true;
        } else if(resolveCondition(contents[i].cond, nextBlock) == PatternResult::Never) {
            return nullptr;
        }

        auto value = resolve(contents[i].then, target, used, implicit);
        if(current) arms.push(BranchArm { current, value, contents[i].then.source });
        bindings.resize(bindingCount);

        current = nextBlock;
    }

    // Without an else case, control can fall out of the last test having produced nothing.
    if(current) {
        if(used) context.diagnostics.error("value-producing multi-if requires an else case"_v, expr.source);
        arms.push(BranchArm { current, nullptr, expr.source });
    }

    return finishBranches(arms, expr.source, used && hasElse);
}

void ExprResolver::resolveWhile(const ast::WhileExpr& loop) {
    auto conditionBlock = addBlock();

    // The exit block is made here rather than left to the condition, because `break` targets it
    // and the body is resolved before anything else refers to it.
    auto exitBlock = addBlock();

    terminate(emit<InstJmp>(loop.cond.source, 0, module.scalar.unit, conditionBlock));

    // A name the body binds belongs to the body, the way it does in the arms of an `if` or a
    // `match`. Letting one outlive the loop would also let it be read from the exit block, which
    // the value it was bound to does not dominate - the loop may have run zero times. The names an
    // `is` condition binds are in the same position and are scoped by the same resize: they are
    // live in the body, which is exactly where the pattern matched.
    auto bindingCount = bindings.size();

    current = conditionBlock;
    if(resolveCondition(loop.cond, exitBlock) == PatternResult::Never) {
        current = exitBlock;
        return;
    }

    loops.push(LoopTarget { conditionBlock, exitBlock });
    resolve(loop.body, nullptr, false);
    loops.pop();

    bindings.resize(bindingCount);

    if(current) terminate(emit<InstJmp>(loop.body.source, 0, module.scalar.unit, conditionBlock));
    current = exitBlock;
}

/*
 * `for pat in a .. b [step s]: body`, and its `..=` and `downto` spellings - Design.md's
 * Expressions.
 *
 * A counted loop, and nothing to do with the iterator form beside it: no continuation is lifted and
 * nothing is handed over. What the three spellings decide is one interval and one direction:
 *
 *  - `a .. b`  walks `[a, b)` upward   - the half-open interval, which is the one that composes;
 *  - `a ..= b` walks `[a, b]` upward   - for a bound that is a real member, `0 ..= 255` on a `U8`;
 *  - `a downto b` walks `[b, a)` *downward*, so `n downto 0` is `0 .. n` reversed exactly.
 *
 * `downto` excluding the bound written first is the one surprising part, and it is what makes
 * reversing a loop a one-token edit rather than an arithmetic one. The alternative - both ends
 * inclusive, as in Pascal - makes the reversal of `0 .. n` read `n - 1 downto 0`, and on an unsigned
 * counter with `n == 0` that subtraction wraps to the top of the type and the loop runs forever.
 * The rule to remember is that an interval is always `[low, high)` and the two forms differ only in
 * which end they start from.
 *
 * Every test below is written so that no bound can overflow, which is the whole reason the loop is
 * built here rather than desugared into source. The distance to the far end is what decides whether
 * to step again - `to - i` and `i - to` are computed on the side of the comparison that has already
 * been proved non-negative - so a counter that ends at the top of its type stops rather than
 * wrapping past it.
 */
void ExprResolver::resolveCountedFor(const ast::Expr& expr, const ast::ForExpr& loop) {
    auto source = expr.source;
    auto ascending = !loop.reverse;

    /*
     * The counter's type, decided by the two bounds together.
     *
     * Both are resolved without a target, because neither is more authoritative than the other:
     * `for i in 0 .. xs.length` has the literal take the length's type, and `for i in first .. 10`
     * has it the other way round. Two literals settle to their own default, which is what an
     * ordinary `let` of one would do.
     */
    auto fromValue = resolve(loop.from, nullptr);
    auto toValue = resolve(*parse[loop.to], nullptr);
    if(!fromValue || !toValue) return;

    auto fromLiteral = isLiteral(global, valueType(fromValue));
    auto toLiteral = isLiteral(global, valueType(toValue));

    if(fromLiteral && !toLiteral) {
        fromValue = convert(fromValue, valueType(toValue), loop.from.source);
    } else if(toLiteral && !fromLiteral) {
        toValue = convert(toValue, valueType(fromValue), parse[loop.to]->source);
    } else {
        fromValue = settle(fromValue, loop.from.source);
        toValue = convert(settle(toValue, parse[loop.to]->source), valueType(fromValue),
                          parse[loop.to]->source);
    }

    if(!fromValue || !toValue) return;
    auto counterType = valueType(fromValue);

    // The step, at the counter's type. A step of zero would never reach the far end, and a written
    // one is worth rejecting where it can be seen rather than leaving as a loop that does not stop.
    if(loop.step && ast::isLiteral(*parse[loop.step]) && parse[loop.step]->lit.i() == 0) {
        context.diagnostics.error("a `for` step of zero never reaches the end of its range"_v,
                                  parse[loop.step]->source);
        return;
    }

    auto stepValue = loop.step ? resolve(*parse[loop.step], counterType) : makeInt(source, counterType, 1);
    if(!stepValue) return;

    stepValue = convert(stepValue, counterType, source);

    /*
     * The blocks, created in the order the block list has to hold them.
     *
     * That order is the whole of why this is built by hand: `resolve/lower.cpp` walks blocks in list
     * order and requires every operand to have been lowered already, and `compiler/opt`'s inliner
     * splices and re-lays lists assuming the same. So the loop is laid out
     *
     *     [guards] condition  body...  advance  step  exit
     *
     * which is a reverse postorder: every edge runs forward down that list except the one back edge
     * from the step to the condition. The condition is the loop header and the only way into the
     * cycle, which is also what keeps the loop reducible for the passes that read dominance.
     *
     * The cost is that `exit` does not exist while the body is being resolved, so a `break` cannot
     * jump to it and neither can a guard that decides the loop runs no times. Both are collected and
     * terminated at the end instead - see LoopTarget, and finishContinuationExits for the same
     * pattern applied to a `return` inside a lifted continuation.
     */
    struct PendingBranch {
        ModulePtr<Block> block;
        ModulePtr<Value> condition;
        ModulePtr<Block> taken;
    };

    Array<PendingBranch> pending;
    Array<ModulePtr<Block>> breaks;
    Array<ModulePtr<Block>> continues;

    ModulePtr<Block> ordered = nullptr;
    ModulePtr<Block> reachable = nullptr;

    if(!ascending) {
        ordered = addBlock();
        reachable = addBlock();
    }

    auto conditionBlock = addBlock();
    auto bodyBlock = addBlock();

    /*
     * A descending loop needs what an ascending one gets from its own condition: that the interval
     * is non-empty, and that it holds at least one step. Both are checked before the counter is
     * built, because the counter starts one step below the bound written first and that subtraction
     * has to be known not to wrap.
     */
    auto initial = fromValue;
    if(!ascending) {
        ModulePtr<Value> above[] = { fromValue, toValue };
        auto isAbove = emitCall(Context::nameHash(">", 1), { above, 2 }, source, module.scalar.bool_);
        if(!isAbove) return;

        pending.push(PendingBranch { current, convert(isAbove, module.scalar.bool_, source), ordered });

        current = ordered;
        ModulePtr<Value> span[] = { fromValue, toValue };
        auto distance = emitCall(Context::nameHash("-", 1), { span, 2 }, source, counterType);
        if(!distance) return;

        ModulePtr<Value> fits[] = { distance, stepValue };
        auto hasStep = emitCall(Context::nameHash(">=", 2), { fits, 2 }, source, module.scalar.bool_);
        if(!hasStep) return;

        pending.push(PendingBranch { current, convert(hasStep, module.scalar.bool_, source), reachable });

        current = reachable;

        ModulePtr<Value> back[] = { fromValue, stepValue };
        initial = emitCall(Context::nameHash("-", 1), { back, 2 }, source, counterType);
        if(!initial) return;
    }

    auto counter = allocate(counterType, source, 0, ast::BindType::Ref);
    initialize(placeFor(counter, source), convert(initial, counterType, source), source);
    terminate(emit<InstJmp>(source, 0, module.scalar.unit, conditionBlock));

    /*
     * The test that says whether this iteration runs at all.
     *
     * Ascending, it is the interval's own upper bound and is what makes an empty range run zero
     * times. Descending, the guards above already proved it, and it is emitted anyway so that the
     * cycle has one header rather than being entered at its body.
     */
    current = conditionBlock;
    auto value = load(placeFor(counter, source), source);

    auto compare = !ascending ? Context::nameHash(">=", 2)
                 : loop.inclusive ? Context::nameHash("<=", 2)
                 : Context::nameHash("<", 1);

    ModulePtr<Value> bound[] = { value, toValue };
    auto more = emitCall(compare, { bound, 2 }, source, module.scalar.bool_);
    if(!more) return;

    pending.push(PendingBranch { conditionBlock, convert(more, module.scalar.bool_, source), bodyBlock });

    // The body, with the counter bound. Scoped to the body the way a `while`'s bindings are: the
    // loop may run zero times, so the name does not dominate the code after it.
    auto bindingCount = bindings.size();

    current = bodyBlock;
    bindIrrefutable(loop.pat, value,
                    "a `for` loop has no alternative to take for an element it does not match"_v);

    loops.push(LoopTarget { nullptr, nullptr, &continues, &breaks });
    resolve(loop.body, nullptr, false);
    loops.pop();

    bindings.resize(bindingCount);

    auto tail = current;

    auto advanceBlock = addBlock();
    auto stepBlock = addBlock();
    auto exitBlock = addBlock();

    /*
     * The step, guarded by how far the counter still has to go.
     *
     * `to - i` ascending and `i - to` descending, each on the side the condition above has already
     * proved is the larger - so neither subtraction wraps, and comparing the distance against the
     * step is what stops a counter whose next value would leave the type rather than the range.
     * A closed range stops when the distance is *below* a step, a half-open one when it is at most
     * one, which is the whole of the difference between `..` and `..=` in the emitted code.
     */
    current = advanceBlock;
    auto atStep = load(placeFor(counter, source), source);

    ModulePtr<Value> remaining[] = { ascending ? toValue : atStep, ascending ? atStep : toValue };
    auto distance = emitCall(Context::nameHash("-", 1), { remaining, 2 }, source, counterType);
    if(!distance) return;

    auto exhausted = (ascending && !loop.inclusive) ? Context::nameHash("<=", 2)
                                                    : Context::nameHash("<", 1);

    ModulePtr<Value> left[] = { distance, stepValue };
    auto done = emitCall(exhausted, { left, 2 }, source, module.scalar.bool_);
    if(!done) return;

    terminate(emit<InstJe>(source, 0, module.scalar.unit, convert(done, module.scalar.bool_, source),
                           exitBlock, stepBlock));

    current = stepBlock;
    ModulePtr<Value> moved[] = { atStep, stepValue };
    auto next = emitCall(ascending ? Context::nameHash("+", 1) : Context::nameHash("-", 1),
                         { moved, 2 }, source, counterType);
    if(!next) return;

    assign(placeFor(counter, source), convert(next, counterType, source), source);
    terminate(emit<InstJmp>(source, 0, module.scalar.unit, conditionBlock));

    // Everything that was waiting for a block that did not exist yet. Each branch falls through to
    // the exit when its condition does not hold, which is one shape for the two guards and the
    // loop's own test alike.
    for(auto& branch: pending) {
        current = branch.block;
        terminate(emit<InstJe>(source, 0, module.scalar.unit, branch.condition, branch.taken, exitBlock));
    }

    for(auto block: continues) {
        current = block;
        terminate(emit<InstJmp>(source, 0, module.scalar.unit, advanceBlock));
    }

    for(auto block: breaks) {
        current = block;
        terminate(emit<InstJmp>(source, 0, module.scalar.unit, exitBlock));
    }

    if(tail) {
        current = tail;
        terminate(emit<InstJmp>(loop.body.source, 0, module.scalar.unit, advanceBlock));
    }

    current = exitBlock;
}

void ExprResolver::resolveReturn(const ast::Expr& expr) {
    if(inThunk) {
        // Returning from the enclosing function out of a `@lazy` argument is a non-local exit
        // across the callee's live frame, which is Analysis-Lens.md §5.1's exit signal - the
        // callee has cleanup that would have to run on the way past. Rejected rather than left to
        // mean "return from the thunk", which is what it would otherwise silently become.
        context.diagnostics.error("`return` inside a `@lazy` argument is not available yet - it would have to leave the function through the callee's frame, which needs the exit signal"_v,
                                  expr.source);
        return;
    }

    if(function.funKind == ast::FunKind::Iter) {
        // An iterator ends by running out of values, and what it hands back then is the step signal
        // rather than anything the body has a name for. A `return` in it would have to produce that
        // signal, which is not a type the declaration wrote or the author could.
        context.diagnostics.error("an `iter fn` ends by running out of values rather than by `return` - what it produces is the loop's own signal, which is not something the body names"_v,
                                  expr.source);
        return;
    }

    if(resultInferred) {
        // Nothing has decided what this lambda returns yet, and `return` cannot be the thing that
        // decides it: a later `return` of a different type would have nothing to be checked
        // against, and the two would silently disagree.
        context.diagnostics.error("this lambda's result type is decided by its body, so it cannot use `return` - write it where a function type is expected"_v,
                                  expr.source);
    }

    /*
     * The function this `return` leaves, which is not always the one it is written in.
     *
     * Inside a lens continuation the block was split out of some enclosing function, and Design.md's
     * Leaving through a lens says a `return` there leaves *that* function - past the lens's own
     * frame, which runs its cleanup on the way. So the type it is checked against is the enclosing
     * one's, and what the departure compiles to is decided later - see emitFunctionReturn.
     */
    auto declared = enclosingResultType();

    ModulePtr<Value> value = nullptr;
    if(expr.ret) value = resolve(*parse[expr.ret], declared);

    if(isUnit(global, declared)) {
        if(value) context.diagnostics.error("unit function cannot return a value"_v, expr.source);
        value = nullptr;
    } else if(!value) {
        context.diagnostics.error("non-unit function must return a value"_v, expr.source);
    } else {
        value = convert(value, declared, expr.source);
    }

    emitFunctionReturn(value, expr.source);
}

ModulePtr<Value> ExprResolver::resolveDecl(ast::ParseList<ast::VarDecl> declarations, TypePtr target, bool used) {
    ModulePtr<Value> result = nullptr;

    for(auto decl: declarations.contents(parse)) {
        if(!decl.content) {
            context.diagnostics.error("let requires an initializer"_v, decl.pat.source);
            continue;
        }

        auto mutable_ = decl.bind == ast::BindType::Ref;
        auto checkpoint = bindings.size();

        // Where this frame's locals had got to before the initializer ran, which is what tells a
        // temporary it built from storage the program already had a name for - see `adoptableLocal`.
        auto fresh = U32(function.localCount());

        // A `let` is a statement boundary, so a literal the initializer left open is settled to
        // its default here: `let x = 1` binds an Int, and nothing later in the block can go back
        // and make it a Long.
        auto value = settle(resolve(*parse[decl.content]), decl.pat.source);
        if(!value) continue;

        // `let ->z = x` takes ownership out of whatever `x` named, so the name that follows binds
        // the moved value rather than the source. The binding itself is an ordinary immutable one:
        // what `->` decides is where the value came from, not what may be done with it after.
        if(decl.bind == ast::BindType::Sink) {
            value = rootSink(sinkValue(value, decl.pat.source), decl.pat.source);
            if(!value) continue;
        }

        if(isBorrow(global, valueType(value))) {
            bindBorrow(decl, value, mutable_);
        } else if(mutable_) {
            bindMutable(decl, value, fresh);
        } else {
            resolveBinding(decl, value);
        }

        if(decl.attributes.isNotEmpty()) applyBindingAttributes(decl, value, checkpoint);

        if(!current) break;

        if(decl.in) {
            result = resolve(*parse[decl.in], target, used);
            bindings.resize(checkpoint);
        } else {
            result = value;
        }
    }

    return result;
}

/*
 * `let &x = value`.
 *
 * The initializer's storage is what the name refers to from here on, so the declaration allocates
 * a slot, writes the value into it, and binds the name to the slot rather than to the value. That
 * is the whole difference between a mutable and an immutable binding at this milestone: the same
 * places, the same InstInit, and one more entry in Function::locals.
 *
 * Only a plain name can be mutable. Destructuring one into several mutable slots is a question
 * about ownership - which of the parts the binding owns - and belongs with the rest of Milestone
 * 5, not with the machinery for writing to a slot.
 */
/*
 * Attributes on a binding.
 *
 * `@heap` is the only one so far, and it is Design.md's "for a large allocation that's freed well
 * before the region closes": an override of the storage class escape analysis would otherwise
 * choose. It is deliberately one-directional - it can only move a value off the frame, never onto
 * it - because the analysis picks the frame exactly when it has proved the frame is enough, and an
 * attribute that could overrule *that* would be an attribute that could introduce a dangling
 * reference.
 *
 * The slot it applies to is whichever local the binding's value ends up occupying: for a mutable
 * binding that is the slot the declaration allocated, and for an aggregate it is the storage the
 * construction already created.
 */
void ExprResolver::applyBindingAttributes(const ast::VarDecl& declaration, ModulePtr<Value> value,
                                          Size bindingBase) {
    auto slot = maxLimit<U32>;

    if(bindings.size() > bindingBase && bindings[bindingBase].local != maxLimit<U32>) {
        slot = bindings[bindingBase].local;
    } else if(auto place = findPlace(value)) {
        if(place.unwrap().root == PlaceRoot::Local) slot = place.unwrap().local;
    }

    auto attributes = declaration.attributes;

    for(auto attribute: attributes.contents(parse)) {
        if(attribute.name != context.addUnqualifiedName("heap", 4)) {
            context.diagnostics.error("unknown attribute %@ on a binding"_v, attribute.source,
                                      context.findName(attribute.name));
            continue;
        }

        if(attribute.args.isNotEmpty()) {
            context.diagnostics.error("`@heap` takes no arguments"_v, attribute.source);
            continue;
        }

        if(slot == maxLimit<U32>) {
            // A value in a register occupies no storage for an attribute to place. Saying so is
            // better than allocating one just so that the attribute has something to be about.
            context.diagnostics.error("`@heap` has nothing to place - this binding names a value that occupies no storage of its own"_v,
                                      attribute.source);
            continue;
        }

        auto local_ = function.localAt(local, slot);
        function.locals.set(local, slot, Local {
            local_.type, local_.name, local_.value, local_.convention, StorageClass::Heap,
            local_.borrowed, local_.closureEnv,
        });
    }
}

/*
 * `let entry = f(...)` and `let &entry = f(...)`, where what `f` returned is a borrow.
 *
 * The name refers to the storage the callee's return-root group named, so there is nothing to
 * allocate and nothing to copy: the binding is a place rooted in the borrow itself. Allocating a
 * slot and writing the borrow into it - which is what the ordinary path would do - would give the
 * name a *copy* of the reference, and `entry.field = value` would then write through to the right
 * storage by accident rather than by construction.
 *
 * The sigil still has to agree with what was handed over. `let &` on an immutable borrow would be a
 * name that claims a capability nobody granted it, and that is the one thing to report here rather
 * than at the first write through it.
 */
void ExprResolver::bindBorrow(const ast::VarDecl& declaration, ModulePtr<Value> value, bool mutable_) {
    if(declaration.pat.kind != ast::Pat::Var) {
        context.diagnostics.error("a binding of a borrow must be a single name - a borrow has no members to destructure"_v,
                                  declaration.pat.source);
        return;
    }

    auto borrow = (BorrowType*)global[valueType(value)];

    if(mutable_ && !borrow->mut) {
        context.diagnostics.error("cannot bind an immutable borrow with `let &` - the value it refers to may not be written through it"_v,
                                  declaration.pat.source);
        return;
    }

    Binding binding { declaration.pat.var, value, maxLimit<U32>, value };
    binding.definition = declaration.pat.source;
    bindings.push(binding);
    recordBindingDefinition(*this, binding);
}

Maybe<U32> ExprResolver::adoptableLocal(ModulePtr<Value> value, U32 fresh) {
    auto found = findPlace(value);
    if(!found) return Nothing();

    // A whole local and not a part of one. A field of something is storage whose owner outlives this
    // binding, and there is nothing to take over.
    auto place = found.unwrap();
    if(place.root != PlaceRoot::Local || place.projections.isNotEmpty()) return Nothing();
    if(place.local < fresh || place.local >= function.localCount()) return Nothing();

    /*
     * Storage this frame allocated, and only that. A `&` parameter's slot is the caller's, a closure
     * environment is the function value's, and a materialized packed-field temporary stands for
     * storage somewhere else - none of the three is a temporary to be taken over, and each of them
     * is already recorded on the slot rather than having to be worked out.
     */
    auto slot = function.localAt(local, place.local);
    if(!slot.value || local[slot.value]->kind != Value::Alloc) return Nothing();
    if(slot.borrowed || slot.closureEnv || slot.materialized) return Nothing();
    if(slot.type != valueType(value)) return Nothing();

    // And nothing already answers to it. The index test above covers a name the program had before
    // this declaration; this covers one the initializer itself introduced, which a `let ... in`
    // inside it can do.
    for(auto& binding: bindings) {
        if(binding.local == place.local) return Nothing();
    }

    return Just(place.local);
}

void ExprResolver::bindMutable(const ast::VarDecl& declaration, ModulePtr<Value> value, U32 fresh) {
    if(declaration.pat.kind != ast::Pat::Var) {
        context.diagnostics.error("a mutable binding must be a single name"_v, declaration.pat.source);
        return;
    }

    auto alternatives = declaration.alts;
    if(alternatives.isNotEmpty()) {
        context.diagnostics.error("a mutable binding always matches, so it takes no alternatives"_v,
                                  declaration.pat.source);
    }

    auto name = declaration.pat.var;

    /*
     * The temporary the initializer built, taken over rather than copied out of - see
     * `adoptableLocal`, which is where the conditions and the reasoning live.
     *
     * Read-modify-write on the slot rather than a fresh `Local`, because a local carries more than
     * the four fields this changes and the ones it does not name are set after `addLocal` rather
     * than by it. What differs about a mutable binding's slot is its name and its convention.
     */
    if(auto adopted = adoptableLocal(value, fresh)) {
        auto index = adopted.unwrap();
        auto slot = function.localAt(local, index);

        slot.name = name;
        slot.convention = ast::BindType::Ref;
        function.locals.set(local, index, slot);

        Binding adoptedBinding { name, slot.value, index };
        adoptedBinding.definition = declaration.pat.source;
        bindings.push(adoptedBinding);
        recordBindingDefinition(*this, adoptedBinding);
        return;
    }

    auto type = valueType(value);
    auto storage = allocate(type, declaration.pat.source, name, ast::BindType::Ref);
    auto place = placeFor(storage, declaration.pat.source);

    initialize(place, value, declaration.pat.source);

    Binding binding { name, storage, place.local };
    binding.definition = declaration.pat.source;
    bindings.push(binding);
    recordBindingDefinition(*this, binding);
}

/*
 * What an assignment writes to.
 *
 * Four expressions name storage: a mutable binding, a mutable global, the memory a raw pointer
 * points at, and - only as the target of a field selection - an immutable binding holding a raw
 * pointer. Everything reachable from those by projection does too, which is what makes `p.x = 1`
 * and `(*node).next = null` work without a rule of their own - the projection path is built by the
 * same field selection an ordinary read uses.
 *
 * `through` is what marks that fourth case: writing *through* a pointer is not writing to the
 * binding that holds it, and the memory a pointer names is always mutable. `let n = ...` followed
 * by `n.value = 5` therefore writes, while `n = q` on the same binding stays the error it is -
 * that one rebinds the pointer rather than writing through it.
 */
Maybe<Place> ExprResolver::resolvePlace(const ast::Expr& astExpr, bool through) {
    auto& expr = unwrapNested(astExpr);

    switch(expr.kind) {
        case ast::Expr::Var: {
            if(auto binding = findBinding(expr.var, expr.source)) {
                if(binding->lazy) {
                    context.diagnostics.error("%@ is a `@lazy` parameter, which names an expression rather than storage - there is nothing to assign to"_v,
                                              expr.source, context.findName(expr.var));
                    return Nothing();
                }

                if(!binding->isPlace()) {
                    // An immutable binding still roots a place when what it holds is a reference:
                    // projecting into it names the storage the reference points at, which is not
                    // this binding's to be mutable about. A raw pointer and a borrow differ here
                    // only in whether anything checked the result.
                    if(through && isPointer(global, valueType(binding->value))) {
                        return Just(Place::atPointer(binding->value));
                    }

                    if(isBorrow(global, valueType(binding->value))) {
                        return Just(Place::inBorrow(binding->value));
                    }

                    context.diagnostics.error("%@ is not mutable - declare it with `let &` to assign to it"_v,
                                              expr.source, context.findName(expr.var));
                    return Nothing();
                }

                /*
                 * A capture the closure owns is not assignable.
                 *
                 * Design-Memory §8 requires a written capture to be by reference, and a capture
                 * that came out by value is exactly one whose enclosing binding was not mutable -
                 * so writing it would write the environment's own copy and the enclosing frame
                 * would never see it. That is the same diagnostic an immutable binding gets,
                 * because it is the same mistake.
                 */
                if(binding->captured && !binding->captureBorrow) {
                    context.diagnostics.error("%@ is captured by value and cannot be assigned to - declare it with `let &` in the enclosing function to capture it by reference"_v,
                                              expr.source, context.findName(expr.var));
                    return Nothing();
                }

                return Just(placeOf(*binding, expr.source));
            }

            if(auto global_ = findGlobal(module, expr.var, expr.source)) {
                if(!local[global_]->mut) {
                    context.diagnostics.error("%@ is not mutable - declare it with `let &` to assign to it"_v,
                                              expr.source, context.findName(expr.var));
                    return Nothing();
                }

                local[global_]->used = true;
                return Just(Place::inGlobal(global_));
            }

            context.diagnostics.error("unknown value %@"_v, expr.source, context.findName(expr.var));
            return Nothing();
        }
        case ast::Expr::Field: {
            auto& field = *parse[expr.field];
            auto target = resolvePlace(field.target, true);
            if(!target) return Nothing();

            return projectField(target.unwrap(), field.field, expr.source);
        }
        case ast::Expr::Sub: {
            // `xs[i] = value`. The mutable accessor hands back a borrow of the element, and the
            // assignment writes through it - which is also what keeps the array exclusively
            // borrowed for as long as the write is in progress.
            auto borrowed = resolveSubscript(expr, *parse[expr.sub], true);
            if(!borrowed) return Nothing();

            return Just(Place::inBorrow(borrowed));
        }
        case ast::Expr::Prefix: {
            // `*p = value` - the one place expression whose root the compiler knows nothing
            // about, which is the point of it.
            auto& prefix = *parse[expr.prefix];
            if(prefix.op.kind != ast::Expr::Var || prefix.op.var != Context::nameHash("*"_v)) break;

            auto pointer = resolve(prefix.on);
            if(!pointer) return Nothing();

            if(!isPointer(global, valueType(pointer))) {
                context.diagnostics.error("cannot dereference %@ - it is not a raw pointer"_v, expr.source,
                                          describeType(context, global, valueType(pointer)));
                return Nothing();
            }

            return Just(Place::atPointer(pointer));
        }
        default:
            break;
    }

    context.diagnostics.error("this expression does not name storage that can be assigned to"_v, expr.source);
    return Nothing();
}

ModulePtr<Value> ExprResolver::resolveAssign(const ast::Expr& expr, const ast::AssignExpr& assignment) {
    auto place = resolvePlace(assignment.target);
    if(!place) return nullptr;

    auto type = placeType(place.unwrap());
    auto value = resolve(assignment.value, type);
    if(!value) return nullptr;

    if(!isMemoryType(global, type)) value = convert(value, type, expr.source);

    // An assignment overwrites whatever the place held, which is what obliges the drop pass to
    // release the old value here rather than at the end of the binding's life.
    assign(place.unwrap(), value, expr.source);
    return nullptr;
}

// An integer-syntax literal can resolve to either kind of number, so a floating target takes it
// as a float constant rather than as an Int that is then converted. Any other concrete target is
// an ordinary FromInt instance - which is how a literal reaches a user type - and no target at
// all leaves a literal variable behind for the surrounding expression to decide.
ModulePtr<Value> ExprResolver::resolveInteger(LocationId source, TypePtr target, U64 value) {
    if(target && isFloat(global, target)) return makeFloat(source, target, F64(value));

    if(target && isInteger(global, target)) {
        checkLiteralRange(source, target, value);
        return makeInt(source, target, value);
    }

    auto literal = constant<ConstInt>(source, literalVariable(module.coreClasses.fromInt), value);
    return target ? materializeLiteral(literal, target, source) : literal;
}

// Decimal syntax means FromDecimal, which no integer type has an instance of - that is what makes
// `1.5 :: Int` a missing instance rather than a lossy conversion. The parser keeps every decimal
// literal at F64 precision until a type is picked here.
ModulePtr<Value> ExprResolver::resolveDecimal(LocationId source, TypePtr target, F64 value) {
    if(target && isFloat(global, target)) return makeFloat(source, target, value);

    auto literal = constant<ConstDouble>(source, literalVariable(module.coreClasses.fromDecimal), value);
    return target ? materializeLiteral(literal, target, source) : literal;
}

/*
 * A string literal - Implementation-String.md part 9, which is the one point a `String` is authored
 * rather than built up through the API.
 *
 * The two targets diverge completely here and share nothing but the decoded bytes, which is the
 * honest shape of "one logical value, two Repr-driven encodings":
 *
 *  - **JS**: the literal is a host string, and the only thing that produces one is a constant in the
 *    emitted source. One value kind, no storage, no descriptor - see ConstString.
 *  - **native**: the bytes go into the module's data as an ordinary global, and the value is the two
 *    words describing them. `runBorrowed` is what makes that free: the run does not own its slots,
 *    so a literal costs no teardown, and `resize` relocates a borrowed run by copying rather than
 *    refusing - which is copy-on-write, reached through Implementation-Containers.md §2's existing
 *    answers rather than a fourth one.
 *
 * The lexer has already decoded every escape and interned the result as UTF-8, so there is no
 * encoding work left here on either side. On native that is the target's native unit already; on JS
 * the emitter re-encodes it into a source literal and the host owns the UTF-16 from there.
 */
ModulePtr<Value> ExprResolver::resolveString(LocationId source, StringId text) {
    if(isJsMode(context.settings.mode)) {
        return constant<ConstString>(source, module.scalar.string_, text);
    }

    auto content = context.findName(text);

    if(!module.program.stringLiteral) {
        context.diagnostics.error("internal: no string literal constructor for this target"_v, source);
        return nullptr;
    }

    /*
     * The bytes, as a global of their own.
     *
     * Named per literal rather than interned by content. Two identical literals therefore get two
     * globals, which costs the bytes twice and is deliberately left alone: deduplicating them is a
     * size optimization over a table keyed on content, and doing it here would mean a name that
     * depends on the bytes - so a literal containing a quote or a newline would have to be escaped
     * into an identifier, which is a decision better made once, later, in one place.
     */
    /*
     * The bytes, as a global of their own, named by position rather than by content.
     *
     * The counter is what makes two literals two globals. Interning them by content instead would
     * save the bytes of a repeated literal, and is deliberately not done here: the name would then
     * have to be derived from the content, so a literal containing a quote or a newline would need
     * escaping into an identifier - a decision worth making once, later, in one place, rather than
     * as a side effect of emitting the first one.
     */
    StringBuilder name;
    name << "string$";
    name.appendValue(module.stringLiteralCount++);

    auto size = content.size();
    auto bytes = module.addGlobal(builtName(context, name), source);
    bytes->type = module.scalar.string_;
    bytes->literalBytes = ByteBuffer((Byte*)module.arena.alloc(size), size);
    copy((const Byte*)content.text(), bytes->literalBytes.ptr, size);
    bytes->used = true;

    auto constructor = module.program.stringLiteral;
    auto local = *module.arena;
    local[constructor]->used = true;

    // `stringLiteral` takes a `%U8`, and what it is handed is the address of a blob - so the
    // pointee type comes from the callee's own signature rather than being built here. That keeps
    // this correct if the unit ever stops being a byte, which is what part 2's table leaves open.
    auto byteType = local[local[constructor]->args.get(local, 0)]->type;
    auto address = ref(emit<InstSymbol>(source, 0, byteType, nullptr, bytes - local));
    auto length = makeInt(source, module.scalar.int_, size);

    auto call = create<InstCall>(source, 0, module.scalar.string_, constructor);
    call->args.push(module.arena, address);
    call->args.push(module.arena, length);
    append(call);

    return ref(call);
}

/*
 * `"a{x}b{y}c"` - Implementation-Storage.md part 8.
 *
 * The parser already produced the chunks; what happens here is the document's three steps, and the
 * design's whole trick is that they produce **one allocation whose extent is an ordinary value**
 * rather than three code paths:
 *
 *  1. the literal segments are known now, so their total `L` is a constant;
 *  2. each hole contributes `showBound(v)`, read through `formatBound` so that `Nothing` is zero;
 *  3. `newStringOfCapacity(L + Σ)` , then the literals and the holes appended in order.
 *
 * The three strategies are what the *existing* passes then make of that one allocation, which is why
 * none of them appears here:
 *
 *  **(a)** every bound is a constant `Just`, so the sum folds to a literal, the run's extent is a
 *  constant, and escape analysis puts a non-escaping format on the frame with no allocator call
 *  anywhere. This is the case the class's shape was designed for, and it needs the specializer to
 *  inline `showBound` and the folder to reduce what is left - both of which run.
 *
 *  **(b)** the sum is a runtime value. The allocation is the same instruction with a computed
 *  extent, and where it lives is `selectStorage`'s answer.
 *
 *  **(c)** some bound is `Nothing`. `formatBound` answers zero, so the seed covers the literals and
 *  the bounds that *are* known, and the appends grow past it through `reserveString`. A format that
 *  does not escape still starts on the frame and migrates only if it overflows.
 *
 * What is *not* here, and is part 8's own open question: the guarded `alloca`/heap pair strategy (b)
 * asks for, with the not-in-a-loop rule. `selectStorage` gives a computed extent the conservative
 * heap answer today - the same answer Implementation-Containers.md §12 records for every other
 * container - so (b) is correct and pays for the heap where it could sometimes have used the frame.
 * That is a placement decision shared with every container rather than something formatting can fix
 * on its own, which is why it is left where the rest of §12 is.
 */
ModulePtr<Value> ExprResolver::resolveFormat(const ast::Expr& expr) {
    auto& program = module.program;

    if(!program.newString || !program.pushString || !program.formatBound || !program.coreClasses.show) {
        context.diagnostics.error("internal: string formatting is unavailable in this build"_v, expr.source);
        return nullptr;
    }

    struct Hole {
        ModulePtr<Value> value = nullptr;
        TypePtr type = nullptr;
        StringId text = 0;
        bool hasText = false;
    };

    SmallArray<Hole, 8> holes;
    U64 literalUnits = 0;

    /*
     * Every hole resolved before anything is measured, and that ordering is the contract rather than
     * convenience: the arguments run left to right exactly once, and both `showBound` and `show` then
     * read the same value. Resolving a hole twice would run its expression twice.
     */
    auto chunks = expr.format;
    for(auto chunk: chunks.contents(parse)) {
        Hole hole;

        if(chunk.string) {
            hole.text = chunk.string;
            hole.hasText = true;
            literalUnits += context.findName(chunk.string).size();
        }

        if(chunk.format) {
            hole.value = resolve(*parse[chunk.format], nullptr, true);
            if(!hole.value) return nullptr;

            hole.value = settle(hole.value, expr.source);
            if(!hole.value) return nullptr;

            hole.type = valueType(hole.value);
        }

        holes.push(hole);
    }

    // Step 3's constant half. Runtime bounds are added to it below, and where there are none this is
    // the whole extent and folds straight into the allocation.
    auto total = makeInt(expr.source, module.scalar.int_, literalUnits);

    for(auto& hole: holes) {
        if(!hole.value) continue;

        auto bound = instanceMember(module, program.coreClasses.show, hole.type, 1, expr.source);
        if(!bound) {
            context.diagnostics.error("cannot format a value of type %@ - it has no instance of `Show`, so there is nothing that says what its text is"_v,
                                      expr.source, describeType(context, global, hole.type));
            return nullptr;
        }

        auto measure = create<InstCall>(expr.source, 0, (*module.arena)[bound]->returnType, bound);
        measure->args.push(module.arena, hole.value);
        append(measure);

        auto units = create<InstCall>(expr.source, 0, module.scalar.int_, program.formatBound);
        units->args.push(module.arena, ref(measure));
        append(units);
        (*module.arena)[program.formatBound]->used = true;

        total = ref(emit<InstBinary>(expr.source, 0, module.scalar.int_, Value::Add, total, ref(units)));
    }

    // The sink. One allocation, whose extent is whatever the sum turned out to be - see above.
    auto sizeType = (*module.arena)[(*module.arena)[program.newString]->args.get(*module.arena, 0)]->type;
    auto extent = convert(total, sizeType, expr.source);
    if(!extent) return nullptr;

    (*module.arena)[program.newString]->used = true;
    auto sink = create<InstCall>(expr.source, 0, module.scalar.string_, program.newString);
    sink->args.push(module.arena, extent);
    append(sink);

    sink->local = function.addLocal(module, sink->type, 0, ref(sink));

    /*
     * The sink's own storage, which is exactly what `let &sink = newStringOfCapacity(n)` compiles to
     * and is written out here for the same reason that line would have been.
     *
     * Two things need it, and borrowing the call's result directly satisfies neither. The appends
     * take a `&`, and a borrow is writable only where the place it names is - a call result's local
     * is not declared mutable. And on JS a `&` of a non-object is the `{$o, $k, $s}` triple
     * (Implementation-Containers.md §14.1), which needs a *box* to point into: a host string is a
     * primitive, so `sink[$k] = ...` against a bare one throws rather than writing. A `Ref`-convention
     * allocation is what makes the backend produce that box, and it is why this is an allocation and
     * an initialization rather than one instruction fewer.
     *
     * The copy is a temporary's, so the optimizer removes it wherever it can adopt the storage - the
     * same path an array literal's run takes.
     */
    auto storage = allocate(module.scalar.string_, expr.source, 0, ast::BindType::Ref);
    if(!storage) return nullptr;

    initialize(placeFor(storage, expr.source), ref(sink), expr.source);
    auto sinkValue = storage;

    /*
     * Appending, in written order. A `&` argument is a borrow of the sink's own storage, which is
     * what lets every one of these write into the buffer that was just sized for them.
     *
     * `sinkFirst` is not a detail: `pushString(&self: String, other: String)` takes the sink first
     * and `show(value: a, &to: String)` takes it second, and pushing the two in one order for both
     * produced a call whose arguments were swapped. The types differ, so it was caught - by the lower
     * IR validator rather than the resolver, because a `&` argument is an address at that level and
     * both positions are addresses at this one.
     */
    auto appendTo = [&](ModulePtr<Function> callee, ModulePtr<Value> argument, bool sinkFirst) {
        auto borrowed = borrowArgument(sinkValue, module.scalar.string_, expr.source, false);
        if(!borrowed) return false;

        (*module.arena)[callee]->used = true;
        auto call = create<InstCall>(expr.source, 0, module.scalar.unit, callee);

        if(sinkFirst) {
            call->args.push(module.arena, borrowed);
            call->args.push(module.arena, argument);
        } else {
            call->args.push(module.arena, argument);
            call->args.push(module.arena, borrowed);
        }

        append(call);
        return true;
    };

    /*
     * The hole first and the literal second, which is the order the parser records rather than the
     * order the two are written in.
     *
     * `parseStringExpr` opens with `{leading text, no expression}` and then pushes one chunk per
     * hole holding *that hole's expression and the text following it*. So a chunk is "this value,
     * then this text", and appending a chunk's text before its value renders `"n={7}!"` as `n=!7` -
     * which is a wrong string of the right length, so a fixture that checked only `length` would
     * have passed. `Format.yana` reads the units back for exactly this reason.
     */
    for(auto& hole: holes) {
        if(hole.value) {
            auto writer = instanceMember(module, program.coreClasses.show, hole.type, 0, expr.source);
            if(!writer) {
                context.diagnostics.error("cannot format a value of type %@ - it has no instance of `Show`, so there is nothing that says what its text is"_v,
                                          expr.source, describeType(context, global, hole.type));
                return nullptr;
            }

            if(!appendTo(writer, hole.value, false)) return nullptr;
        }

        if(hole.hasText) {
            auto literal = resolveString(expr.source, hole.text);
            if(!literal || !appendTo(program.pushString, literal, true)) return nullptr;
        }
    }

    /*
     * The finished string, read out of the storage it was built in.
     *
     * A *load* and not the allocation, and the difference is the whole of what a format expression
     * produces: the sink is storage this frame owns and the format's value is the string in it. On
     * JS that distinction is visible in the emitted source - the sink is a box, so handing the
     * allocation on passes `{$v: ...}` where every reader wants `.$v` - and natively it is the
     * difference between the address and the two words at it.
     */
    return load(placeFor(sinkValue, expr.source), expr.source);
}

ModulePtr<Value> ExprResolver::resolveLiteral(const ast::Expr& expr, TypePtr target) {
    switch(ast::Literal::Kind(expr.kind - ast::Expr::Lit)) {
        case ast::Literal::Int:
            return resolveInteger(expr.source, target, expr.lit.i());
        case ast::Literal::Float:
            return resolveDecimal(expr.source, target, F64(expr.lit.f));
        case ast::Literal::Double:
            return resolveDecimal(expr.source, target, expr.lit.d());
        case ast::Literal::String:
            return resolveString(expr.source, expr.lit.s);
        case ast::Literal::Bool:
            return makeInt(expr.source, module.scalar.bool_, expr.lit.b ? 1 : 0);
        default:
            context.diagnostics.error("literal is not available in the aggregate resolver"_v, expr.source);
            return nullptr;
    }
}

ModulePtr<Value> ExprResolver::resolve(const ast::Expr& expr, TypePtr target, bool used, bool implicit) {
    if(!current) return nullptr;
    if(ast::isLiteral(expr)) return resolveLiteral(expr, target);

    /*
     * The top of a chain containing a `?.`, which is where the skip those need has to be set up.
     *
     * Ahead of the switch because the extent of a `?.`'s skip is the rest of *its chain*, and a
     * chain is a spine of these four node kinds rather than one of them - `a?.b.c(x)` tops out at a
     * call and `a?.b` at the `?.` itself. Entering here is what lets everything below resolve as the
     * ordinary chain it is, with the `?.` nodes finding the join through `optionalChain`.
     *
     * `onOptionalSpine` is what stops this re-entering the chain it is already resolving, and what
     * makes a chain written inside one's *arguments* its own - see OptionalChain.
     */
    switch(expr.kind) {
        case ast::Expr::Field:
        case ast::Expr::Unwrap:
        case ast::Expr::App:
        case ast::Expr::Sub:
            if(!onOptionalSpine(expr) && chainSkips(expr)) {
                return resolveOptionalChain(expr, target, used, implicit);
            }

            break;
        default:
            break;
    }

    switch(expr.kind) {
        case ast::Expr::Error:
            sawParseError = true;
            return nullptr;
        case ast::Expr::Nested:
            return resolve(*parse[expr.nested], target, used, implicit);
        case ast::Expr::Multi: {
            ModulePtr<Value> result = nullptr;
            auto expressions = expr.multi;
            auto values = expressions.contents(parse);

            for(Size i = 0; i < values.size() && current; i++) {
                auto last = i + 1 == values.size();

                /*
                 * A lens call consumes the rest of this block, so it is the last thing the loop
                 * does whatever position it was written in - see expr_lens.cpp. The value it
                 * produces is the block's, because the statements after it are what produced it.
                 */
                ModulePtr<Value> lens = nullptr;
                if(resolveLensStatement(expressions, i, used, lens)) {
                    if(lens && target && current) lens = convert(lens, target, values[i].source, implicit);
                    return lens;
                }

                result = resolve(values[i], last ? target : nullptr, used && last, last && implicit);

                // Each element of a block is a statement of its own, which is the boundary a
                // literal variable that nothing decided has to be settled at.
                if(!last) result = settle(result, values[i].source);
            }

            return result;
        }
        case ast::Expr::Var: {
            /*
             * The cursor sentinel in ordinary value position - Implementation-Tooling.md §8.2.
             *
             * Everything completion needs is already here: the scope stack, and the type this
             * position was asked for. Ahead of the lookup because the sentinel names nothing, so
             * the lookup's only possible outcome is the "unknown scalar value" report below.
             */
            if(isCursorSentinel(context, expr.var)) {
                captureCompletion(*this, target, nullptr, false);
                return nullptr;
            }

            auto binding = findBinding(expr.var, expr.source);
            if(!binding) {
                if(auto found = findGlobal(module, expr.var, expr.source)) {
                    auto value = globalValue(found, expr.source);
                    return value && target ? convert(value, target, expr.source, implicit) : value;
                }

                // A function's name in value position is the function value that reaches it. This
                // is the last thing tried rather than the first, so a binding and a global still
                // shadow a declaration exactly as they did before function values existed.
                if(auto callee = findFunction(module, expr.var, expr.source)) {
                    auto value = functionValue(callee, expr.source);
                    return value && target ? convert(value, target, expr.source, implicit) : value;
                }

                context.diagnostics.error("unknown scalar value %@"_v, expr.source, context.findName(expr.var));
                return nullptr;
            }

            // Reading a `@lazy` parameter is what runs the argument the caller wrote, so this one
            // name is an effect rather than a value that was already there. Once, on any path -
            // checked over the whole body by checkLazyForcing below.
            if(binding->lazy) {
                Deferred deferred;
                deferred.thunk = binding->value;

                auto forced = force(deferred, nullptr, expr.source);
                return forced && target ? convert(forced, target, expr.source, implicit) : forced;
            }

            // A mutable binding names storage, so what its name produces is whatever is in that
            // storage now rather than what was put there when it was declared. The name stays on
            // the place, and each read of it is its own value.
            auto value = binding->isPlace() ? load(placeOf(*binding, expr.source), expr.source)
                                            : binding->value;

            return value && target ? convert(value, target, expr.source, implicit) : value;
        }
        case ast::Expr::Con:
            return resolveConstruct(expr, *parse[expr.con], target);
        case ast::Expr::App:
            return resolveCall(expr, *parse[expr.app], target, implicit);
        case ast::Expr::Infix:
            return resolveBinary(expr, *parse[expr.infix], target, implicit);
        case ast::Expr::Prefix:
            return resolvePrefix(expr, *parse[expr.prefix], target, implicit);
        case ast::Expr::If:
            return resolveIf(expr, *parse[expr.singleIf], target, used, implicit);
        case ast::Expr::MultiIf:
            return resolveMultiIf(expr, expr.multiIf, target, used, implicit);
        case ast::Expr::Is:
            return resolveIs(expr, *parse[expr.is], used);
        case ast::Expr::Try:
            return resolveTry(expr, target, used, implicit);
        case ast::Expr::Match:
            return resolveMatch(expr, *parse[expr.match], target, used, implicit);
        case ast::Expr::Decl:
            return resolveDecl(expr.decl, target, used);
        case ast::Expr::While:
            resolveWhile(*parse[expr.whileLoop]);
            return nullptr;
        case ast::Expr::For:
            resolveFor(expr, *parse[expr.forLoop]);
            return nullptr;
        case ast::Expr::Coerce: {
            auto& coerce = *parse[expr.coerce];

            // Resolved against this function's own context, so that an ascription inside a generic
            // body may name the variables that body is written over - `cast(p) :: %a` is how a
            // generic function says which of the two pointer types a reinterpretation produces.
            auto type = resolveType(module, coerce.type, functionGen(global, function));

            /*
             * The cursor sentinel takes the ascription as what this position asked for.
             *
             * Here rather than in the Var case below, because the fallback at the end of this
             * function deliberately does *not* push the type into a plain name - `x :: U8` converts
             * explicitly afterwards rather than resolving `x` against `U8`. An ascription on a name
             * that has not been written yet is the one thing saying what belongs there, so it is
             * exactly the type completion should rank by.
             */
            if(coerce.target.kind == ast::Expr::Var && isCursorSentinel(context, coerce.target.var)) {
                captureCompletion(*this, type, nullptr, false);
                return nullptr;
            }

            // `::` is what supplies the expected type where nothing else does, so it is pushed
            // down into a literal (which has no type of its own), into a call (whose class
            // instance may be decided by its result type - `truncate(x) :: Int`) and into a
            // constructor (whose record's type arguments may be - `Nothing :: Maybe(%U8)`, which
            // nothing else in the expression says). The call keeps its own result unconverted,
            // because the ascription that selected the instance is also the explicit conversion,
            // and an explicit one may narrow.
            if(ast::isLiteral(coerce.target)) {
                return convert(resolve(coerce.target, type), type, expr.source, false);
            }

            if(coerce.target.kind == ast::Expr::Con) {
                return resolveConstruct(coerce.target, *parse[coerce.target.con], type);
            }

            // An array literal, for the same reason - Implementation-Containers.md §8's "a literal
            // reaches `[T]` and `[T *n]` by ordinary context typing". Which of the two it builds is
            // decided by the expected type and by nothing else, so an ascription that arrived after
            // the fact would have built the wrong container and then found no conversion between
            // them - there is deliberately none, since fixed-owner to growable-owner allocates and
            // copies. The result is still converted, because `[1, 2] :: [Int]` in an argument
            // position may go on to become a slice.
            if(coerce.target.kind == ast::Expr::Array) {
                return convert(resolveArray(coerce.target, coerce.target.arr, type), type,
                               expr.source, false);
            }

            // A lambda has no type of its own either: its argument types and its result are read
            // off the position it appears in, and `::` is what supplies one where nothing else
            // does. Through the parentheses, because `::` binds looser than the lambda arrow and
            // `((x) -> x * 3) :: (Int) -> Int` is how one is written.
            auto& ascribed = unwrapNested(coerce.target);
            if(ascribed.kind == ast::Expr::Fun) {
                return resolveFun(ascribed, *parse[ascribed.fun], type);
            }

            if(coerce.target.kind == ast::Expr::App) {
                auto value = resolveCall(coerce.target, *parse[coerce.target.app], type, false);
                return convert(value, type, expr.source, false);
            }

            if(coerce.target.kind == ast::Expr::Prefix) {
                auto value = resolvePrefix(coerce.target, *parse[coerce.target.prefix], type, false);
                return convert(value, type, expr.source, false);
            }

            /*
             * A form with no type of its own - a parenthesis, a block, the arms of an `if` or a
             * `match` - is a pass-through, so the ascription belongs to each leaf rather than to the
             * value they join. Without this the target stopped at the parenthesis: `(a `or` b) :: T`
             * resolved the operator chain against nothing, its literals defaulted to `Int`, and the
             * truncated result was converted afterwards.
             *
             * Pushed down as an *explicit* conversion, which is the whole reason `implicit` exists
             * as a parameter. `(x) :: U8` has to keep meaning what `x :: U8` means, and an implicit
             * conversion to a narrower type is an error about precision rather than a narrowing.
             *
             * The conversion below is then a no-op in the ordinary case and the fallback in the one
             * where a leaf produced something else - a branch whose arms unified to a common type
             * that still has to reach the ascribed one.
             */
            if(isPassThrough(coerce.target)) {
                return convert(resolve(coerce.target, type, true, false), type, expr.source, false);
            }

            return convert(resolve(coerce.target), type, expr.source, false);
        }
        case ast::Expr::Ret:
            resolveReturn(expr);
            return nullptr;
        case ast::Expr::Yield:
            return resolveYield(expr);
        case ast::Expr::Break:
        case ast::Expr::Continue: {
            /*
             * A `for` loop's body is lifted, so the loop these leave is not one this function has a
             * block for: it is the call in the enclosing frame, and leaving it is a value returned
             * to the iterator - Analysis-Lens.md §7.3's step signal. Which value depends on what
             * the rest of this body does, so the block is left open the way a `return` here is and
             * finished once that is known.
             *
             * Before the `inContinuation` case below, because a `for` body is both: what makes a
             * `break` here mean this loop rather than one further out is that a `for` *is* the
             * nearest enclosing loop of anything written in it.
             */
            if(loops.isEmpty() && inLoopBody) {
                if(expr.kind == ast::Expr::Break && expr.breakValue) {
                    context.diagnostics.error("a `for` loop does not produce a value in this version, so `break` cannot carry one - the loop's own value is what the iterator's result would have to hold, which is Analysis-Lens.md's V3"_v,
                                              expr.source);
                }

                loopExits.push(ContinuationLoopExit { current, expr.kind == ast::Expr::Break, expr.source });
                current = nullptr;
                return nullptr;
            }

            if(loops.isEmpty() && inContinuation) {
                // The loop is in the function this continuation was split out of, so leaving it is
                // the exit signal carrying a `break` rather than a `return` - Analysis-Lens.md
                // §5.1's "break/continue are the loop-shaped instance of one mechanism". A `for`
                // body is the case that mechanism now covers; a lens continuation is not, since the
                // lens between here and the loop has no step signal to report the departure in.
                context.diagnostics.error("`break` and `continue` cannot cross a lens call yet - the loop is in the function this block was split out of, and only `return` carries the exit signal past a lens"_v,
                                          expr.source);
                return nullptr;
            }

            if(loops.isEmpty()) {
                context.diagnostics.error(expr.kind == ast::Expr::Break ? "break outside a loop"_v : "continue outside a loop"_v, expr.source);
                return nullptr;
            }

            if(expr.kind == ast::Expr::Break && expr.breakValue) {
                context.diagnostics.error("scalar while loops do not produce values"_v, expr.source);
            }

            auto& loop = loops[loops.size() - 1];

            // A counted `for` has not built the block this leaves to yet - see LoopTarget. The
            // block is left open and resolveCountedFor comes back for it.
            if(auto deferred = expr.kind == ast::Expr::Break ? loop.deferredBreak : loop.deferredContinue) {
                deferred->push(current);
                current = nullptr;
                return nullptr;
            }

            auto targetBlock = expr.kind == ast::Expr::Break ? loop.breakBlock : loop.continueBlock;
            terminate(emit<InstJmp>(expr.source, 0, module.scalar.unit, targetBlock));

            return nullptr;
        }
        case ast::Expr::Array:
            return resolveArray(expr, expr.arr, target);
        case ast::Expr::Format:
            return resolveFormat(expr);
        case ast::Expr::Sub: {
            // A subscript read produces a borrow of the element, which the position it appears in
            // then reads through - so the caller writes `xs[0] + 1` and never names the borrow.
            auto borrowed = resolveSubscript(expr, *parse[expr.sub], false);
            if(!borrowed || !isBorrow(global, valueType(borrowed))) return borrowed;

            return convert(borrowed, ((BorrowType*)global[valueType(borrowed)])->to, expr.source);
        }
        case ast::Expr::Tup:
            return resolveTuple(expr, expr.tup, target);
        case ast::Expr::TupUpdate:
            return resolveTupUpdate(expr, *parse[expr.tupUpdate], target);
        case ast::Expr::Field:
            return resolveField(expr, *parse[expr.field]);
        case ast::Expr::Unwrap:
            return resolveUnwrap(expr);
        case ast::Expr::Assign:
            return resolveAssign(expr, *parse[expr.assign]);
        case ast::Expr::Fun:
            return resolveFun(expr, *parse[expr.fun], target);
        default:
            context.diagnostics.error("expression is not available in the aggregate resolver"_v, expr.source);
            return nullptr;
    }
}

/*
 * Names one binding per parameter, and storage for the ones that need it.
 *
 * `firstArg` is where the declared parameters start, which is one for anything reached as a
 * function value: those take the closure environment as argument zero, and it is bound by whoever
 * knows what is in it rather than by name.
 */
void bindFunctionArgs(ExprResolver& resolver, Module& module, Function& function, Size firstArg) {
    Size index = 0;

    for(auto argPointer: function.args.contents(*module.arena)) {
        if(index++ < firstArg) continue;

        auto arg = (*module.arena)[argPointer];
        auto value = (ModulePtr<Value>)argPointer;
        Binding binding { arg->name, value };
        binding.definition = arg->source;

        if(arg->isLazy()) {
            // The name holds the thunk, not the value the signature declared, and reading it is
            // what runs the caller's expression. No local and no place: there is nothing here to
            // load from until the force has happened - see ExprResolver::force.
            binding.lazy = true;
            resolver.bindings.push(binding);
            recordBindingDefinition(resolver, binding);
            continue;
        }

        if(arg->isMutableBorrow()) {
            // A `&` parameter names storage the caller owns. The argument arrived as the address
            // of it, so the parameter gets a local whose value *is* that address - which is
            // exactly what a local of an ordinary allocation holds - and the binding names the
            // slot rather than the value, so reads load and assignments write through.
            //
            // `borrowed` is what keeps this frame from treating the slot as its own: it is never
            // allocated here and never dropped here.
            binding.local = function.addLocal(module, arg->type, arg->name, value,
                                              ast::BindType::Ref, true);
        } else if(isMemoryType(*module.types, arg->type)) {
            function.addLocal(module, arg->type, arg->name, value, arg->convention);
        }

        resolver.bindings.push(binding);
        recordBindingDefinition(resolver, binding);
    }
}

/*
 * A `@lazy` parameter may be forced at most once on any path (Design.md's Deferred arguments).
 *
 * This is what makes the absence of a memoization slot a rule rather than an omission: forcing
 * twice is rejected, so no program can tell call-by-name from call-by-need and no cell has to exist
 * to make that true. It is also the whole of the linearity checking this version needs - the same
 * shape linear types will want, stated over one parameter instead of over every owner.
 *
 * *Using* the parameter rather than calling it, because there are two ways to spend the one
 * evaluation and only one of them is a call. Reading the name forces it here; passing it on to
 * another `@lazy` parameter hands the evaluation to a callee that may spend it, and a body that did
 * both would have evaluated the caller's argument twice. Every use is therefore counted, which is
 * what the value's own use list already records.
 *
 * A forward fixpoint over "may already have been used", which is the only formulation that gets a
 * loop right: using it once in a loop body is using it once per iteration, and that is the second
 * use. Iterated because the block list is in RPO but a back edge still carries state backwards -
 * one pass would clear a body that the second visit rejects.
 */
static void checkLazyForcing(Module& module, Function& function) {
    auto local = *module.arena;
    auto blocks = function.blocks.size();

    for(auto argPointer: function.args.contents(local)) {
        auto arg = local[argPointer];
        if(!arg->isLazy()) continue;

        if(arg->useCount() < 2) continue;

        auto isUse = [&](ModulePtr<Inst> instruction) {
            for(auto user: arg->uses(local)) {
                if(user == instruction) return true;
            }

            return false;
        };

        // `exit[i]` is whether some path through block i has used it by the time it ends. Nothing
        // else has to be remembered: a block's entry state is the union of its predecessors' exits,
        // which is recomputed each visit.
        Array<bool> exit;
        for(Size i = 0; i < blocks; i++) exit.push(false);

        auto reported = false;

        for(auto changed = true; changed && !reported;) {
            changed = false;

            for(Size i = 0; i < blocks; i++) {
                auto block = local[function.blocks.get(local, i)];

                auto used = false;
                for(auto incoming: block->incoming(local)) {
                    if(exit[local[incoming]->index]) used = true;
                }

                if(i == 0) used = false;

                for(auto instPointer: block->instructions(local)) {
                    if(!isUse(instPointer)) continue;

                    if(used) {
                        module.context.diagnostics.error("%@ is a `@lazy` parameter and may be used at most once on any path, but this path uses it again - passing it to another `@lazy` parameter counts, since the callee may be the one that runs it. Read it into a `let` and use that instead"_v,
                                                         local[instPointer]->source,
                                                         module.context.findName(arg->name));
                        reported = true;
                        break;
                    }

                    used = true;
                }

                if(reported) break;
                if(exit[i] != used) changed = true;

                exit[i] = used;
            }
        }
    }
}

// Class signatures, generated functions and specializations have no AST and are already complete.
bool resolveFunctionBody(Module& module, Function& function) {
    auto& context = module.context;
    if(!function.ast || function.resolving) return true;

    // A declaration whose implementation the compiler generates has no body to resolve and never
    // will: what it means is one instruction at each call site rather than anything writable.
    if(function.intrinsic) return true;

    auto& decl = *module.parse[function.ast];
    if(!decl.fun.body) {
        context.diagnostics.error("function %@ requires a body"_v, decl.source, context.findName(function.name));
        return false;
    }

    function.resolving = true;

    ExprResolver resolver(context, module, function);
    bindFunctionArgs(resolver, module, function, 0);

    auto errors = context.diagnostics.errorCount();

    /*
     * A `yield`-form lens returns what its continuation produced, not what its last statement did.
     *
     * Everything after the `yield` is cleanup - Design.md's `withLock` unlocks there - so the value
     * that leaves is the one the `yield` handed back, and the body's own fall-through result is
     * discarded. That is the whole of what the sugar does that the explicit form would have had to
     * write out by hand.
     */
    if(function.yieldForm) {
        resolver.resolve(*module.parse[decl.fun.body], nullptr, false);

        if(resolver.current) {
            // An iterator falling off the end ran to completion, which is the step signal's
            // `Proceed` - there is no carried value, because nothing stopped it. A lens instead
            // returns what its one `yield` handed back, since everything after that is cleanup.
            auto result = function.funKind == ast::FunKind::Iter
                        ? resolver.makeOutcome(function.returnType, true, nullptr, decl.source)
                        : resolver.yieldResult;

            resolver.terminate(resolver.emit<InstRet>(decl.source, 0, module.scalar.unit, result));
        }

        checkLensYields(module, function, toBuffer(resolver.yields), decl.source);
        checkLazyForcing(module, function);

        function.ast = nullptr;
        function.resolving = false;
        return errors == context.diagnostics.errorCount();
    }

    if(decl.fun.implicitReturn) {
        /*
         * An `=` body is the function's result, so what is resolved here is a value and what
         * follows it is a `ret` carrying that value.
         *
         * Three ways the result type is known. Written, in which case the body is checked against
         * it. Inferred, in which case the body decides and `settle` runs first for the same reason
         * it does for a lambda - a bare literal body must not leave a result type no caller could
         * name. Or written as unit, where the body is resolved with no expected type, because `()`
         * is not a type a literal or a class function could have been asked to produce.
         */
        auto infer = function.inferReturn;
        auto unit = !infer && isUnit(*module.types, function.returnType);
        auto expected = infer ? nullptr : function.returnType;
        auto result = resolver.resolve(*module.parse[decl.fun.body], unit ? nullptr : expected, !unit);

        if(resolver.current) {
            if(infer) {
                result = resolver.settle(result, decl.source);
                function.returnType = result ? resolver.valueType(result) : module.scalar.unit;
                function.inferReturn = false;
                applyReturnRoots(module, function, decl.source);

                if(isUnit(*module.types, function.returnType)) result = nullptr;
            } else {
                result = unit ? nullptr : resolver.convert(result, function.returnType, decl.source);
            }

            resolver.terminate(resolver.emit<InstRet>(decl.source, 0, module.scalar.unit,
                                                      resolver.returnValue(result, decl.source)));
        } else if(infer) {
            // Every path left through an explicit `return`, so nothing falls off the end for the
            // type to be read off. Those returns were checked against null and reported there.
            function.returnType = module.scalar.unit;
            function.inferReturn = false;
        }

        /*
         * An `=` function that produces nothing is written in the wrong form.
         *
         * The `=` form says "this function *is* this expression", so a body with no value is a
         * statement wearing an expression's syntax - `fn bump(&x: Int) = x = x + 1` reads as though
         * it returned something. The block form is how that is said, and it is what this points at.
         *
         * Only when the unit result was not written down: `-> ()` is the author saying the same
         * thing the warning would, so repeating it back is noise.
         */
        if(!decl.fun.ret && isUnit(*module.types, function.returnType) && !function.instanceOf
           && errors == context.diagnostics.errorCount()) {
            context.diagnostics.warning("`%@` is written with `=` but its body produces no value, so it returns `()` - use the `:` block form for a function that runs statements rather than producing a result"_v,
                                        decl.source, context.findName(function.name));
        }
    } else {
        resolver.resolve(*module.parse[decl.fun.body], nullptr, false);

        if(resolver.current) {
            if(isUnit(*module.types, function.returnType)) {
                resolver.terminate(resolver.emit<InstRet>(decl.source, 0, module.scalar.unit, nullptr));
            } else if(!resolver.sawParseError) {
                // A body with a hole in it does not return a value because it is not finished,
                // which the parser has already said. Saying it again puts a second mark on a
                // function whose only problem is that it is halfway through being written.
                context.diagnostics.error("not all paths return a value"_v, decl.source);
            }
        }
    }

    checkLazyForcing(module, function);

    function.ast = nullptr;
    function.resolving = false;
    return errors == context.diagnostics.errorCount();
}

bool resolveModuleBodies(Module& module) {
    auto success = true;
    auto local = *module.arena;

    /*
     * The functions whose result type their own body decides, first.
     *
     * A call reads its callee's result type, so every one of these has to be known before any body
     * that might call one is resolved - otherwise the answer would depend on declaration order.
     * Doing them as their own pass makes the order they are settled in irrelevant: what remains is
     * one inferring function calling another, which requireReturnType() resolves on demand.
     */
    for(Size i = 0; i < module.functionOrder.size(); i++) {
        auto function = local[module.functionOrder.get(local, i)];
        if(function->inferReturn) success = resolveFunctionBody(module, *function) && success;
    }

    // Resolving one body adds specialized functions to the module, so the list is walked by index
    // rather than by iterator: a specialization created while resolving function 3 is reached
    // when the loop gets to it.
    for(Size i = 0; i < module.functionOrder.size(); i++) {
        success = resolveFunctionBody(module, *local[module.functionOrder.get(local, i)]) && success;
    }

    return success;
}
