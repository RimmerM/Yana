#include "expr.h"
#include "generic.h"
#include "name.h"

void ExprResolver::terminate(Inst* inst) {
    assertTrue(isTerminator(*inst));
    current = nullptr;
}

Binding* ExprResolver::findBinding(StringId name) {
    for(Size i = bindings.size(); i > 0; i--) {
        if(bindings[i - 1].name == name) return &bindings[i - 1];
    }

    // A name a lambda body does not bind itself may still belong to an enclosing one, and naming it
    // is what makes it a capture. Nothing is a capture until it is used, which is Design-Memory
    // §8's "there is no capture list" made literal.
    return captureBinding(name);
}

ModulePtr<Value> ExprResolver::find(StringId name) {
    auto binding = findBinding(name);
    return binding ? binding->value : nullptr;
}

ModulePtr<Value> ExprResolver::makeInt(LocationId source, TypePtr type, U64 value) {
    return constant<ConstInt>(source, type, value);
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
    if(isFloat(global, type)) {
        if(((FloatType*)global[type])->width == FloatType::Float) {
            F32 single;
            copy((const Byte*)&bits, (Byte*)&single, sizeof(single));
            return makeFloat(source, type, F64(single));
        }

        F64 number;
        copy((const Byte*)&bits, (Byte*)&number, sizeof(number));
        return makeFloat(source, type, number);
    }

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
        if(isInteger(global, target)) return makeInt(source, target, written);
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

    Array<ClassFunRef> candidates;
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

        return ref(emit<InstBorrow>(source, 0, target, place.unwrap(), wanted->mut));
    }

    if(sameType(((BorrowType*)global[from])->to, target)) {
        return load(Place::inBorrow(value), source, local[value]->name);
    }

    context.diagnostics.error("cannot convert %@ to %@"_v, source,
                              describeType(context, global, from),
                              describeType(context, global, target));
    return value;
}

ModulePtr<Value> ExprResolver::convert(ModulePtr<Value> value, TypePtr target, LocationId source, bool implicit) {
    if(!value || !target) return value;

    auto from = local[value]->type;

    // A literal has no type to convert from: it is built at whatever type this position asks for,
    // through its own class, which is also how it reaches a user type that has an instance.
    if(isLiteral(global, from)) return materializeLiteral(value, target, source);

    if(sameType(from, target)) return value;
    if(global[from]->kind == Type::Error || global[target]->kind == Type::Error) return value;

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

    TypePtr args[] = { from, target };
    return findInstance(module, module.coreClasses.widen, { args, 2 }) != nullptr;
}

ModulePtr<Value> ExprResolver::finishBranches(Array<BranchArm>& arms, LocationId source, bool used) {
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

ModulePtr<Value> ExprResolver::resolveIf(const ast::Expr& expr, const ast::IfExpr& branch, TypePtr target, bool used) {
    auto bindingCount = bindings.size();
    ModulePtr<Block> elseBlock = nullptr;

    // The condition leaves `current` at the block where it held, which is where an `is` test's
    // bindings are live - so the `then` arm is resolved with them in scope and the resize below
    // takes them away again, exactly as the arms of a `match` scope what their patterns bind.
    if(resolveCondition(branch.cond, elseBlock) == PatternResult::Never) return nullptr;

    Array<BranchArm> arms;

    auto thenValue = resolve(branch.then, target, used);
    if(current) arms.push(BranchArm { current, thenValue, branch.then.source });
    bindings.resize(bindingCount);

    current = elseBlock;
    ModulePtr<Value> elseValue = nullptr;
    auto elseSource = expr.source;

    if(branch.otherwise) {
        elseValue = resolve(branch.otherwise.unwrap(), target, used);
        elseSource = branch.otherwise.unwrap().source;
    } else if(used) {
        context.diagnostics.error("value-producing if requires an else branch"_v, expr.source);
    }

    if(current) arms.push(BranchArm { current, elseValue, elseSource });
    bindings.resize(bindingCount);

    return finishBranches(arms, expr.source, used);
}

ModulePtr<Value> ExprResolver::resolveMultiIf(const ast::Expr& expr, ast::ParseList<ast::IfCase> cases, TypePtr target, bool used) {
    auto contents = cases.contents(parse);
    if(contents.size() == 0) return nullptr;

    auto bindingCount = bindings.size();
    Array<BranchArm> arms;
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

        auto value = resolve(contents[i].then, target, used);
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

void ExprResolver::resolveReturn(const ast::Expr& expr) {
    if(resultInferred) {
        // Nothing has decided what this lambda returns yet, and `return` cannot be the thing that
        // decides it: a later `return` of a different type would have nothing to be checked
        // against, and the two would silently disagree.
        context.diagnostics.error("this lambda's result type is decided by its body, so it cannot use `return` - write it where a function type is expected"_v,
                                  expr.source);
    }

    ModulePtr<Value> value = nullptr;
    if(expr.ret) value = resolve(*parse[expr.ret], function.returnType);

    if(isUnit(global, function.returnType)) {
        if(value) context.diagnostics.error("unit function cannot return a value"_v, expr.source);
        value = nullptr;
    } else if(!value) {
        context.diagnostics.error("non-unit function must return a value"_v, expr.source);
    } else {
        value = convert(value, function.returnType, expr.source);
    }

    terminate(emit<InstRet>(expr.source, 0, module.scalar.unit, value));
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

        // A `let` is a statement boundary, so a literal the initializer left open is settled to
        // its default here: `let x = 1` binds an Int, and nothing later in the block can go back
        // and make it a Long.
        auto value = settle(resolve(*parse[decl.content]), decl.pat.source);
        if(!value) continue;

        // `let ->z = x` takes ownership out of whatever `x` named, so the name that follows binds
        // the moved value rather than the source. The binding itself is an ordinary immutable one:
        // what `->` decides is where the value came from, not what may be done with it after.
        if(decl.bind == ast::BindType::Sink) {
            value = sinkValue(value, decl.pat.source);
            if(!value) continue;
        }

        if(isBorrow(global, valueType(value))) {
            bindBorrow(decl, value, mutable_);
        } else if(mutable_) {
            bindMutable(decl, value);
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
    bindings.push(binding);
}

void ExprResolver::bindMutable(const ast::VarDecl& declaration, ModulePtr<Value> value) {
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
    auto type = valueType(value);
    auto storage = allocate(type, declaration.pat.source, name, ast::BindType::Ref);
    auto place = placeFor(storage, declaration.pat.source);

    initialize(place, value, declaration.pat.source);
    bindings.push(Binding { name, storage, place.local });
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
            if(auto binding = findBinding(expr.var)) {
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
    if(target && isInteger(global, target)) return makeInt(source, target, value);

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

ModulePtr<Value> ExprResolver::resolveLiteral(const ast::Expr& expr, TypePtr target) {
    switch(ast::Literal::Kind(expr.kind - ast::Expr::Lit)) {
        case ast::Literal::Int:
            return resolveInteger(expr.source, target, expr.lit.i());
        case ast::Literal::Float:
            return resolveDecimal(expr.source, target, F64(expr.lit.f));
        case ast::Literal::Double:
            return resolveDecimal(expr.source, target, expr.lit.d());
        case ast::Literal::Bool:
            return makeInt(expr.source, module.scalar.bool_, expr.lit.b ? 1 : 0);
        default:
            context.diagnostics.error("literal is not available in the aggregate resolver"_v, expr.source);
            return nullptr;
    }
}

ModulePtr<Value> ExprResolver::resolve(const ast::Expr& expr, TypePtr target, bool used) {
    if(!current) return nullptr;
    if(ast::isLiteral(expr)) return resolveLiteral(expr, target);

    switch(expr.kind) {
        case ast::Expr::Error:
            return nullptr;
        case ast::Expr::Nested:
            return resolve(*parse[expr.nested], target, used);
        case ast::Expr::Multi: {
            ModulePtr<Value> result = nullptr;
            auto expressions = expr.multi;
            auto values = expressions.contents(parse);

            for(Size i = 0; i < values.size() && current; i++) {
                auto last = i + 1 == values.size();
                result = resolve(values[i], last ? target : nullptr, used && last);

                // Each element of a block is a statement of its own, which is the boundary a
                // literal variable that nothing decided has to be settled at.
                if(!last) result = settle(result, values[i].source);
            }

            return result;
        }
        case ast::Expr::Var: {
            auto binding = findBinding(expr.var);
            if(!binding) {
                if(auto found = findGlobal(module, expr.var, expr.source)) {
                    auto value = globalValue(found, expr.source);
                    return value && target ? convert(value, target, expr.source) : value;
                }

                // A function's name in value position is the function value that reaches it. This
                // is the last thing tried rather than the first, so a binding and a global still
                // shadow a declaration exactly as they did before function values existed.
                if(auto callee = findFunction(module, expr.var, expr.source)) {
                    auto value = functionValue(callee, expr.source);
                    return value && target ? convert(value, target, expr.source) : value;
                }

                context.diagnostics.error("unknown scalar value %@"_v, expr.source, context.findName(expr.var));
                return nullptr;
            }

            // A mutable binding names storage, so what its name produces is whatever is in that
            // storage now rather than what was put there when it was declared. The name stays on
            // the place, and each read of it is its own value.
            auto value = binding->isPlace() ? load(placeOf(*binding, expr.source), expr.source)
                                            : binding->value;

            return value && target ? convert(value, target, expr.source) : value;
        }
        case ast::Expr::Con:
            return resolveConstruct(expr, *parse[expr.con], target);
        case ast::Expr::App:
            return resolveCall(expr, *parse[expr.app], target);
        case ast::Expr::Infix:
            return resolveBinary(expr, *parse[expr.infix], target);
        case ast::Expr::Prefix:
            return resolvePrefix(expr, *parse[expr.prefix], target);
        case ast::Expr::If:
            return resolveIf(expr, *parse[expr.singleIf], target, used);
        case ast::Expr::MultiIf:
            return resolveMultiIf(expr, expr.multiIf, target, used);
        case ast::Expr::Is:
            return resolveIs(expr, *parse[expr.is], used);
        case ast::Expr::Match:
            return resolveMatch(expr, *parse[expr.match], target, used);
        case ast::Expr::Decl:
            return resolveDecl(expr.decl, target, used);
        case ast::Expr::While:
            resolveWhile(*parse[expr.whileLoop]);
            return nullptr;
        case ast::Expr::Coerce: {
            auto& coerce = *parse[expr.coerce];

            // Resolved against this function's own context, so that an ascription inside a generic
            // body may name the variables that body is written over - `cast(p) :: %a` is how a
            // generic function says which of the two pointer types a reinterpretation produces.
            auto type = resolveType(module, coerce.type, functionGen(global, function));

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

            return convert(resolve(coerce.target), type, expr.source, false);
        }
        case ast::Expr::Ret:
            resolveReturn(expr);
            return nullptr;
        case ast::Expr::Break:
        case ast::Expr::Continue: {
            if(loops.isEmpty()) {
                context.diagnostics.error(expr.kind == ast::Expr::Break ? "break outside a loop"_v : "continue outside a loop"_v, expr.source);
                return nullptr;
            }

            if(expr.kind == ast::Expr::Break && expr.breakValue) {
                context.diagnostics.error("scalar while loops do not produce values"_v, expr.source);
            }

            auto& loop = loops[loops.size() - 1];
            auto targetBlock = expr.kind == ast::Expr::Break ? loop.breakBlock : loop.continueBlock;
            terminate(emit<InstJmp>(expr.source, 0, module.scalar.unit, targetBlock));

            return nullptr;
        }
        case ast::Expr::Array:
            return resolveArray(expr, expr.arr, target);
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

    if(decl.fun.implicitReturn) {
        // A unit function's body produces nothing that survives, so it is resolved with no
        // expected type rather than against `()` - which is not a type a literal or a class
        // function could have been asked to produce.
        auto unit = isUnit(*module.types, function.returnType);
        auto result = resolver.resolve(*module.parse[decl.fun.body], unit ? nullptr : function.returnType, !unit);

        if(resolver.current) {
            result = unit ? nullptr : resolver.convert(result, function.returnType, decl.source);
            resolver.terminate(resolver.emit<InstRet>(decl.source, 0, module.scalar.unit, result));
        }
    } else {
        resolver.resolve(*module.parse[decl.fun.body], nullptr, false);

        if(resolver.current) {
            if(isUnit(*module.types, function.returnType)) {
                resolver.terminate(resolver.emit<InstRet>(decl.source, 0, module.scalar.unit, nullptr));
            } else {
                context.diagnostics.error("not all paths return a value"_v, decl.source);
            }
        }
    }

    function.ast = nullptr;
    function.resolving = false;
    return errors == context.diagnostics.errorCount();
}

bool resolveModuleBodies(Module& module) {
    auto success = true;
    auto local = *module.arena;

    // Resolving one body adds specialized functions to the module, so the list is walked by index
    // rather than by iterator: a specialization created while resolving function 3 is reached
    // when the loop gets to it.
    for(Size i = 0; i < module.functionOrder.size(); i++) {
        success = resolveFunctionBody(module, *local[module.functionOrder.get(local, i)]) && success;
    }

    return success;
}
