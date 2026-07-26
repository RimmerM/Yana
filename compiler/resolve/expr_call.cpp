#include "expr.h"
#include "generic.h"
#include "name.h"

/*
 * Calls, operators, and typeclass instance selection.
 *
 * Every operator in the language is an ordinary call, and every arithmetic and comparison
 * operator is a class function: `+` is `Num.+`, `==` is `Eq.==`, and a cast is `Extend.extend`
 * or `Truncate.truncate`. Selecting one is two steps - work out what the class's type variables
 * must be here, then find the instance for them - and both steps run against the class
 * signature, which is why a variable appearing only in the result type is inferable exactly like
 * one appearing in an argument. That is what makes `round(x) :: Long` pick an instance by its
 * return type.
 *
 * The rule for which position gets to decide is deliberately one-directional: arguments bind
 * first, and the expected result type only fills in variables the arguments left open. Design.md
 * asks for bottom-up, left-to-right inference with no backtracking, and this is that rule - it
 * keeps `1 + 2 :: Long` an Int addition widened afterwards rather than silently becoming a Long
 * addition, while still letting `extend(x) :: Long` work at all.
 *
 * A name may be declared by more than one class and by one class at more than one arity, so
 * emitCall selects out of an overload set rather than off a single signature. Design.md's
 * Overloading section states the five rules it implements: the set is keyed by (name, arity),
 * candidates are not ranked, a constraint declared by the enclosing function is what picks a class
 * in generic code, ambiguity is resolved by writing `Class.method` and never by a tiebreak, and a
 * plain function is one member of the set rather than a shadow over it.
 */

static U8 operatorPrecedence(Module& module, StringId op) {
    auto found = findPrecedence(module, op);
    return found ? found.unwrap() : 0;
}

// R4 resolves ambiguity by qualification and never by a tiebreak, so an ambiguity diagnostic has to
// say which qualified names there are to choose between - `Integral.and or Logic.and`. Leaving the
// author to go and find the classes themselves is the difference between a rule and a puzzle.
static String describeQualified(Context& context, GlobalBase global, StringId name,
                               Buffer<GlobalPtr<TypeClass>> classes) {
    Array<char> text;

    for(Size i = 0; i < classes.length; i++) {
        if(i) appendText(text, i + 1 == classes.length ? " or "_v : ", "_v);

        auto qualified = context.findName(global[classes[i]]->name) + String(".") + context.findName(name);
        for(Size c = 0; c < qualified.size(); c++) text.push(qualified.text()[c]);
    }

    auto data = (char*)hAlloc(text.size());
    copy(text.pointer(), data, text.size());
    return String(data, text.size(), true);
}

// Binds the class type variables that one position of a signature constrains. Numeric widening
// applies at the top level only: it is how `1 + 2.5` reaches Num(Double) without the class
// machinery having to know anything about numbers below the outermost type.
bool ExprResolver::bindPosition(TypePtr pattern, TypePtr actual, Array<TypePtr>& bindings, bool widen) {
    if(!pattern || !actual) return false;

    if(global[pattern]->kind == Type::Gen) {
        auto index = ((GenType*)global[pattern])->index;
        if(index >= bindings.size()) return false;

        if(!bindings[index]) {
            bindings[index] = actual;
            return true;
        }

        if(bindings[index] == actual) return true;
        if(!widen || !isNumeric(global, bindings[index]) || !isNumeric(global, actual)) return false;

        bindings[index] = numericRank(actual) > numericRank(bindings[index]) ? actual : bindings[index];
        return true;
    }

    return matchType(global, pattern, actual, { bindings.pointer(), bindings.size() });
}

// The instance of `typeClass` whose types are exactly `args`, or null.
ModulePtr<ClassInstance> ExprResolver::selectInstance(GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args) {
    return findInstance(module, typeClass, args);
}

// Works out whether one class function can serve this call, and if so which instance it selects.
// Returns false when the call does not fit the signature at all; a fitting signature with no
// instance is reported through `resolved` so the caller can tell "wrong function" from "no
// instance for these types", which are very different diagnostics.
bool ExprResolver::matchClassFun(const ClassFunRef& reference, Buffer<ModulePtr<Value>> args, TypePtr target,
                                 ClassMatch& resolved) {
    auto typeClass = global[reference.typeClass];
    auto signature = local[typeClass->functions.get(global, reference.index).fun];
    if(!signature || signature->args.size() != args.length) return false;

    auto env = global[typeClass->gen];
    Array<TypePtr> bindings;
    for(Size i = 0; i < env->types.size(); i++) bindings.push(nullptr);

    for(Size i = 0; i < args.length; i++) {
        if(!args[i]) return false;

        auto declared = local[signature->args.get(local, i)]->type;
        if(!bindPosition(declared, valueType(args[i]), bindings, true)) return false;
    }

    // The expected result only fills in what the arguments left open, so an ascription can pick
    // an instance but cannot re-pick one the arguments already determined.
    if(target) {
        Array<TypePtr> withTarget;
        for(auto binding: bindings) withTarget.push(binding);

        if(bindPosition(signature->returnType, target, withTarget, false)) {
            for(Size i = 0; i < bindings.size(); i++) {
                if(!bindings[i]) bindings[i] = withTarget[i];
            }
        }
    }

    for(auto binding: bindings) {
        if(!binding) return false;
    }

    resolved.typeClass = reference.typeClass;
    resolved.index = reference.index;
    resolved.instance = selectInstance(reference.typeClass, toBuffer(bindings));
    resolved.args.clear();
    for(auto binding: bindings) resolved.args.push(binding);

    return true;
}

// The plain-function half of an overload set. Design.md's R1 keys the set by (name, arity) and
// admits at most one plain function, so this is arity plus "do the arguments fit", and the answer
// has to be reached without reporting anything - see ExprResolver::convertible.
bool ExprResolver::matchFunction(ModulePtr<Function> callee, Buffer<ModulePtr<Value>> args, TypePtr target,
                                LocationId source) {
    auto callable = local[callee];
    if(callable->args.size() != args.length) return false;

    // A generic function fits when its type arguments can all be inferred here, by the same
    // one-directional rule the classes use.
    if(auto env = functionGen(global, *callable)) {
        Array<TypePtr> bindings;
        for(Size i = 0; i < env->types.size(); i++) bindings.push(nullptr);

        for(Size i = 0; i < args.length; i++) {
            auto declared = local[callable->args.get(local, i)]->type;
            if(!bindPosition(declared, valueType(args[i]), bindings, true)) return false;
        }

        if(target) {
            Array<TypePtr> withTarget;
            for(auto binding: bindings) withTarget.push(binding);

            if(bindPosition(callable->returnType, target, withTarget, false)) {
                for(Size i = 0; i < bindings.size(); i++) {
                    if(!bindings[i]) bindings[i] = withTarget[i];
                }
            }
        }

        for(auto binding: bindings) {
            if(!binding) return false;
        }

        return true;
    }

    for(Size i = 0; i < args.length; i++) {
        if(!convertible(args[i], local[callable->args.get(local, i)]->type, source)) return false;
    }

    return true;
}

// Precedence climbing over the flattened operand/operator lists. The parser cannot do this
// itself: fixity declarations are module-level, so an operator's precedence is only known once
// the whole module has been read.
ModulePtr<Value> ExprResolver::resolvePrecedence(Array<const ast::Expr*>& operands, Array<StringId>& operators, Size& operandIndex, Size& operatorIndex, U8 minimumPrecedence) {
    auto lhsExpr = operands[operandIndex++];
    auto lhs = resolve(*lhsExpr);

    while(operatorIndex < operators.size() && operatorPrecedence(module, operators[operatorIndex]) >= minimumPrecedence) {
        auto op = operators[operatorIndex++];
        auto precedence = operatorPrecedence(module, op);
        auto rhs = resolvePrecedence(operands, operators, operandIndex, operatorIndex, precedence + 1);

        if(!lhs || !rhs) return nullptr;

        ModulePtr<Value> args[] = { lhs, rhs };
        lhs = emitCall(op, { args, 2 }, lhsExpr->source);
    }

    return lhs;
}

ModulePtr<Value> ExprResolver::resolveBinary(const ast::Expr& expr, const ast::InfixExpr& binary, TypePtr target) {
    Array<const ast::Expr*> operands;
    Array<StringId> operators;
    auto node = &binary;

    // The parser nests infix expressions to the right without regard for precedence, so the
    // chain is flattened first and then re-associated by resolvePrecedence.
    while(true) {
        if(node->op.kind != ast::Expr::Var) {
            context.diagnostics.error("an infix operator must be a named operator"_v, node->op.source);
            return nullptr;
        }

        if(operatorPrecedence(module, node->op.var) == 0) {
            context.diagnostics.error("operator has no declared fixity %@"_v, node->op.source, context.findName(node->op.var));
            return nullptr;
        }

        operands.push(&node->lhs);
        operators.push(node->op.var);

        if(node->rhs.kind != ast::Expr::Infix) {
            operands.push(&node->rhs);
            break;
        }

        node = parse[node->rhs.infix];
    }

    Size operandIndex = 0;
    Size operatorIndex = 0;

    auto result = resolvePrecedence(operands, operators, operandIndex, operatorIndex, 1);
    if(result && target) result = convert(result, target, expr.source);
    return result;
}

ModulePtr<Value> ExprResolver::resolvePrefix(const ast::Expr& expr, const ast::PrefixExpr& prefix, TypePtr target,
                                             bool convertResult) {
    if(prefix.op.kind != ast::Expr::Var) {
        context.diagnostics.error("a prefix operator must be named"_v, prefix.op.source);
        return nullptr;
    }

    auto value = resolve(prefix.on, target);
    if(!value) return nullptr;

    ModulePtr<Value> args[] = { value };
    auto result = emitCall(prefix.op.var, { args, 1 }, expr.source, target);

    return convertResult && target ? convert(result, target, expr.source) : result;
}

ModulePtr<Value> ExprResolver::resolveCall(const ast::Expr& expr, const ast::AppExpr& call, TypePtr target, bool convertResult) {
    if(call.callee.kind != ast::Expr::Var) {
        context.diagnostics.error("function values and indirect calls are not available yet"_v, call.callee.source);
        return nullptr;
    }

    // A plain function's parameter types are known before its arguments are resolved, so they
    // are pushed down as the expected type of each one. That is what lets `f(Nothing)` know
    // which `Maybe` it is building - neither a class function nor a generic function can do the
    // same, because which types their parameters have is exactly what the arguments are being
    // resolved to decide.
    //
    // Only when the plain function is the whole overload set, though: R5 lets the class half serve
    // a call the plain function does not fit, and pushing its parameter types into the arguments
    // would report the mismatch before selection ever got the chance to look elsewhere.
    auto direct = findFunction(module, call.callee.var, expr.source);
    auto callArgs = call.args;
    auto declared = direct && !local[direct]->gen && local[direct]->args.size() == callArgs.size();

    if(declared) {
        Array<ClassFunRef> overloads;
        findClassFunctions(module, call.callee.var, expr.source, overloads);
        declared = overloads.isEmpty();
    }

    Array<ModulePtr<Value>> values;
    Size index = 0;

    for(auto arg: callArgs.contents(parse)) {
        if(arg.name) {
            context.diagnostics.error("named call arguments are not available yet"_v, arg.value.source);
        }

        auto expected = declared ? local[local[direct]->args.get(local, index)]->type : TypePtr(nullptr);
        values.push(resolve(arg.value, expected));
        index++;
    }

    auto result = declared ? emitDirectCall(direct, toBuffer(values), expr.source, target)
                           : emitCall(call.callee.var, toBuffer(values), expr.source, target);

    return convertResult && target ? convert(result, target, expr.source) : result;
}

// Emits a call to a known function, converting each argument to its declared type. An intrinsic
// produces its result directly instead: the primitives are real functions with real bodies, but
// an ordinary call to one expands to the instruction it contains rather than to a call the
// backend would have to inline again later.
ModulePtr<Value> ExprResolver::emitDirectCall(ModulePtr<Function> callee, Buffer<ModulePtr<Value>> args,
                                              LocationId source, TypePtr, StringId resultName) {
    auto function_ = local[callee];

    Array<ModulePtr<Value>> converted;
    for(Size i = 0; i < args.length; i++) {
        auto expected = local[function_->args.get(local, i)]->type;
        converted.push(convert(args[i], expected, source));
    }

    if(function_->intrinsic) {
        return function_->intrinsic(*this, toBuffer(converted), function_->returnType, source, resultName);
    }

    function_->used = true;
    auto call = create<InstCall>(source, resultName, function_->returnType, callee);

    for(auto value: converted) {
        if(value) call->args.push(module.arena, value);
    }

    append(call);
    auto result = ref(call);

    // An aggregate result is returned through storage the caller provides, so it needs a local
    // for the same reason a constructed value does - see resolve/lower.cpp's Call case.
    if(isMemoryType(global, call->type)) {
        call->local = function.addLocal(module, call->type, resultName, result);
    }

    return result;
}

ModulePtr<Value> ExprResolver::emitCall(StringId callName, Buffer<ModulePtr<Value>> args, LocationId source, TypePtr target, StringId resultName) {
    for(auto arg: args) {
        if(!arg) return nullptr;
    }

    Array<ClassFunRef> candidates;
    findClassFunctions(module, callName, source, candidates);

    auto direct = findFunction(module, callName, source);

    // Committing to the plain function, once it is the candidate the call is being served by. Its
    // arity is checked here rather than in matchFunction because a mismatch has to be reported as
    // itself: "takes two arguments" says more than the list of types the class half accepts.
    auto emitPlain = [&]() -> ModulePtr<Value> {
        if(local[direct]->args.size() != args.length) {
            context.diagnostics.error("%@ takes %@ arguments but was given %@"_v, source, context.findName(callName),
                                      U32(local[direct]->args.size()), U32(args.length));
            return nullptr;
        }

        return local[direct]->gen ? emitGenericCall(direct, args, source, target, resultName)
                                  : emitDirectCall(direct, args, source, target, resultName);
    };

    // R5: a plain function is an ordinary member of the overload set, not a shadow over it. It wins
    // when it fits, which keeps "my definition beats the imported one" for the case that really
    // overlaps; when it doesn't fit, the class candidates are still reachable. Shadowing outright
    // meant that a module-level `fn and(a: Permissions, b: Permissions)` silently disabled
    // `Integral.and` for every Int in the module, reported as an argument-type error on a call the
    // author never touched.
    if(direct && (candidates.isEmpty() || matchFunction(direct, args, target, source))) {
        return emitPlain();
    }

    if(candidates.isEmpty()) {
        context.diagnostics.error("unknown function %@"_v, source, context.findName(callName));
        return nullptr;
    }

    ClassMatch selected;
    ClassMatch withoutInstance;
    auto selectedCount = 0;
    auto withoutInstanceCount = 0;

    // Matches on this function's own type variables. Which class a call is, and with which type
    // arguments, is still decided here and once; only the instance has to wait until the types
    // become concrete.
    Array<ClassMatch> deferred;

    // Every class that turned out to apply, kept only so an ambiguity can name them all.
    Array<GlobalPtr<TypeClass>> applicable;

    for(auto& candidate: candidates) {
        ClassMatch match;
        if(!matchClassFun(candidate, args, target, match)) continue;

        auto isDeferred = false;
        for(auto argument: match.args) isDeferred = isDeferred || isGeneric(global, argument);

        if(isDeferred) {
            applicable.push(match.typeClass);
            deferred.push(::move(match));
        } else if(match.instance) {
            applicable.push(match.typeClass);
            if(!selectedCount) selected = ::move(match);
            selectedCount++;
        } else {
            if(!withoutInstanceCount) withoutInstance = ::move(match);
            withoutInstanceCount++;
        }
    }

    if(!selectedCount && deferred.isNotEmpty()) {
        // A requirement the signature already declared wins over one that would have to be
        // inferred, so writing the constraint out is also how an overloaded name is settled.
        auto env = functionGen(global, function);
        Size chosen = 0;
        Size declaredCount = 0;

        for(Size i = 0; env && i < deferred.size(); i++) {
            if(!hasClassRequirement(global, *env, deferred[i].typeClass, toBuffer(deferred[i].args))) continue;

            chosen = i;
            declaredCount++;
        }

        if(declaredCount > 1 || (!declaredCount && deferred.size() > 1)) {
            context.diagnostics.error(
                "ambiguous call to %@ - more than one class applies, and the types that would decide are not known here. Name one class here (%@), or declare which one this function requires"_v,
                source, context.findName(callName),
                describeQualified(context, global, callName, toBuffer(applicable)));
            return nullptr;
        }

        return emitGenericDispatch(deferred[chosen], args, source, resultName);
    }

    if(selectedCount > 1) {
        context.diagnostics.error("ambiguous call to %@ - more than one class instance applies. Name one class here (%@)"_v,
                                  source, context.findName(callName),
                                  describeQualified(context, global, callName, toBuffer(applicable)));
        return nullptr;
    }

    if(!selectedCount) {
        // Nothing in the class half of the overload set fits. A plain function of this name is then
        // the only candidate left, and its own diagnostic - which argument did not fit, and what it
        // was declared as - says more than the list of types the classes would not take.
        if(direct) return emitPlain();

        Array<char> types;

        if(withoutInstanceCount) {
            for(Size i = 0; i < withoutInstance.args.size(); i++) {
                if(i) appendText(types, ", "_v);
                describeType(context, global, withoutInstance.args[i], types);
            }

            context.diagnostics.error("no instance of %@ for (%@), required by %@"_v, source,
                                      context.findName(global[withoutInstance.typeClass]->name),
                                      String(types.pointer(), types.size()), context.findName(callName));
        } else {
            for(Size i = 0; i < args.length; i++) {
                if(i) appendText(types, ", "_v);
                describeType(context, global, valueType(args[i]), types);
            }

            context.diagnostics.error("no class function %@ accepts (%@)"_v, source, context.findName(callName),
                                      String(types.pointer(), types.size()));
        }

        return nullptr;
    }

    auto implementation = local[selected.instance]->functions.get(local, selected.index);
    if(!implementation) {
        context.diagnostics.error("instance of %@ does not implement %@"_v, source,
                                  context.findName(global[selected.typeClass]->name), context.findName(callName));
        return nullptr;
    }

    return emitDirectCall(implementation, args, source, target, resultName);
}

/*
 * Generic calls.
 */

ModulePtr<Value> ExprResolver::emitGenericDispatch(ClassMatch& match, Buffer<ModulePtr<Value>> args,
                                                   LocationId source, StringId resultName) {
    auto env = functionGen(global, function);
    if(!env) {
        // Nothing outside a generic body has a type variable to be undecided about.
        context.diagnostics.error("internal: a class call was deferred outside a generic function"_v, source);
        return nullptr;
    }

    requireClass(module, function, match.typeClass, toBuffer(match.args), source);

    auto typeClass = global[match.typeClass];
    auto entry = typeClass->functions.get(global, match.index);
    auto signature = local[entry.fun];
    auto resultType = substituteType(module, signature->returnType, toBuffer(match.args), source);

    auto call = create<InstGenCall>(source, resultName, resultType, entry.fun, match.typeClass, match.index);
    for(auto argument: match.args) call->typeArgs.push(module.arena, argument);

    for(Size i = 0; i < args.length; i++) {
        auto declared = local[signature->args.get(local, i)]->type;
        auto expected = substituteType(module, declared, toBuffer(match.args), source);
        call->args.push(module.arena, convert(args[i], expected, source));
    }

    append(call);
    auto result = ref(call);
    if(isMemoryType(global, resultType)) call->local = function.addLocal(module, resultType, resultName, result);

    return result;
}

ModulePtr<Value> ExprResolver::emitGenericCall(ModulePtr<Function> callee, Buffer<ModulePtr<Value>> args,
                                               LocationId source, TypePtr target, StringId resultName) {
    auto generic = local[callee];
    auto calleeEnv = functionGen(global, *generic);

    if(!calleeEnv || generic->args.size() != args.length) {
        return emitDirectCall(callee, args, source, target, resultName);
    }

    Array<TypePtr> bindings;
    for(Size i = 0; i < calleeEnv->types.size(); i++) bindings.push(nullptr);

    // The same one-directional rule the classes use: the arguments decide, and the expected
    // result only fills in what they left open.
    for(Size i = 0; i < args.length; i++) {
        auto declared = local[generic->args.get(local, i)]->type;

        if(!bindPosition(declared, valueType(args[i]), bindings, true)) {
            context.diagnostics.error("argument %@ of %@ is %@, which does not fit %@"_v, source, U32(i + 1),
                                      context.findName(generic->name),
                                      describeType(context, global, valueType(args[i])),
                                      describeType(context, global, declared));
            return nullptr;
        }
    }

    if(target) {
        Array<TypePtr> withTarget;
        for(auto binding: bindings) withTarget.push(binding);

        if(bindPosition(generic->returnType, target, withTarget, false)) {
            for(Size i = 0; i < bindings.size(); i++) {
                if(!bindings[i]) bindings[i] = withTarget[i];
            }
        }
    }

    for(Size i = 0; i < bindings.size(); i++) {
        if(bindings[i]) continue;

        context.diagnostics.error("cannot infer type argument %@ of %@ here - give the expected type"_v, source,
                                  context.findName(global[calleeEnv->types.get(global, i)]->name),
                                  context.findName(generic->name));
        return nullptr;
    }

    Array<ModulePtr<Value>> converted;
    for(Size i = 0; i < args.length; i++) {
        auto declared = local[generic->args.get(local, i)]->type;
        converted.push(convert(args[i], substituteType(module, declared, toBuffer(bindings), source), source));
    }

    auto deferred = false;
    for(auto binding: bindings) deferred = deferred || isGeneric(global, binding);

    if(!deferred) {
        auto specialized = instantiateFunction(module, callee, toBuffer(bindings), source);
        if(!specialized) return nullptr;

        return emitDirectCall(specialized, toBuffer(converted), source, target, resultName);
    }

    auto env = functionGen(global, function);
    if(!env) {
        context.diagnostics.error("internal: a generic call was deferred outside a generic function"_v, source);
        return nullptr;
    }

    // The callee's requirements become this function's, expressed in this function's variables:
    // whoever instantiates this one has to prove them, because nobody else can. Its body is
    // resolved first, since that is what collects the ones its signature did not declare - a
    // forward reference would otherwise inherit a shorter list than the callee really has.
    resolveFunctionBody(*generic->module, *generic);

    for(auto constraint: calleeEnv->classes.contents(global)) {
        if(!constraint.typeClass) continue;

        Array<TypePtr> forwarded;
        for(auto argument: constraint.args.contents(global)) {
            forwarded.push(substituteType(module, argument, toBuffer(bindings), source));
        }

        requireClass(module, function, constraint.typeClass, toBuffer(forwarded), source);
    }

    auto resultType = substituteType(module, generic->returnType, toBuffer(bindings), source);
    auto call = create<InstGenCall>(source, resultName, resultType, callee, nullptr, 0);

    for(auto binding: bindings) call->typeArgs.push(module.arena, binding);
    for(auto value: converted) call->args.push(module.arena, value);

    append(call);
    auto result = ref(call);
    if(isMemoryType(global, resultType)) call->local = function.addLocal(module, resultType, resultName, result);

    return result;
}
