#include <alloca.h>
#include "../parse/ast.h"
#include "expr.h"

static const int kIntMax = 2147483647;

auto eqHash = Context::nameHash("==", 2);
auto geHash = Context::nameHash(">=", 2);
auto leHash = Context::nameHash("<=", 2);

static OpProperties opInfo(Context* context, Module* module, Id name) {
    auto op = findOp(context, module, name);
    if(op) return *op;

    return OpProperties{9, Assoc::Left};
}

static ast::InfixExpr* reorder(Context* context, Module* module, ast::InfixExpr* ast, U32 min) {
    auto lhs = ast;
    while(lhs->rhs && lhs->rhs->type == ast::Expr::Infix && !lhs->ordered) {
        auto first = opInfo(context, module, lhs->op->name);
        if(first.precedence < min) break;

        auto rhs = (ast::InfixExpr*)lhs->rhs;
        auto second = opInfo(context, module, rhs->op->name);
        if(second.precedence > first.precedence || (second.precedence == first.precedence && second.associativity == Assoc::Right)) {
            lhs->rhs = reorder(context, module, rhs, second.precedence);
            if(lhs->rhs == rhs) {
                lhs->ordered = true;
                break;
            }
        } else {
            lhs->ordered = true;
            lhs->rhs = rhs->lhs;
            rhs->lhs = lhs;
            lhs = rhs;
        }
    }

    return lhs;
}

// Checks if the two provided types are the same. If not, it tries to implicitly convert to a common type.
static bool generalizeTypes(FunBuilder* b, Value*& lhs, Value*& rhs) {
    // Try to do an implicit conversion each way.
    auto prevBlock = b->block;
    b->block = lhs->block;

    if(auto v = implicitConvert(b, lhs, rhs->type, false, false)) {
        lhs = v;
        b->block = prevBlock;
        return true;
    } else {
        b->block = rhs->block;
        if(auto v = implicitConvert(b, rhs, lhs->type, false, false)) {
            rhs = v;
            b->block = prevBlock;
            return true;
        }
    }

    b->block = prevBlock;
    error(b, "cannot implicitly convert values to the same type", nullptr);

    return false;
}

static bool alwaysTrue(Value* v) {
    return v->kind == Value::ConstInt && ((ConstInt*)v)->value != 0;
}

static Field* findStaticField(Type* type, Id stringField, U32 intField) {
    if(type->kind == Type::Tup) {
        auto tup = (TupType*)type;

        if(stringField) {
            for(U32 i = 0; i < tup->count; i++) {
                if(tup->fields[i].name == stringField) {
                    return tup->fields + i;
                }
            }
        } else if(tup->count > intField) {
            return tup->fields + intField;
        }
    }

    return nullptr;
}

static Value* testStaticField(FunBuilder* b, Id name, Value* target, ast::Expr* ast) {
    auto targetType = canonicalType(target->type);
    Field* staticField = nullptr;

    switch(ast->type) {
        case ast::Expr::Var: {
            auto n = ((ast::VarExpr*)ast)->name;
            staticField = findStaticField(targetType, n, 0);
            break;
        }
        case ast::Expr::Lit: {
            auto lit = ((ast::LitExpr*)ast)->literal;
            if(lit.type == ast::Literal::String) {
                staticField = findStaticField(targetType, lit.s, 0);
            } else if(lit.type == ast::Literal::Int) {
                staticField = findStaticField(targetType, 0, (U32)lit.i);
            }
            break;
        }
    }

    if(!staticField) return nullptr;

    auto indices = (U32*)b->fun->module->memory.alloc(sizeof(U32));
    indices[0] = staticField->index;

    if(target->type->kind == Type::Ref) {
        return loadField(b->block, name, target, staticField->type, indices, 1);
    } else {
        return getField(b->block, name, target, staticField->type, indices, 1);
    }
}

Value* resolveMulti(FunBuilder* b, ast::MultiExpr* expr, Id name, bool used) {
    auto e = expr->exprs;
    if(used) {
        // Expressions that are part of a statement list are never used, unless they are the last in the list.
        Value* result = nullptr;
        while(e) {
            auto isLast = e->next == nullptr;
            result = resolveExpr(b, e->item, 0, isLast);
            e = e->next;
        }
        return result;
    } else {
        while(e) {
            resolveExpr(b, e->item, 0, false);
            e = e->next;
        }
        return nullptr;
    }
}

Value* resolveLit(FunBuilder* b, ast::Literal* lit, Id name, bool used) {
    switch(lit->type) {
        case ast::Literal::Int:
            return constInt(b->block, name, lit->i, lit->i > kIntMax ? &intTypes[IntType::Long] : &intTypes[IntType::Int]);
        case ast::Literal::Float:
            return constFloat(b->block, name, lit->f, &floatTypes[FloatType::F64]);
        case ast::Literal::Char:
            return nullptr;
        case ast::Literal::String: {
            auto string = b->context.find(lit->s);
            return constString(b->block, name, string.text, string.textLength);
        }
        case ast::Literal::Bool: {
            auto c = constInt(b->block, name, lit->b, &intTypes[IntType::Bool]);
            c->type = &intTypes[IntType::Bool];
            return c;
        }
    }

    return nullptr;
}

static Value* findVar(FunBuilder* b, Id name) {
    // Try to find a local variable or argument.
    auto value = b->block->findValue(name);
    if(!value) {
        // Try to find a global variable.
        value = findGlobal(&b->context, b->fun->module, name);
    }

    return value;
}

static Value* useValue(FunBuilder* b, Value* value, bool asRV) {
    if(!asRV && (value->type->kind == Type::Ref) && ((RefType*)value->type)->isLocal) {
        return load(b->block, 0, value);
    }

    if(!asRV && value->kind == Value::Global) {
        return load(b->block, 0, value);
    }

    return value;
}

Value* resolveVar(FunBuilder* b, ast::VarExpr* expr, bool asRV) {
    auto value = findVar(b, expr->name);

    // If the value doesn't exist, we create a placeholder to allow the rest of the code to be resolved.
    if(!value) {
        error(b, "identifier not found", expr);
        value = error(b->block, 0, &errorType);
    }

    return useValue(b, value, asRV);
}

static Value* resolveDynCall(FunBuilder* b, Value* callee, List<ast::TupArg>* argList, Id name) {
    auto funType = (FunType*)callee->type;
    auto argCount = (U32)funType->argCount;

    auto args = (Value**)b->mem.alloc(sizeof(Value*) * argCount);
    memset(args, 0, sizeof(Value*) * argCount);

    U32 i = 0;
    while(argList) {
        auto arg = argList->item;
        auto argIndex = i;
        bool found = true;

        if(arg.name) {
            found = false;
            for(U32 a = 0; a < argCount; a++) {
                auto fa = funType->args[a];
                if(arg.name == fa.name) {
                    argIndex = fa.index;
                    found = true;
                }
            }

            if(!found) {
                error(b, "function has no argument with this name", arg.value);
            }
        }

        if(found) {
            if(args[argIndex]) {
                error(b, "function argument specified more than once", arg.value);
            }

            args[argIndex] = resolveExpr(b, arg.value, 0, true);
        }

        i++;
        argList = argList->next;
    }

    // If the call used incorrect argument names this error may not trigger.
    // However, in that case we already have an error for the argument name.
    if(i != argCount) {
        error(b, "incorrect number of function arguments", nullptr);
    }

    // Check the argument types and perform implicit conversions if needed.
    for(i = 0; i < argCount; i++) {
        auto v = implicitConvert(b, args[i], funType->args[i].type, false, false);
        if(!v) {
            error(b, "incompatible type for function argument", nullptr);
        }

        args[i] = v;
    }

    return callDyn(b->block, name, callee, args, argCount);
}

static Function* resolveStaticFun(FunBuilder* b, Id funName, Value* fieldArg) {
    // TODO: Use field argument to find an instance for that type.
    auto fun = findFun(&b->context, b->fun->module, funName);
    if(!fun) {
        error(b, "no function found for this name", nullptr);
        return nullptr;
    }

    // Make sure the function definition is finished.
    resolveFun(&b->context, fun);
    return fun;
}

static Value* finishStaticCall(FunBuilder* b, Function* fun, Value** args, U32 count, Id name) {
    auto argCount = (U32)fun->args.size();

    // If the call used incorrect argument names this error may not trigger.
    // However, in that case we already have an error for the argument name.
    if(count != argCount) {
        error(b, "incorrect number of function arguments", nullptr);
    }

    // Check the argument types and perform implicit conversions if needed.
    for(U32 i = 0; i < argCount; i++) {
        auto v = implicitConvert(b, args[i], fun->args[i].type, false, false);
        if(v) {
            args[i] = v;
        } else {
            error(b, "incompatible type for function argument", nullptr);
            args[i] = error(b->block, 0, fun->args[i].type);
        }
    }

    // If the function is an intrinsic, we use that instead.
    if(fun->intrinsic) {
        return fun->intrinsic(b, args, argCount, name);
    } else {
        return call(b->block, name, fun, args, argCount);
    }
}

static Value* genStaticCall(FunBuilder* b, Id funName, Value** args, U32 count, Id name) {
    auto fun = resolveStaticFun(b, funName, nullptr);
    if(!fun) return nullptr;

    return finishStaticCall(b, fun, args, count, name);
}

static Value* resolveStaticCall(FunBuilder* b, Id funName, Value* firstArg, List<ast::TupArg>* argList, Id name) {
    auto fun = resolveStaticFun(b, funName, firstArg);
    if(!fun) return nullptr;

    auto argCount = (U32)fun->args.size();

    auto args = (Value**)b->mem.alloc(sizeof(Value*) * argCount);
    memset(args, 0, sizeof(Value*) * argCount);

    U32 i = 0;
    if(firstArg) {
        args[0] = firstArg;
        i++;
    }

    while(argList) {
        auto arg = argList->item;
        auto argIndex = i;
        bool found = true;

        if(arg.name) {
            found = false;
            for(U32 a = 0; a < argCount; a++) {
                auto fa = &fun->args[a];
                if(arg.name == fa->name) {
                    argIndex = fa->index;
                    found = true;
                    break;
                }
            }

            if(!found) {
                error(b, "function has no argument with this name", arg.value);
            }
        }

        if(found && argIndex < argCount) {
            if(args[argIndex]) {
                error(b, "function argument specified more than once", arg.value);
            }

            args[argIndex] = resolveExpr(b, arg.value, 0, true);
        }

        i++;
        argList = argList->next;
    }

    return finishStaticCall(b, fun, args, i, name);
}

Value* resolveApp(FunBuilder* b, ast::AppExpr* expr, Id name, bool used) {
    // If the operand is a field expression we need special handling, since there are several options:
    // - the field operand is an actual field of its target and has a function type, which we call.
    // - the field operand is not a field, and we produce a function call with the target as first parameter.
    if(expr->callee->type == ast::Expr::Field) {
        auto callee = (ast::FieldExpr*)expr->callee;
        auto target = resolveExpr(b, callee->target, 0, true);
        auto staticField = testStaticField(b, 0, target, callee->field);
        if(staticField) {
            Value* field = nullptr;
            if(staticField->type->kind == Type::Fun) {
                field = staticField;
            } else if(staticField->type->kind == Type::Ref && ((RefType*)staticField->type)->to->kind == Type::Fun) {
                field = useValue(b, field, false);
            }

            if(field) {
                return resolveDynCall(b, staticField, expr->args, name);
            }
        }

        // TODO: Handle array and maps field loads.
        if(callee->field->type == ast::Expr::Var) {
            return resolveStaticCall(b, ((ast::VarExpr*)callee->field)->name, target, expr->args, name);
        } else {
            error(b, "field is not a function type", callee->field);
            return nullptr;
        }
    } else if(expr->callee->type == ast::Expr::Var) {
        auto callee = (ast::VarExpr*)expr->callee;

        // If this is a variable of function type, call it.
        auto var = findVar(b, callee->name);
        if(var && var->type->kind == Type::Fun) {
            return resolveDynCall(b, var, expr->args, name);
        } else if(var && var->type->kind == Type::Ref && ((RefType*)var->type)->to->kind == Type::Fun) {
            var = useValue(b, var, false);
            return resolveDynCall(b, var, expr->args, name);
        }

        // Otherwise, look for a globally defined function.
        return resolveStaticCall(b, callee->name, nullptr, expr->args, name);
    } else {
        auto callee = resolveExpr(b, expr->callee, 0, true);
        if(callee->type->kind == Type::Fun) {
            return resolveDynCall(b, callee, expr->args, name);
        } else {
            error(b, "callee is not a function type", expr->callee);
            return nullptr;
        }
    }
}

Value* resolveFun(FunBuilder* b, ast::FunExpr* expr, Id name, bool used) {
    return nullptr;
}

Value* resolveInfix(FunBuilder* b, ast::InfixExpr* unordered, Id name, bool used) {
    ast::InfixExpr* ast;
    if(unordered->ordered) {
        ast = unordered;
    } else {
        ast = reorder(&b->context, b->fun->module, unordered, 0);
    }

    // Create a temporary app-expression to resolve the operator as a function call.
    List<ast::TupArg> lhs(ast::TupArg(0, ast->lhs));
    List<ast::TupArg> rhs(ast::TupArg(0, ast->rhs));
    lhs.next = &rhs;

    ast::AppExpr app(ast->op, &lhs);
    app.locationFrom(*ast);

    return resolveApp(b, &app, name, used);
}

Value* resolvePrefix(FunBuilder* b, ast::PrefixExpr* expr, Id name, bool used) {
    // Create a temporary app-expression to resolve the operator as a function call.
    List<ast::TupArg> arg(ast::TupArg(0, expr->dst));
    ast::AppExpr app(expr->op, &arg);
    app.locationFrom(*expr);

    return resolveApp(b, &app, name, used);
}

Value* resolveIf(FunBuilder* b, ast::IfExpr* expr, Id name, bool used) {
    auto cond = resolveExpr(b, expr->cond, 0, true);
    auto condBlock = b->block;
    if(cond->type != &intTypes[IntType::Bool]) {
        error(b, "if condition must be a boolean", expr);
    }

    auto then = block(b->fun);
    then->preceding = condBlock;

    auto otherwise = block(b->fun);
    otherwise->preceding = condBlock;

    je(b->block, cond, then, otherwise);
    b->block = then;

    auto thenValue = resolveExpr(b, expr->then, 0, used);
    auto thenBlock = b->block;

    Value* elseValue = nullptr;
    Block* elseBlock = nullptr;
    if(expr->otherwise) {
        b->block = otherwise;
        elseValue = resolveExpr(b, expr->otherwise, 0, used);
        elseBlock = b->block;
    }

    if(used) {
        auto after = block(b->fun);
        after->preceding = condBlock;
        b->block = after;

        if(!elseValue || !elseBlock || elseBlock->complete || thenBlock->complete) {
            error(b, "if expression doesn't produce a result in every case", expr);
            return phi(after, name, nullptr, 0);
        }

        // This updates thenValue or elseValue if needed.
        if(!generalizeTypes(b, thenValue, elseValue)) {
            error(b, "if and else branches produce differing types", expr);
        }

        jmp(thenBlock, after);
        jmp(elseBlock, after);

        auto alts = (InstPhi::Alt*)b->mem.alloc(sizeof(InstPhi::Alt) * 2);
        alts[0].value = thenValue;
        alts[0].fromBlock = thenValue->block;
        alts[1].value = elseValue;
        alts[1].fromBlock = elseValue->block;

        return phi(after, name, alts, 2);
    } else {
        if(!elseBlock) {
            jmp(thenBlock, otherwise);
            b->block = otherwise;
        } else if(!(thenBlock->complete && elseBlock->complete)) {
            auto after = block(b->fun);
            after->preceding = condBlock;
            b->block = after;
            jmp(thenBlock, after);
            jmp(elseBlock, after);
        }

        return nullptr;
    }
}

Value* resolveMultiIf(FunBuilder* b, ast::MultiIfExpr* expr, Id name, bool used) {
    // Calculate the number of cases.
    U32 caseCount = 0;
    auto c = expr->cases;
    while(c) {
        caseCount++;
        c = c->next;
    }

    auto preceding = b->block;
    auto alts = (InstPhi::Alt*)b->fun->module->memory.alloc(sizeof(InstPhi::Alt) * caseCount);
    bool hasElse = false;

    c = expr->cases;
    for(U32 i = 0; i < caseCount; i++) {
        auto item = c->item;
        auto cond = resolveExpr(b, item.cond, 0, true);
        if(cond->type != &intTypes[IntType::Bool]) {
            error(b, "if condition must be a boolean", expr);
        }

        if(alwaysTrue(cond)) {
            auto result = resolveExpr(b, item.then, 0, used);
            alts[i].value = result;
            alts[i].fromBlock = b->block;

            b->block = block(b->fun);
            b->block->preceding = preceding;
            hasElse = true;
            break;
        } else {
            auto then = block(b->fun);
            auto next = block(b->fun);
            then->preceding = preceding;
            next->preceding = preceding;

            je(b->block, cond, then, next);
            b->block = then;

            auto result = resolveExpr(b, item.then, 0, used);
            alts[i].value = result;
            alts[i].fromBlock = b->block;

            b->block = next;
        }

        c = c->next;
    }

    auto next = b->block;
    if(used) {
        if(!hasElse) {
            error(b, "if expression doesn't produce a result in every case", expr);
        }

        for(U32 i = 0; i < caseCount; i++) {
            if(!alts[i].value) {
                error(b, "if expression doesn't produce a result in every case", expr);
            }

            if(i > 0) {
                // This will update the value stored in the alt if needed.
                if(!generalizeTypes(b, alts[i - 1].value, alts[i].value)) {
                    error(b, "if and else branches produce differing types", expr);
                }
            }

            jmp(alts[i].fromBlock, next);
        }

        return phi(next, name, alts, caseCount);
    } else {
        // If each case returns, we don't need a block afterwards.
        U32 returnCount = 0;
        for(U32 i = 0; i < caseCount; i++) {
            if(alts[i].fromBlock->returns) {
                returnCount++;
            }
        }

        if(returnCount == caseCount) {
            Size blockIndex = 0;
            for(Size i = 0; i < b->fun->blocks.size(); i++) {
                if(b->fun->blocks[i] == next) break;
                blockIndex++;
            }

            b->fun->blocks.remove(blockIndex);
            b->block = *b->fun->blocks.back();
        } else {
            for(U32 i = 0; i < caseCount; i++) {
                if(!alts[i].fromBlock->complete) {
                    jmp(alts[i].fromBlock, next);
                }
            }
        }

        return nullptr;
    }
}

Value* resolveDecl(FunBuilder* b, ast::DeclExpr* expr, Id name, bool used) {
    auto content = expr->content;
    if(!content) {
        error(b, "variables must be initialized on declaration", expr);
        return nullptr;
    }

    // If the declaration is global, we simply never define the name in the current scope.
    // That way, any uses will automatically use the global version, while local overrides, etc work normally.
    // The declaration is instead resolved as a store into the global.
    if(expr->isGlobal) {
        // The globals for each module are defined before expressions are resolved.
        Global* global = b->fun->module->globals.get(expr->name);
        assert(global != nullptr);

        // Resolve the contents - in case of an error below we want to do as much as possible.
        auto value = resolveExpr(b, expr->content, 0, true);

        // If the global has no ast set, it has been defined twice.
        if(!global->ast) {
            error(b, "duplicate definition of global variable", expr);
            return nullptr;
        }

        switch(expr->mut) {
            case ast::DeclExpr::Immutable:
            case ast::DeclExpr::Val: {
                if(value->type->kind == Type::Ref) {
                    global->type = getRef(b->fun->module, ((RefType*)value->type)->to, false, false, true);
                    auto v = load(b->block, 0, value);
                    store(b->block, 0, global, v);
                } else {
                    global->type = getRef(b->fun->module, value->type, false, false, true);
                    store(b->block, 0, global, value);
                }
                break;
            }
            case ast::DeclExpr::Ref: {
                global->type = getRef(b->fun->module, value->type, false, false, true);
                store(b->block, 0, global, value);
                break;
            }
        }

        if(expr->in) {
            resolveExpr(b, expr->in, 0, false);
        }

        global->ast = nullptr;
        return nullptr;
    } else {
        Value* result;
        switch(expr->mut) {
            case ast::DeclExpr::Immutable: {
                // Immutable values are stored as registers, so we just have to resolve the creation expression.
                result = resolveExpr(b, expr->content, expr->name, true);
                break;
            }
            case ast::DeclExpr::Val: {
                auto value = resolveExpr(b, expr->content, 0, true);
                if(value->type->kind == Type::Ref) {
                    auto var = alloc(b->block, expr->name, ((RefType*)value->type)->to, true, true);
                    auto v = load(b->block, 0, value);
                    store(b->block, 0, var, v);
                    result = var;
                    break;
                } else {
                    auto var = alloc(b->block, expr->name, value->type, true, true);
                    store(b->block, 0, var, value);
                    result = var;
                    break;
                }
            }
            case ast::DeclExpr::Ref: {
                auto value = resolveExpr(b, expr->content, 0, true);
                auto var = alloc(b->block, expr->name, value->type, true, false);
                store(b->block, 0, var, value);
                result = var;
                break;
            }
        }

        if(expr->in) {
            return resolveExpr(b, expr->in, name, used);
        } else {
            return result;
        }
    }
}

Value* resolveWhile(FunBuilder* b, ast::WhileExpr* expr) {
    auto preceding = b->block;
    auto condBlock = block(b->fun);
    jmp(b->block, condBlock);
    condBlock->preceding = b->block;
    b->block = condBlock;

    auto cond = resolveExpr(b, expr->cond, 0, true);
    if(cond->type != &intTypes[IntType::Bool]) {
        error(b, "while condition must be a boolean", expr);
    }

    auto bodyBlock = block(b->fun);
    auto exitBlock = block(b->fun);
    bodyBlock->preceding = b->block;
    exitBlock->preceding = preceding;

    je(b->block, cond, bodyBlock, exitBlock);
    b->block = bodyBlock;

    resolveExpr(b, expr->loop, 0, false);
    jmp(b->block, condBlock);

    b->block = exitBlock;
    return nullptr;
}

Value* resolveFor(FunBuilder* b, ast::ForExpr* expr) {
    // The starting value is only evaluated once.
    auto from = resolveExpr(b, expr->from, 0, true);
    Value* step = implicitConvert(b, expr->step ? (
        resolveExpr(b, expr->step, 0, true)
    ) : (
        constInt(b->block, 0, 1, from->type)
    ), from->type, false, true);

    if(from->type->kind != Type::Int || step->type->kind != Type::Int) {
        error(b, "only integer arguments are implemented for for loops", expr->from);
    }

    auto startBlock = b->block;

    // Create the starting block for each loop part.
    auto condBlock = block(b->fun);
    auto loopBlock = block(b->fun);
    auto endBlock = block(b->fun);
    condBlock->preceding = startBlock;
    loopBlock->preceding = condBlock;
    endBlock->preceding = startBlock;

    // Evaluate the loop condition.
    jmp(startBlock, condBlock);
    b->block = condBlock;

    // The condition we look at here depends on the body block, but the body hasn't been resolved yet.
    // We create a dummy instruction here and update the incoming blocks later.
    auto alts = (InstPhi::Alt*)b->mem.alloc(sizeof(InstPhi::Alt) * 2);
    alts[0].value = from;
    alts[0].fromBlock = startBlock;
    alts[1].value = nullptr;
    alts[1].fromBlock = nullptr;

    auto var = phi(b->block, expr->var, alts, 2);

    // Evaluate the end for each iteration, since it could be changed inside the loop.
    auto to = implicitConvert(b, resolveExpr(b, expr->to, 0, true), from->type, false, true);
    if(to->type->kind != Type::Int) {
        error(b, "only integer arguments are implemented for for loops", expr->from);
    }

    auto cmp = expr->reverse ? ICmp::gt : ICmp::lt;
    auto comp = icmp(b->block, 0, var, to, cmp);
    je(b->block, comp, loopBlock, endBlock);

    // Evaluate the loop body. At the end we create the loop variable for the next iteration.
    b->block = loopBlock;
    resolveExpr(b, expr->body, 0, false);
    auto loopEnd = b->block;

    Value* nextVar;
    if(expr->reverse) {
        nextVar = sub(loopEnd, 0, var, step);
    } else {
        nextVar = add(loopEnd, 0, var, step);
    }

    jmp(loopEnd, condBlock);

    // Update the phi-node we created earlier.
    alts[1].value = nextVar;
    alts[1].fromBlock = loopEnd;
    var->usedValues[1] = nextVar;
    condBlock->use(nextVar, var);
    b->block = endBlock;

    return nullptr;
}

Value* resolveAssign(FunBuilder* b, ast::AssignExpr* expr) {
    auto target = expr->target;
    switch(target->type) {
        case ast::Expr::Var: {
            auto var = resolveVar(b, (ast::VarExpr*)target, true);
            auto val = resolveExpr(b, expr->value, 0, true);

            if(!var || var->type->kind != Type::Ref || !((RefType*)var->type)->isMutable) {
                error(b, "type is not assignable", target);
                return nullptr;
            }

            auto targetType = ((RefType*)var->type)->to;
            auto v = implicitConvert(b, val, targetType, false, true);
            return store(b->block, 0, var, v);
        }
        case ast::Expr::Field: {

        }
        default: {
            error(b, "assign target is not assignable", target);
        }
    }

    return nullptr;
}

Value* resolveNested(FunBuilder* b, ast::NestedExpr* expr, Id name, bool used) {
    return resolveExpr(b, expr->expr, name, used);
}

Value* resolveCoerce(FunBuilder* b, ast::CoerceExpr* expr, Id name, bool used) {
    auto target = resolveExpr(b, expr->target, 0, used);
    // TODO: Use function GenContext.
    auto type = resolveType(&b->context, b->fun->module, expr->kind, nullptr);
    auto result = implicitConvert(b, target, type, true, true);
    setName(result, name);
    return result;
}

Value* resolveField(FunBuilder* b, ast::FieldExpr* expr, Id name, bool used) {
    auto target = resolveExpr(b, expr->target, 0, true);
    auto field = testStaticField(b, name, target, expr->field);
    if(field) return field;

    // TODO: Handle array and map loads.
    error(b, "type does not contain the requested field", expr->target);
    return error(b->block, name, &errorType);
}

Value* resolveTup(FunBuilder* b, ast::TupExpr* expr, Id name) {
    U32 argCount = 0;
    auto arg = expr->args;
    while(arg) {
        argCount++;
        arg = arg->next;
    }

    auto args = (Value**)b->mem.alloc(sizeof(Value*) * argCount);
    auto fields = (Field*)alloca(sizeof(Field) * argCount);

    arg = expr->args;
    for(U32 i = 0; i < argCount; i++) {
        args[i] = resolveExpr(b, arg->item.value, 0, true);
        fields[i].type = args[i]->type;
        fields[i].name = arg->item.name;
        arg = arg->next;
    }

    auto type = resolveTupType(&b->context, b->fun->module, fields, argCount);
    return tup(b->block, name, type, args, argCount);
}

Value* resolveTupUpdate(FunBuilder* b, ast::TupUpdateExpr* expr, Id name, bool used) {
    auto target = resolveExpr(b, expr->value, 0, true);
    auto type = canonicalType(target->type);
    if(type->kind != Type::Tup) {
        error(b, "only tuples can update their fields", expr->value);
    }

    U32 argCount = 0;
    auto arg = expr->args;
    while(arg) {
        argCount++;
        arg = arg->next;
    }

    auto tup = (TupType*)type;
    auto fields = (InstUpdateField::Field*)b->mem.alloc(sizeof(InstUpdateField::Field) * argCount);
    arg = expr->args;
    U32 fieldCount = 0;

    for(U32 i = 0; i < argCount; i++) {
        bool found = false;
        for(U32 f = 0; f < tup->count; f++) {
            if(tup->fields[f].name && tup->fields[f].name == arg->item.name) {
                fields[fieldCount].index = f;
                fields[fieldCount].value = resolveExpr(b, arg->item.value, 0, true);
                fieldCount++;
                found = true;
                break;
            }
        }

        if(!found) {
            error(b, "field does not exist in this type", arg->item.value);
        }
        arg = arg->next;
    }

    return updateField(b->block, name, target, fields, fieldCount);
}

Value* resolveArray(FunBuilder* b, ast::ArrayExpr* expr, Id name) {
    U32 length = 0;
    auto arg = expr->args;
    while(arg) {
        length++;
        arg = arg->next;
    }

    if(length == 0) {
        error(b, "cannot infer type of array", expr);
        return error(b->block, name, getArray(b->fun->module, &unitType));
    }

    auto values = (Value**)b->mem.alloc(sizeof(Value*) * length);

    arg = expr->args;
    for(U32 i = 0; i < length; i++) {
        values[i] = resolveExpr(b, arg->item, 0, true);
        if(i > 0) {
            // This will update the value stored in the alt if needed.
            if(!generalizeTypes(b, values[i - 1], values[i])) {
                error(b, "array contents must have the same type", expr);
            }
        }

        arg = arg->next;
    }

    auto array = allocArray(b->block, name, values[0]->type, constInt(b->block, 0, length, &intTypes[IntType::Int]), false, false);
    storeArray(b->block, 0, array, constInt(b->block, 0, 0, &intTypes[IntType::Int]), values, length, false);
    return array;
}

Value* resolveMap(FunBuilder* b, ast::MapExpr* expr, Id name, bool used) {
    return nullptr;
}

Value* resolveFormat(FunBuilder* b, ast::FormatExpr* expr, Id name, bool used) {
    return nullptr;
}

Value* resolveCase(FunBuilder* b, ast::CaseExpr* expr, Id name, bool used) {
    auto pivot = resolveExpr(b, expr->pivot, 0, true);
    auto alt = expr->alts;

    auto preceding = b->block;
    auto after = block(b->fun);
    after->preceding = b->block;

    U32 altCount = 0;
    while(alt) {
        altCount++;
        alt = alt->next;
    }

    auto alts = (InstPhi::Alt*)b->fun->module->memory.alloc(sizeof(InstPhi::Alt) * altCount);
    bool hasElse = false;

    alt = expr->alts;
    for(U32 i = 0; i < altCount; i++) {
        auto result = resolvePat(b, pivot, alt->item.pat);

        if(alwaysTrue(result)) {
            alts[i].value = resolveExpr(b, alt->item.expr, 0, true);
            alts[i].fromBlock = b->block;

            hasElse = true;
            break;
        } else {
            auto then = block(b->fun);
            then->preceding = preceding;

            auto otherwise = alt->next ? block(b->fun) : after;
            otherwise->preceding = preceding;

            je(b->block, result, then, otherwise);

            b->block = then;
            alts[i].value = resolveExpr(b, alt->item.expr, 0, true);
            alts[i].fromBlock = b->block;

            b->block = otherwise;
        }

        alt = alt->next;
    }

    if(used) {
        // TODO: If there is no else-case, we have to analyze the alts to make sure that every possibility is covered.
        if(!hasElse) {
            error(b, "match expression doesn't produce a result in every case", expr);
        }

        for(U32 i = 0; i < altCount; i++) {
            if(!alts[i].value) {
                error(b, "if expression doesn't produce a result in every case", expr);
            }

            if(i > 0) {
                // This will update the value stored in the alt if needed.
                if(!generalizeTypes(b, alts[i - 1].value, alts[i].value)) {
                    error(b, "match cases produce differing types", expr);
                }
            }

            jmp(alts[i].fromBlock, after);
        }

        b->block = after;
        return phi(after, name, alts, altCount);
    } else {
        // If each case returns, we don't need a block afterwards.
        U32 returnCount = 0;
        for(U32 i = 0; i < altCount; i++) {
            if(alts[i].fromBlock->returns) {
                returnCount++;
            }
        }

        if(returnCount == altCount) {
            Size blockIndex = 0;
            for(Size i = 0; i < b->fun->blocks.size(); i++) {
                if(b->fun->blocks[i] == after) break;
                blockIndex++;
            }

            b->fun->blocks.remove(blockIndex);
            b->block = *b->fun->blocks.back();
        } else {
            for(U32 i = 0; i < altCount; i++) {
                if(!alts[i].fromBlock->complete) {
                    jmp(alts[i].fromBlock, after);
                }
            }
        }

        b->block = after;
        return nullptr;
    }
}

Value* resolveRet(FunBuilder* b, ast::RetExpr* expr) {
    // The expression needs to be resolved before the current block is loaded.
    auto value = resolveExpr(b, expr->value, 0, true);
    return ret(b->block, value);
}

Value* resolveExpr(FunBuilder* b, ast::Expr* expr, Id name, bool used) {
    switch(expr->type) {
        case ast::Expr::Error:
            return nullptr;
        case ast::Expr::Multi:
            return resolveMulti(b, (ast::MultiExpr*)expr, name, used);
        case ast::Expr::Lit:
            return resolveLit(b, &((ast::LitExpr*)expr)->literal, name, used);
        case ast::Expr::Var:
            return resolveVar(b, (ast::VarExpr*)expr, false);
        case ast::Expr::App:
            return resolveApp(b, (ast::AppExpr*)expr, name, used);
        case ast::Expr::Fun:
            return resolveFun(b, (ast::FunExpr*)expr, name, used);
        case ast::Expr::Infix:
            return resolveInfix(b, (ast::InfixExpr*)expr, name, used);
        case ast::Expr::Prefix:
            return resolvePrefix(b, (ast::PrefixExpr*)expr, name, used);
        case ast::Expr::If:
            return resolveIf(b, (ast::IfExpr*)expr, name, used);
        case ast::Expr::MultiIf:
            return resolveMultiIf(b, (ast::MultiIfExpr*)expr, name, used);
        case ast::Expr::Decl:
            return resolveDecl(b, (ast::DeclExpr*)expr, name, used);
        case ast::Expr::While:
            return resolveWhile(b, (ast::WhileExpr*)expr);
        case ast::Expr::For:
            return resolveFor(b, (ast::ForExpr*)expr);
        case ast::Expr::Assign:
            return resolveAssign(b, (ast::AssignExpr*)expr);
        case ast::Expr::Nested:
            return resolveNested(b, (ast::NestedExpr*)expr, name, used);
        case ast::Expr::Coerce:
            return resolveCoerce(b, (ast::CoerceExpr*)expr, name, used);
        case ast::Expr::Field:
            return resolveField(b, (ast::FieldExpr*)expr, name, used);
        case ast::Expr::Con:
            return resolveCon(b, (ast::ConExpr*)expr, name);
        case ast::Expr::Tup:
            return resolveTup(b, (ast::TupExpr*)expr, name);
        case ast::Expr::TupUpdate:
            return resolveTupUpdate(b, (ast::TupUpdateExpr*)expr, name, used);
        case ast::Expr::Array:
            return resolveArray(b, (ast::ArrayExpr*)expr, name);
        case ast::Expr::Map:
            return resolveMap(b, (ast::MapExpr*)expr, name, used);
        case ast::Expr::Format:
            return resolveFormat(b, (ast::FormatExpr*)expr, name, used);
        case ast::Expr::Case:
            return resolveCase(b, (ast::CaseExpr*)expr, name, used);
        case ast::Expr::Ret:
            return resolveRet(b, (ast::RetExpr*)expr);
    }

    return nullptr;
}

Value* resolveVarPat(FunBuilder* b, Value* pivot, ast::VarPat* pat) {
    auto var = findVar(b, pat->var);
    if(var) {
        ast::VarExpr cmp(pat->var);
        List<ast::TupArg> arg(ast::TupArg(0, &cmp));
        auto call = resolveStaticCall(b, eqHash, pivot, &arg, 0);
        if(!call || call->type != &intTypes[IntType::Bool]) {
            if(!call || call->type->kind != Type::Error) {
                error(b, "result of a comparison must be a boolean", pat);
            }
            return constInt(b->block, 0, 0, &intTypes[IntType::Bool]);
        }

        return call;
    } else {
        b->block->namedValues[pat->var] = pivot;
        return constInt(b->block, 0, 1, &intTypes[IntType::Bool]);
    }
}

Value* resolveLitPat(FunBuilder* b, Value* pivot, ast::LitPat* pat) {
    ast::LitExpr lit(pat->lit);
    List<ast::TupArg> arg(ast::TupArg(0, &lit));
    auto call = resolveStaticCall(b, eqHash, pivot, &arg, 0);
    if(!call || call->type != &intTypes[IntType::Bool]) {
        if(!call || call->type->kind != Type::Error) {
            error(b, "result of a comparison must be a boolean", pat);
        }
        return constInt(b->block, 0, 0, &intTypes[IntType::Bool]);
    }

    return call;
}

Value* resolveAnyPat(FunBuilder* b, Value* pivot, ast::Pat* pat) {
    return constInt(b->block, 0, 1, &intTypes[IntType::Bool]);
}

Value* resolveTupPat(FunBuilder* b, Value* pivot, ast::TupPat* pat) {
    return constInt(b->block, 0, 0, &intTypes[IntType::Bool]);
}

Value* resolveConPat(FunBuilder* b, Value* pivot, ast::ConPat* pat) {
    auto con = findCon(&b->context, b->fun->module, pat->constructor);
    if(!compareTypes(&b->context, con->parent, pivot->type)) {
        error(b, "constructor type is incompatible with pivot type", pat);
        return constInt(b->block, 0, 0, &intTypes[IntType::Bool]);
    }

    auto intType = &intTypes[IntType::Int];
    auto indices = (U32*)b->mem.alloc(sizeof(U32) * 1);
    indices[0] = 0;

    Value* pivotCon;
    if(pivot->type->kind == Type::Ref) {
        pivotCon = loadField(b->block, 0, pivot, intType, indices, 1);
    } else {
        pivotCon = getField(b->block, 0, pivot, intType, indices, 1);
    }

    auto equal = icmp(b->block, 0, pivotCon, constInt(b->block, 0, con->index, intType), ICmp::eq);
    return equal;
}

Value* resolveArrPat(FunBuilder* b, Value* pivot, ast::ArrayPat* pat) {
    return constInt(b->block, 0, 0, &intTypes[IntType::Bool]);
}

Value* resolveRestPat(FunBuilder* b, Value* pivot, ast::RestPat* pat) {
    // Currently only used as part of an array pat.
    return constInt(b->block, 0, 0, &intTypes[IntType::Bool]);
}

static Value* getRangeArg(FunBuilder* b, ast::Pat* pat) {
    if(pat->kind == ast::Pat::Any) {
        return nullptr;
    } else if(pat->kind == ast::Pat::Var) {
        auto var = findVar(b, ((ast::VarPat*)pat)->var);
        if(var) {
            auto val = useValue(b, var, false);
            auto type = canonicalType(val->type);
            if(type->kind == Type::Int || type->kind == Type::Float) {
                return val;
            }
        }
    } else if(pat->kind == ast::Pat::Lit) {
        auto value = resolveLit(b, &((ast::LitPat*)pat)->lit, 0, true);
        if(value->type->kind == Type::Int || value->type->kind == Type::Float) {
            return value;
        }
    }

    error(b, "range patterns must use a variable or numeric literal", pat);
    return nullptr;
}

Value* resolveRangePat(FunBuilder* b, Value* pivot, ast::RangePat* pat) {
    auto from = getRangeArg(b, pat->from);
    auto to = getRangeArg(b, pat->to);

    if(!from && !to) {
        return constInt(b->block, 0, 1, &intTypes[IntType::Bool]);
    } else if(!from) {
        auto fromArgs = (Value**)b->mem.alloc(sizeof(Value) * 2);
        fromArgs[0] = pivot;
        fromArgs[1] = to;
        auto fromCmp = genStaticCall(b, leHash, fromArgs, 2, 0);

        if(!fromCmp || fromCmp->type != &intTypes[IntType::Bool]) {
            if(!fromCmp || fromCmp->type->kind != Type::Error) {
                error(b, "result of a comparison must be a boolean", pat);
            }
            return constInt(b->block, 0, 0, &intTypes[IntType::Bool]);
        }

        return fromCmp;
    } else if(!to) {
        auto toArgs = (Value**)b->mem.alloc(sizeof(Value) * 2);
        toArgs[0] = pivot;
        toArgs[1] = from;
        auto toCmp = genStaticCall(b, geHash, toArgs, 2, 0);

        if(!toCmp || toCmp->type != &intTypes[IntType::Bool]) {
            if(!toCmp || toCmp->type->kind != Type::Error) {
                error(b, "result of a comparison must be a boolean", pat);
            }
            return constInt(b->block, 0, 0, &intTypes[IntType::Bool]);
        }

        return toCmp;
    } else {
        auto fromArgs = (Value**)b->mem.alloc(sizeof(Value) * 2);
        fromArgs[0] = pivot;
        fromArgs[1] = from;
        auto fromCmp = genStaticCall(b, geHash, fromArgs, 2, 0);

        auto toArgs = (Value**)b->mem.alloc(sizeof(Value) * 2);
        toArgs[0] = pivot;
        toArgs[1] = to;
        auto toCmp = genStaticCall(b, leHash, toArgs, 2, 0);

        if(!fromCmp || !toCmp || fromCmp->type != &intTypes[IntType::Bool] || toCmp->type != &intTypes[IntType::Bool]) {
            if(!fromCmp || !toCmp || fromCmp->type->kind != Type::Error || toCmp->type->kind != Type::Error) {
                error(b, "result of a comparison must be a boolean", pat);
            }
            return constInt(b->block, 0, 0, &intTypes[IntType::Bool]);
        }

        return and_(b->block, 0, fromCmp, toCmp);
    }
}

Value* resolvePat(FunBuilder* b, Value* pivot, ast::Pat* pat) {
    switch(pat->kind) {
        case ast::Pat::Error:
            return constInt(b->block, 0, 0, &intTypes[IntType::Bool]);
        case ast::Pat::Var:
            return resolveVarPat(b, pivot, (ast::VarPat*)pat);
        case ast::Pat::Lit:
            return resolveLitPat(b, pivot, (ast::LitPat*)pat);
        case ast::Pat::Any:
            return resolveAnyPat(b, pivot, pat);
        case ast::Pat::Tup:
            return resolveTupPat(b, pivot, (ast::TupPat*)pat);
        case ast::Pat::Con:
            return resolveConPat(b, pivot, (ast::ConPat*)pat);
        case ast::Pat::Array:
            return resolveArrPat(b, pivot, (ast::ArrayPat*)pat);
        case ast::Pat::Rest:
            return resolveRestPat(b, pivot, (ast::RestPat*)pat);
        case ast::Pat::Range:
            return resolveRangePat(b, pivot, (ast::RangePat*)pat);
        default:
            return constInt(b->block, 0, 0, &intTypes[IntType::Bool]);
    }
}