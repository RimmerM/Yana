#include "expr.h"
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
    Array<ModulePtr<ClassInstance>> candidates;
    findInstances(module, typeClass, candidates);

    for(auto candidate: candidates) {
        auto instance = local[candidate];
        if(instance->forTypes.size() != args.length) continue;

        auto equal = true;
        for(Size i = 0; i < args.length; i++) {
            if(instance->forTypes.get(local, i) != args[i]) {
                equal = false;
                break;
            }
        }

        if(equal) return candidate;
    }

    return nullptr;
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
    // which `Maybe` it is building - a class function cannot do the same, because which types
    // its parameters have is exactly what the arguments are being resolved to decide.
    auto direct = findFunction(module, call.callee.var, expr.source);
    auto callArgs = call.args;
    auto declared = direct && local[direct]->args.size() == callArgs.size();

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
            return emitDirectCall(direct, args, source, target, resultName);
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

    for(auto& candidate: candidates) {
        ClassMatch match;
        if(!matchClassFun(candidate, args, target, match)) continue;

        if(match.instance) {
            if(!selectedCount) selected = ::move(match);
            selectedCount++;
        } else {
            if(!withoutInstanceCount) withoutInstance = ::move(match);
            withoutInstanceCount++;
        }
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
