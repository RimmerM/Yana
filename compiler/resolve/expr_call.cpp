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
 */

static U8 operatorPrecedence(Module& module, StringId op) {
    auto found = findPrecedence(module, op);
    return found ? found.unwrap() : 0;
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
    auto direct = findFunction(module, call.callee.var, expr.source);
    auto callArgs = call.args;
    auto declared = direct && !local[direct]->gen && local[direct]->args.size() == callArgs.size();

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

    // A plain function shadows a class function of the same name, in keeping with a local
    // definition shadowing an imported one.
    if(auto direct = findFunction(module, callName, source)) {
        if(local[direct]->args.size() == args.length) {
            return local[direct]->gen ? emitGenericCall(direct, args, source, target, resultName)
                                      : emitDirectCall(direct, args, source, target, resultName);
        }

        context.diagnostics.error("%@ takes %@ arguments but was given %@"_v, source, context.findName(callName),
                                  U32(local[direct]->args.size()), U32(args.length));
        return nullptr;
    }

    Array<ClassFunRef> candidates;
    findClassFunctions(module, callName, source, candidates);

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

    for(auto& candidate: candidates) {
        ClassMatch match;
        if(!matchClassFun(candidate, args, target, match)) continue;

        auto isDeferred = false;
        for(auto argument: match.args) isDeferred = isDeferred || isGeneric(global, argument);

        if(isDeferred) {
            deferred.push(::move(match));
        } else if(match.instance) {
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
                "ambiguous call to %@ - more than one class applies, and the types that would decide are not known here"_v,
                source, context.findName(callName));
            return nullptr;
        }

        return emitGenericDispatch(deferred[chosen], args, source, resultName);
    }

    if(selectedCount > 1) {
        context.diagnostics.error("ambiguous call to %@ - more than one class instance applies"_v, source,
                                  context.findName(callName));
        return nullptr;
    }

    if(!selectedCount) {
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
