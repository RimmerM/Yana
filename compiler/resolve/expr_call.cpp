#include "expr.h"

/*
 * Calls, operators, and the temporary overload resolver.
 *
 * Every operator in the language is an ordinary call: the builtins are generated as real IR
 * functions (builtins.cpp) and selected by the same code that selects a user's function. That
 * is deliberately more machinery than a compiler-internal operator table would need, because it
 * is the shape typeclass dispatch will take - see Implementation-IR.md part 6.
 */

static U8 operatorPrecedence(Module& module, StringId op) {
    auto found = module.operatorPrecedence.get(op);
    return found ? found.unwrap() : 0;
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
            context.diagnostics.error("scalar operator must be a named operator"_v, node->op.source);
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

ModulePtr<Value> ExprResolver::resolvePrefix(const ast::Expr& expr, const ast::PrefixExpr& prefix, TypePtr target) {
    if(prefix.op.kind != ast::Expr::Var) {
        context.diagnostics.error("scalar prefix operator must be named"_v, prefix.op.source);
        return nullptr;
    }

    auto value = resolve(prefix.on, target);
    if(!value) return nullptr;

    ModulePtr<Value> args[] = { value };
    return emitCall(prefix.op.var, { args, 1 }, expr.source, target);
}

ModulePtr<Value> ExprResolver::resolveCall(const ast::Expr& expr, const ast::AppExpr& call, TypePtr target) {
    if(call.callee.kind != ast::Expr::Var) {
        context.diagnostics.error("function values and indirect calls are not available yet"_v, call.callee.source);
        return nullptr;
    }

    Array<ModulePtr<Value>> values;

    auto callArgs = call.args;

    for(auto arg: callArgs.contents(parse)) {
        if(arg.name) {
            context.diagnostics.error("named call arguments are not available in the aggregate resolver"_v, arg.value.source);
        }

        values.push(resolve(arg.value));
    }

    return emitCall(call.callee.var, toBuffer(values), expr.source, target);
}

// Selects a callee by name and argument types, then emits the call.
//
// The scoring rule is the placeholder Milestone 2 needs and no more: a candidate is viable when
// every argument either matches exactly or widens to the parameter, and the winner is the one
// that widens least. It exists because the builtins are overloaded on the five primitives; real
// overload resolution belongs with typeclasses.
ModulePtr<Value> ExprResolver::emitCall(StringId callName, Buffer<ModulePtr<Value>> args, LocationId source, TypePtr target, StringId resultName) {
    Array<ModulePtr<Function>> candidates;

    if(auto direct = module.functions.get(callName)) {
        if(!local[direct.unwrap()]->builtin) candidates.push(direct.unwrap());
    }

    for(auto overload: module.overloads.contents(local)) {
        if(overload.name == callName) candidates.push(overload.function);
    }

    ModulePtr<Function> selected = nullptr;
    U32 bestScore = maxLimit<U32>;
    auto ambiguous = false;

    for(auto candidate: candidates) {
        auto callee = local[candidate];
        if(callee->args.size() != args.length) continue;

        U32 score = 0;
        auto viable = true;

        for(Size i = 0; i < args.length; i++) {
            if(!args[i]) {
                viable = false;
                break;
            }

            auto from = valueType(args[i]);
            auto to = local[callee->args.get(local, i)]->type;
            if(sameType(from, to)) continue;

            if(!isNumeric(global, from) || !isNumeric(global, to) || numericRank(from) > numericRank(to)) {
                viable = false;
                break;
            }

            score += 1 + numericRank(to) - numericRank(from);
        }

        if(!viable) continue;

        if(score < bestScore) {
            selected = candidate;
            bestScore = score;
            ambiguous = false;
        } else if(score == bestScore && selected != candidate) {
            ambiguous = true;
        }
    }

    if(!selected) {
        context.diagnostics.error("no matching function %@"_v, source, context.findName(callName));
        return nullptr;
    }

    if(ambiguous) {
        context.diagnostics.error("ambiguous temporary builtin overload for %@"_v, source, context.findName(callName));
        return nullptr;
    }

    auto callee = local[selected];
    callee->used = true;

    auto call = create<InstCall>(source, resultName, callee->returnType, selected);

    for(Size i = 0; i < args.length; i++) {
        auto expected = local[callee->args.get(local, i)]->type;
        auto value = convert(args[i], expected, source);
        if(value) call->args.push(module.arena, value);
    }

    append(call);
    auto result = ref(call);

    // An aggregate result is returned through storage the caller provides, so it needs a local
    // for the same reason a constructed value does - see resolve/lower.cpp's Call case.
    if(isMemoryType(global, call->type)) {
        call->local = function.addLocal(module, call->type, resultName, result);
    }

    if(target && !isUnit(global, callee->returnType) && !isMemoryType(global, callee->returnType)) {
        return convert(result, target, source);
    }

    return result;
}
