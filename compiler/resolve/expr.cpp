#include <alloca.h>
#include "../parse/ast.h"
#include "expr.h"

static const int kIntMax = 2147483647;

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
    error(b, "cannot implicitly convert values to the same type"_buffer, nullptr);

    return false;
}

bool alwaysTrue(Value* v) {
    return v->kind == Value::ConstInt && ((ConstInt*)v)->value != 0;
}

bool alwaysFalse(Value* v) {
    return v->kind == Value::ConstInt && ((ConstInt*)v)->value == 0;
}

struct FieldResult {
    Field* field;
    Con* con;
};

static Field* findTupleField(TupType* type, Id stringField, U32 intField) {
    if(stringField) {
        for(U32 i = 0; i < type->count; i++) {
            if(type->fields[i].name == stringField) {
                return type->fields + i;
            }
        }
    } else if(type->count > intField) {
        return type->fields + intField;
    }

    return nullptr;
}

static FieldResult findStaticField(Type* type, Id stringField, U32 intField) {
    if(type->kind == Type::Tup) {
        return {findTupleField((TupType*)type, stringField, intField), nullptr};
    } else if(type->kind == Type::Record) {
        auto record = (RecordType*)type;
        if(record->kind == RecordType::Single) {
            auto con = &record->cons[0];
            if(con->content->kind == Type::Tup) {
                return {findTupleField((TupType*)con->content, stringField, intField), con};
            }
        }
    }

    return {nullptr, nullptr};
}

static Value* testStaticField(FunBuilder* b, Id name, Value* target, ast::Expr* ast) {
    auto targetType = rValueType(target->type);
    FieldResult result = {nullptr, nullptr};

    switch(ast->type) {
        case ast::Expr::Var: {
            auto n = ((ast::VarExpr*)ast)->name;
            result = findStaticField(targetType, n, 0);
            break;
        }
        case ast::Expr::Lit: {
            auto lit = ((ast::LitExpr*)ast)->literal;
            if(lit.type == ast::Literal::String) {
                result = findStaticField(targetType, lit.s, 0);
            } else if(lit.type == ast::Literal::Int) {
                result = findStaticField(targetType, 0, (U32)lit.i);
            }
            break;
        }
    }

    if(!result.field) return nullptr;

    if(result.con) {
        return getNestedField(b, target, name, result.con->index + 1, result.field->index, result.field->type);
    } else {
        return getField(b, target, name, result.field->index, result.field->type);
    }
}

Value* resolveMulti(FunBuilder* b, Type* targetType, ast::MultiExpr* expr, Id name, bool used) {
    auto e = expr->exprs;
    if(used || targetType) {
        // Expressions that are part of a statement list are never used, unless they are the last in the list.
        Value* result = nullptr;
        while(e) {
            auto isLast = e->next == nullptr;
            auto exprName = isLast ? name : 0;
            auto exprType = isLast ? targetType : nullptr;

            result = resolveExpr(b, exprType, e->item, exprName, used && isLast);
            e = e->next;
        }
        return result;
    } else {
        while(e) {
            resolveExpr(b, nullptr, e->item, 0, false);
            e = e->next;
        }
        return nullptr;
    }
}

Value* resolveLit(FunBuilder* b, Type* targetType, ast::Literal* lit, Id name) {
    Value* value;

    switch(lit->type) {
        case ast::Literal::Int:
            value = constInt(b->block, name, lit->i, lit->i > kIntMax ? &intTypes[IntType::Long] : &intTypes[IntType::Int]);
            break;
        case ast::Literal::Float:
            value = constFloat(b->block, name, lit->f, &floatTypes[FloatType::F64]);
            break;
        case ast::Literal::Char:
            value = constInt(b->block, name, lit->c, &intTypes[IntType::Int]);
            break;
        case ast::Literal::String: {
            auto string = b->context.find(lit->s);
            value = constString(b->block, name, string.text, string.textLength);
            break;
        }
        case ast::Literal::Bool:
            value = constInt(b->block, name, lit->b, &intTypes[IntType::Bool]);
            break;
    }

    if(targetType) {
        auto v = implicitConvert(b, value, targetType, true, false);
        if(v) value = v;
    }

    return value;
}

Value* findVar(FunBuilder* b, Id name) {
    // Try to find a local variable or argument.
    auto value = b->block->findValue(name);
    if(!value) {
        // Try to find a global variable.
        value = findGlobal(&b->context, b->fun->module, name);
    }

    return value;
}

Value* useValue(FunBuilder* b, Value* value, bool asRV) {
    if(!asRV && (value->type->kind == Type::Ref) && ((RefType*)value->type)->isLocal) {
        return load(b->block, 0, value);
    }

    if(!asRV && value->kind == Value::Global) {
        return load(b->block, 0, value);
    }

    return value;
}

static Value* getField(FunBuilder* b, Value* value, Id name, Type* type, U32* indices, U32 count) {
    if(value->type->kind == Type::Ref || value->kind == Value::Global) {
        return loadField(b->block, name, value, type, indices, count);
    } else {
        return getField(b->block, name, value, type, indices, count);
    }
}

Value* getField(FunBuilder* b, Value* value, Id name, U32 field, Type* type) {
    auto indices = (U32*)b->mem.alloc(sizeof(U32));
    indices[0] = field;
    return getField(b, value, name, type, indices, 1);
}

Value* getNestedField(FunBuilder* b, Value* value, Id name, U32 firstField, U32 secondField, Type* type) {
    auto indices = (U32*)b->mem.alloc(sizeof(U32) * 2);
    indices[0] = firstField;
    indices[1] = secondField;
    return getField(b, value, name, type, indices, 2);
}

Value* resolveVar(FunBuilder* b, ast::VarExpr* expr, bool asRV) {
    auto value = findVar(b, expr->name);

    // If the value doesn't exist, we create a placeholder to allow the rest of the code to be resolved.
    if(!value) {
        error(b, "identifier not found"_buffer, expr);
        value = error(b->block, 0, &errorType);
    }

    return useValue(b, value, asRV);
}

Value* resolveApp(FunBuilder* b, Type* targetType, ast::AppExpr* expr, Id name, bool used) {
    // If the operand is a field expression we need special handling, since there are several options:
    // - the field operand is an actual field of its target and has a function type, which we call.
    // - the field operand is not a field, and we produce a function call with the target as first parameter.
    if(expr->callee->type == ast::Expr::Field) {
        auto callee = (ast::FieldExpr*)expr->callee;
        auto target = resolveExpr(b, nullptr, callee->target, 0, true);
        auto staticField = testStaticField(b, 0, target, callee->field);
        if(staticField) {
            Value* field = nullptr;
            if(staticField->type->kind == Type::Fun) {
                field = staticField;
            } else if(staticField->type->kind == Type::Ref && ((RefType*)staticField->type)->to->kind == Type::Fun) {
                field = useValue(b, field, false);
            }

            if(field) {
                return resolveDynCall(b, targetType, staticField, expr->args, name);
            }
        }

        // TODO: Handle array and maps field loads.
        if(callee->field->type == ast::Expr::Var) {
            return resolveStaticCall(b, targetType, ((ast::VarExpr*)callee->field)->name, target, expr->args, name);
        } else {
            error(b, "field is not a function type"_buffer, callee->field);
            return nullptr;
        }
    } else if(expr->callee->type == ast::Expr::Var) {
        auto callee = (ast::VarExpr*)expr->callee;

        // If this is a variable of function type, call it.
        auto var = findVar(b, callee->name);
        if(var && var->type->kind == Type::Fun) {
            return resolveDynCall(b, targetType, var, expr->args, name);
        } else if(var && var->type->kind == Type::Ref && ((RefType*)var->type)->to->kind == Type::Fun) {
            var = useValue(b, var, false);
            return resolveDynCall(b, targetType, var, expr->args, name);
        }

        // Otherwise, look for a globally defined function.
        return resolveStaticCall(b, targetType, callee->name, nullptr, expr->args, name);
    } else {
        auto callee = resolveExpr(b, nullptr, expr->callee, 0, true);
        auto calleeType = canonicalType(callee->type);
        if(calleeType->kind == Type::Fun) {
            return resolveDynCall(b, targetType, callee, expr->args, name);
        } else {
            error(b, "callee is not a function type"_buffer, expr->callee);
            return nullptr;
        }
    }
}

Value* resolveFun(FunBuilder* b, ast::FunExpr* expr, Id name, bool used) {
    return nullptr;
}

Value* resolveInfix(FunBuilder* b, Type* targetType, ast::InfixExpr* unordered, Id name, bool used) {
    ast::InfixExpr* ast;
    if(unordered->ordered) {
        ast = unordered;
    } else {
        ast = reorder(&b->context, b->fun->module, unordered, 0);
    }

    // Create a temporary app-expression to resolve the operator as a function call.
    List<ast::TupArg> lhs(nullptr, ast::TupArg(0, ast->lhs));
    List<ast::TupArg> rhs(nullptr, ast::TupArg(0, ast->rhs));
    lhs.next = &rhs;

    ast::AppExpr app(ast->op, &lhs);
    app.locationFrom(*ast);

    return resolveApp(b, targetType, &app, name, used);
}

Value* resolvePrefix(FunBuilder* b, Type* targetType, ast::PrefixExpr* expr, Id name, bool used) {
    // Create a temporary app-expression to resolve the operator as a function call.
    List<ast::TupArg> arg(nullptr, ast::TupArg(0, expr->dst));
    ast::AppExpr app(expr->op, &arg);
    app.locationFrom(*expr);

    return resolveApp(b, targetType, &app, name, used);
}

Value* resolveIf(FunBuilder* b, Type* targetType, ast::IfExpr* expr, Id name, bool used) {
    Value* cond = resolveExpr(b, &intTypes[IntType::Bool], expr->cond, 0, true);

    auto condBlock = b->block;
    if(cond->type != &intTypes[IntType::Bool]) {
        error(b, "if condition must be a boolean"_buffer, expr);
    }

    auto then = block(b->fun);
    then->preceding = condBlock;

    auto otherwise = block(b->fun);
    otherwise->preceding = condBlock;

    je(b->block, cond, then, otherwise);
    b->block = then;

    auto thenValue = resolveExpr(b, targetType, expr->then, 0, used);
    auto thenBlock = b->block;

    Value* elseValue = nullptr;
    Block* elseBlock = nullptr;
    if(expr->otherwise) {
        b->block = otherwise;
        elseValue = resolveExpr(b, targetType, expr->otherwise, 0, used);
        elseBlock = b->block;
    }

    if(used) {
        auto after = block(b->fun);
        after->preceding = condBlock;
        b->block = after;

        if(!elseValue || !elseBlock || elseBlock->complete || thenBlock->complete) {
            error(b, "if expression doesn't produce a result in every case"_buffer, expr);
            return phi(after, name, nullptr, 0);
        }

        // This updates thenValue or elseValue if needed.
        if(!generalizeTypes(b, thenValue, elseValue)) {
            error(b, "if and else branches produce differing types"_buffer, expr);
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

Value* resolveMultiIf(FunBuilder* b, Type* targetType, ast::MultiIfExpr* expr, Id name, bool used) {
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

        Value* cond = resolveExpr(b, &intTypes[IntType::Bool], item.cond, 0, true);

        if(cond->type != &intTypes[IntType::Bool]) {
            error(b, "if condition must be a boolean"_buffer, expr);
        }

        if(alwaysTrue(cond)) {
            auto result = resolveExpr(b, targetType, item.then, 0, used);
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

            auto result = resolveExpr(b, targetType, item.then, 0, used);
            alts[i].value = result;
            alts[i].fromBlock = b->block;

            b->block = next;
        }

        c = c->next;
    }

    auto next = b->block;
    if(used) {
        if(!hasElse) {
            error(b, "if expression doesn't produce a result in every case"_buffer, expr);
        }

        for(U32 i = 0; i < caseCount; i++) {
            if(!alts[i].value) {
                error(b, "if expression doesn't produce a result in every case"_buffer, expr);
            }

            if(i > 0) {
                // This will update the value stored in the alt if needed.
                if(!generalizeTypes(b, alts[i - 1].value, alts[i].value)) {
                    error(b, "if and else branches produce differing types"_buffer, expr);
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

Value* resolveDecl(FunBuilder* b, Type* targetType, ast::VarDecl* expr, Id name, bool used, bool isGlobal) {
    auto content = expr->content;
    if(!content) {
        error(b, "variables must be initialized on declaration"_buffer, expr);
        return error(b->block, name, &unitType);
    }

    auto declName = getDeclName(expr);

    // If the declaration is global, we simply never define the name in the current scope.
    // That way, any uses will automatically use the global version, while local overrides, etc work normally.
    // The declaration is instead resolved as a store into the global.
    if(isGlobal && declName) {
        // The globals for each module are defined before expressions are resolved.
        Global* global = b->fun->module->globals.get(declName).unwrap();
        assertTrue(global != nullptr);

        // Resolve the contents - in case of an error below we want to do as much as possible.
        auto value = resolveExpr(b, nullptr, expr->content, 0, true);

        // If the global has no ast set, it has been defined twice.
        if(!global->ast) {
            error(b, "duplicate definition of global variable"_buffer, expr);
            return nullptr;
        }

        switch(expr->mut) {
            case ast::VarDecl::Immutable:
            case ast::VarDecl::Val: {
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
            case ast::VarDecl::Ref: {
                global->type = getRef(b->fun->module, value->type, false, false, true);
                store(b->block, 0, global, value);
                break;
            }
        }

        if(expr->in) {
            resolveExpr(b, nullptr, expr->in, 0, false);
        }

        global->ast = nullptr;
        return nullptr;
    } else {
        Value* result = nullptr;
        switch(expr->mut) {
            case ast::VarDecl::Immutable: {
                // Immutable values are stored as registers, so we just have to resolve the creation expression.
                result = resolveExpr(b, nullptr, expr->content, 0, true);
                break;
            }
            case ast::VarDecl::Val: {
                auto value = resolveExpr(b, nullptr, expr->content, 0, true);
                if(value->type->kind == Type::Ref) {
                    auto var = alloc(b->block, 0, ((RefType*)value->type)->to, true, true);
                    auto v = load(b->block, 0, value);
                    store(b->block, 0, var, v);
                    result = var;
                    break;
                } else {
                    auto var = alloc(b->block, 0, value->type, true, true);
                    store(b->block, 0, var, value);
                    result = var;
                    break;
                }
            }
            case ast::VarDecl::Ref: {
                auto value = resolveExpr(b, nullptr, expr->content, 0, true);
                auto var = alloc(b->block, 0, value->type, true, false);
                store(b->block, 0, var, value);
                result = var;
                break;
            }
        }

        auto otherwise = block(b->fun, true);
        MatchContext* context = nullptr;

        auto patResult = resolvePat(b, &context, otherwise, result, expr->pat);
        if(patResult <= 0) {
            if(patResult < 0) {
                b->context.diagnostics.warning("this pattern can never match"_buffer, expr->pat, noSource);
            }

            if(expr->alts) {
                auto then = b->block;
                auto alt = expr->alts;

                bool isComplete = false;
                while(alt) {
                    b->block = otherwise;
                    b->fun->blocks.push(otherwise);
                    otherwise = block(b->fun, true);

                    patResult = resolvePat(b, &context, otherwise, result, alt->item.pat);
                    if(patResult == 0) {
                        // Default case: resolve the expression for this match into the matching block.
                        // Continue with other matches in the failing block.
                        resolveExpr(b, nullptr, alt->item.expr, 0, true);
                        if(!b->block->complete) {
                            error(b, "declaration alternatives are not implemented yet"_buffer, alt->item.expr);
                        }
                    } else if(patResult == 1) {
                        // Pattern always matches. Resolve in the matching block, don't add the failing block.
                        resolveExpr(b, nullptr, alt->item.expr, 0, true);
                        if(!b->block->complete) {
                            error(b, "declaration alternatives are not implemented yet"_buffer, alt->item.expr);
                        }

                        isComplete = true;
                        break;
                    } else {
                        // Pattern never matches. Continue with other matches in the failing block.
                    }

                    alt = alt->next;
                }

                if(!isComplete) {
                    error(b, "declaration doesn't produce a result in every case"_buffer, expr);
                }

                b->block = then;
            } else {
                error(b, "this pattern may not succeed, but no alternative patterns were provided"_buffer, expr->pat);
            }
        } else if(expr->alts) {
            b->context.diagnostics.warning("this pattern will always match"_buffer, expr->pat, noSource);
        }

        if(expr->in) {
            return resolveExpr(b, targetType, expr->in, name, used);
        } else {
            return result;
        }
    }
}

Value* resolveDecl(FunBuilder* b, Type* targetType, ast::DeclExpr* expr, Id name, bool used) {
    auto e = expr->decls;
    if(used) {
        // Expressions that are part of a statement list are never used, unless they are the last in the list.
        Value* result = nullptr;
        while(e) {
            auto isLast = e->next == nullptr;
            result = resolveDecl(b, targetType, &e->item, 0, isLast, expr->isGlobal);
            e = e->next;
        }
        return result;
    } else {
        while(e) {
            resolveDecl(b, targetType, &e->item, 0, false, expr->isGlobal);
            e = e->next;
        }
        return nullptr;
    }
}

Value* resolveWhile(FunBuilder* b, ast::WhileExpr* expr, bool used) {
    auto preceding = b->block;
    auto condBlock = block(b->fun);
    auto exitBlock = block(b->fun, true);

    condBlock->loop = true;
    condBlock->preceding = b->block;
    condBlock->succeeding = exitBlock;

    jmp(b->block, condBlock);
    b->block = condBlock;

    Value* cond = resolveExpr(b, &intTypes[IntType::Bool], expr->cond, 0, true);
    if(cond->type != &intTypes[IntType::Bool]) {
        error(b, "while condition must be a boolean"_buffer, expr);
    }

    auto bodyBlock = block(b->fun);

    bodyBlock->preceding = b->block;
    exitBlock->preceding = preceding;

    je(b->block, cond, bodyBlock, exitBlock);
    b->block = bodyBlock;

    resolveExpr(b, nullptr, expr->loop, 0, false);
    jmp(b->block, condBlock);

    b->fun->blocks.push(exitBlock);
    b->block = exitBlock;

    if(used) {
        error(b, "while-blocks cannot be used as expressions"_buffer, expr);
        return error(b->block, 0, &unitType);
    } else {
        return nullptr;
    }
}

Value* resolveFor(FunBuilder* b, ast::ForExpr* expr, bool used) {
    // The starting value is only evaluated once.
    auto from = resolveExpr(b, nullptr, expr->from, 0, true);
    Value* step = implicitConvert(b, expr->step ? (
        resolveExpr(b, from->type, expr->step, 0, true)
    ) : (
        constInt(b->block, 0, 1, from->type)
    ), from->type, false, true);

    if(from->type->kind != Type::Int || step->type->kind != Type::Int) {
        error(b, "only integer arguments are implemented for for loops"_buffer, expr->from);
    }

    auto startBlock = b->block;

    // Create the starting block for each loop part.
    auto condBlock = block(b->fun);
    auto loopBlock = block(b->fun);
    auto endBlock = block(b->fun);

    condBlock->loop = true;
    condBlock->preceding = startBlock;
    condBlock->succeeding = endBlock;

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
    auto to = implicitConvert(b, resolveExpr(b, from->type, expr->to, 0, true), from->type, false, true);
    if(to->type->kind != Type::Int) {
        error(b, "only integer arguments are implemented for for loops"_buffer, expr->from);
    }

    auto cmp = expr->reverse ? ICmp::igt : ICmp::ilt;
    auto comp = icmp(b->block, 0, var, to, cmp);
    je(b->block, comp, loopBlock, endBlock);

    // Evaluate the loop body. At the end we create the loop variable for the next iteration.
    b->block = loopBlock;
    resolveExpr(b, nullptr, expr->body, 0, false);
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

    if(used) {
        error(b, "for-blocks cannot be used as expressions"_buffer, expr);
        return error(b->block, 0, &unitType);
    } else {
        return nullptr;
    }
}

Value* resolveAssign(FunBuilder* b, ast::AssignExpr* expr, bool used) {
    auto target = expr->target;
    switch(target->type) {
        case ast::Expr::Var: {
            auto var = resolveVar(b, (ast::VarExpr*)target, true);
            auto val = resolveExpr(b, var->type, expr->value, 0, true);

            if(var->type->kind != Type::Ref || !((RefType*)var->type)->isMutable) {
                error(b, "type is not assignable"_buffer, target);
                return error(b->block, 0, &unitType);
            }

            auto targetType = ((RefType*)var->type)->to;
            auto v = implicitConvert(b, val, targetType, false, true);
            return store(b->block, 0, var, v);
        }
        case ast::Expr::Field: {
            error(b, "field assignment is not implemented yet"_buffer, expr);
        }
        default: {
            error(b, "assign target is not assignable"_buffer, target);
        }
    }

    if(used) {
        error(b, "assignments cannot be used as expressions"_buffer, expr);
        return error(b->block, 0, &unitType);
    } else {
        return nullptr;
    }
}

Value* resolveNested(FunBuilder* b, Type* targetType, ast::NestedExpr* expr, Id name, bool used) {
    return resolveExpr(b, targetType, expr->expr, name, used);
}

Value* resolveCoerce(FunBuilder* b, ast::CoerceExpr* expr, Id name, bool used) {
    auto type = resolveType(&b->context, b->fun->module, expr->kind, &b->fun->gen);
    auto target = resolveExpr(b, type, expr->target, 0, used);

    auto result = implicitConvert(b, target, type, true, true);
    setName(result, name);
    return result;
}

Value* resolveField(FunBuilder* b, ast::FieldExpr* expr, Id name, bool used) {
    auto target = resolveExpr(b, nullptr, expr->target, 0, true);
    auto field = testStaticField(b, name, target, expr->field);
    if(field) return field;

    // TODO: Handle array and map loads.
    error(b, "type does not contain the requested field"_buffer, expr->target);
    return error(b->block, name, &errorType);
}

static void resetTupOrder(Value** args, Field* fields, U32* fieldIndices, U32 argCount) {
    auto orderedFields = (Field*)alloca(sizeof(Field) * argCount);
    auto orderedArgs = (Value**)alloca(sizeof(Value*) * argCount);

    for(U32 j = 0; j < argCount; j++) {
        if(fieldIndices[j] != -1) {
            orderedArgs[fieldIndices[j]] = args[j];
            orderedFields[fieldIndices[j]] = fields[j];
        } else {
            orderedArgs[j] = args[j];
            orderedFields[j] = fields[j];
        }
    }

    copy(orderedArgs, args, argCount);
    copy(orderedFields, fields, argCount);
}

Value* resolveTup(FunBuilder* b, Type* targetType, ast::TupExpr* expr, Id name) {
    U32 argCount = 0;
    auto arg = expr->args;
    while(arg) {
        argCount++;
        arg = arg->next;
    }

    // Convert empty tuples to nop instructions of unit type.
    if(argCount == 0) {
        return nop(b->block, name);
    }

    // Otherwise, resolve the argument types and instantiate a tuple.
    auto args = (Value**)b->mem.alloc(sizeof(Value*) * argCount);

    // In order to support resetting a partially matching tuple to use the initial order,
    // we allocate an additional array to copy the correct order into.
    // We also keep track of the initial indices.
    auto fields = (Field*)alloca(sizeof(Field) * argCount);
    auto fieldIndices = (U32*)alloca(sizeof(U32) * argCount);

    set(fieldIndices, argCount, 0xff);

    // Check if the tuple has to be typecast into a specific set of fields.
    // Only use the target information if the fields are compatible.
    Field* targetFields = nullptr;
    if(targetType && targetType->kind == Type::Tup) {
        auto tupType = (TupType*)targetType;
        if(tupType->count == argCount) {
            targetFields = tupType->fields;
        }
    }

    U32 matchCount = 0;
    arg = expr->args;
    for(U32 i = 0; i < argCount; i++) {
        U32 fieldIndex = i;
        Type* fieldType = nullptr;

        // If we have a target type, try to match target field names.
        if(targetFields && arg->item.name) {
            for(U32 j = 0; j < argCount; j++) {
                auto field = targetFields + j;
                if(field->name && field->name == arg->item.name) {
                    fieldIndex = j;
                    fieldIndices[j] = i;
                    fieldType = field->type;
                    matchCount++;
                    break;
                }
            }

            // Whenever we fail to match a field, stop trying and reset to use normal indices.
            if(matchCount < i + 1) {
                resetTupOrder(args, fields, fieldIndices, argCount);
                targetFields = nullptr;
            }
        }

        args[fieldIndex] = resolveExpr(b, fieldType, arg->item.value, 0, true);

        if(fieldType) {
            auto c = implicitConvert(b, args[fieldIndex], fieldType, true, false);
            if(c) args[fieldIndex] = c;
        }

        fields[fieldIndex].type = args[fieldIndex]->type;
        fields[fieldIndex].name = arg->item.name;

        arg = arg->next;
    }

    // If the final match count is too low, reset to use the initial argument order.
    if(targetFields && matchCount < argCount) {
        resetTupOrder(args, fields, fieldIndices, argCount);
    }

    auto type = resolveTupType(&b->context, b->fun->module, fields, argCount);
    return tup(b->block, name, type, args, argCount);
}

Value* resolveTupUpdate(FunBuilder* b, Type* targetType, ast::TupUpdateExpr* expr, Id name, bool used) {
    // An update-expression only changes the content while the type stays the same.
    // This means that we can just forward the target type to the source value.
    auto target = resolveExpr(b, targetType, expr->value, 0, true);
    auto type = canonicalType(target->type);
    if(type->kind != Type::Tup) {
        error(b, "only tuples can update their fields"_buffer, expr->value);
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
                auto fieldType = tup->fields[f].type;

                fields[fieldCount].index = f;
                fields[fieldCount].value = resolveExpr(b, fieldType, arg->item.value, 0, true);
                fieldCount++;
                found = true;
                break;
            }
        }

        if(!found) {
            error(b, "field %@ does not exist in this type"_buffer, arg->item.value, arg->item.name);
        }
        arg = arg->next;
    }

    return updateField(b->block, name, target, fields, fieldCount);
}

Value* resolveArray(FunBuilder* b, Type* targetType, ast::ArrayExpr* expr, Id name) {
    U32 length = 0;
    auto arg = expr->args;
    while(arg) {
        length++;
        arg = arg->next;
    }

    Type* arrayType = nullptr;
    if(targetType && targetType->kind == Type::Array) {
        arrayType = ((ArrayType*)targetType)->content;
    }

    if(length == 0) {
        if(arrayType) {
            return allocArray(b->block, name, arrayType, constInt(b->block, 0, 0, &intTypes[IntType::Int]), false, false);
        } else {
            error(b, "cannot infer type of array"_buffer, expr);
            return error(b->block, name, getArray(b->fun->module, &unitType));
        }
    }

    auto values = (Value**)b->mem.alloc(sizeof(Value*) * length);

    arg = expr->args;
    for(U32 i = 0; i < length; i++) {
        values[i] = resolveExpr(b, arrayType, arg->item, 0, true);
        if(i > 0) {
            // This will update the value stored in the alt if needed.
            if(!generalizeTypes(b, values[i - 1], values[i])) {
                error(b, "array contents must have the same type"_buffer, expr);
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

Value* resolveCase(FunBuilder* b, Type* targetType, ast::CaseExpr* expr, Id name, bool used) {
    auto pivot = resolveExpr(b, nullptr, expr->pivot, 0, true);
    auto alt = expr->alts;
    auto preceding = b->block;

    U32 altCount = 0;
    while(alt) {
        altCount++;
        alt = alt->next;
    }

    auto alts = (InstPhi::Alt*)b->fun->module->memory.alloc(sizeof(InstPhi::Alt) * altCount);
    U32 usedAlts = 0;
    bool hasElse = false;

    MatchContext* context = nullptr;

    alt = expr->alts;
    for(U32 i = 0; i < altCount; i++) {
        auto otherwise = block(b->fun, true);
        otherwise->preceding = b->block;

        auto result = resolvePat(b, &context, otherwise, pivot, alt->item.pat);
        if(result == 0) {
            // Default case: resolve the expression for this match into the matching block.
            // Continue with other matches in the failing block.
            alts[usedAlts].value = resolveExpr(b, targetType, alt->item.expr, 0, true);
            alts[usedAlts].fromBlock = b->block;

            b->fun->blocks.push(otherwise);
            b->block = otherwise;

            usedAlts++;
        } else if(result == 1) {
            // Pattern always matches. Resolve in the matching block, don't add the failing block.
            alts[usedAlts].value = resolveExpr(b, targetType, alt->item.expr, 0, true);
            alts[usedAlts].fromBlock = b->block;

            usedAlts++;
            hasElse = true;
            break;
        } else {
            // Pattern never matches. Continue with other matches in the failing block.
            b->block = otherwise;
            b->fun->blocks.push(otherwise);
        }

        alt = alt->next;
    }

    if(used) {
        if(!hasElse) {
            error(b, "match expression doesn't produce a result in every case"_buffer, expr);
        }

        if(usedAlts == 1) {
            // If the very first match always succeeds we can use that value immediately and continue in the same block.
            b->block = alts[0].fromBlock;
            return alts[0].value;
        } else if(usedAlts == 0) {
            // All matches will always fail - return an error in this case.
            return error(b->block, name, &errorType);
        }

        // Create an after-block that each non-complete match block continues in.
        // This block selects the result from the match that succeeded.
        auto after = block(b->fun);
        after->preceding = preceding;

        for(U32 i = 0; i < usedAlts; i++) {
            if(!alts[i].value) {
                error(b, "match expression doesn't produce a result in every case"_buffer, expr);
                b->block = after;
                return error(after, name, &errorType);
            }

            if(i > 0) {
                // This will update the value stored in the alt if needed.
                if(!generalizeTypes(b, alts[i - 1].value, alts[i].value)) {
                    error(b, "match cases produce differing types"_buffer, expr);
                }
            }

            jmp(alts[i].fromBlock, after);
        }

        b->block = after;
        return phi(after, name, alts, usedAlts);
    } else {
        // We don't need a succeeding block if:
        //  - Every match always returns. Each block is already complete.
        // or
        //  - The very first match always succeeds. We can continue in the current block without branching.
        U32 returnCount = 0;
        for(U32 i = 0; i < usedAlts; i++) {
            if(alts[i].fromBlock->returns) {
                returnCount++;
            }
        }

        if(returnCount == usedAlts || usedAlts <= 1) {
            return nullptr;
        }

        // Create an after-block that each non-complete match block continues in.
        auto after = block(b->fun);
        after->preceding = preceding;

        for(U32 i = 0; i < usedAlts; i++) {
            if(!alts[i].fromBlock->complete) {
                jmp(alts[i].fromBlock, after);
            }
        }

        b->block = after;
        return nullptr;
    }
}

Value* resolveRet(FunBuilder* b, ast::RetExpr* expr) {
    // The expression needs to be resolved before the current block is loaded.
    Value* value = nullptr;
    if(expr->value) {
        auto returnType = b->fun->returnType;
        value = resolveExpr(b, returnType, expr->value, 0, true);

        if(returnType && returnType->kind != Type::Error) {
            // Try to implicitly convert to the target return type if one was provided.
            // If the conversion fails (returns null), let the function resolving code handle it.
            auto converted = implicitConvert(b, value, returnType, false, false);
            if(converted) value = converted;
        }
    }
    return ret(b->block, value);
}

Value* resolveExpr(FunBuilder* b, Type* targetType, ast::Expr* expr, Id name, bool used) {
    switch(expr->type) {
        case ast::Expr::Error:
            return nullptr;
        case ast::Expr::Multi:
            return resolveMulti(b, targetType, (ast::MultiExpr*)expr, name, used);
        case ast::Expr::Lit:
            return resolveLit(b, targetType, &((ast::LitExpr*)expr)->literal, name);
        case ast::Expr::Var:
            return resolveVar(b, (ast::VarExpr*)expr, false);
        case ast::Expr::App:
            return resolveApp(b, targetType, (ast::AppExpr*)expr, name, used);
        case ast::Expr::Fun:
            return resolveFun(b, (ast::FunExpr*)expr, name, used);
        case ast::Expr::Infix:
            return resolveInfix(b, targetType, (ast::InfixExpr*)expr, name, used);
        case ast::Expr::Prefix:
            return resolvePrefix(b, targetType, (ast::PrefixExpr*)expr, name, used);
        case ast::Expr::If:
            return resolveIf(b, targetType, (ast::IfExpr*)expr, name, used);
        case ast::Expr::MultiIf:
            return resolveMultiIf(b, targetType, (ast::MultiIfExpr*)expr, name, used);
        case ast::Expr::Decl:
            return resolveDecl(b, targetType, (ast::DeclExpr*)expr, name, used);
        case ast::Expr::While:
            return resolveWhile(b, (ast::WhileExpr*)expr, used);
        case ast::Expr::For:
            return resolveFor(b, (ast::ForExpr*)expr, used);
        case ast::Expr::Assign:
            return resolveAssign(b, (ast::AssignExpr*)expr, used);
        case ast::Expr::Nested:
            return resolveNested(b, targetType, (ast::NestedExpr*)expr, name, used);
        case ast::Expr::Coerce:
            return resolveCoerce(b, (ast::CoerceExpr*)expr, name, used);
        case ast::Expr::Field:
            return resolveField(b, (ast::FieldExpr*)expr, name, used);
        case ast::Expr::Con:
            return resolveCon(b, targetType, (ast::ConExpr*)expr, name);
        case ast::Expr::Tup:
            return resolveTup(b, targetType, (ast::TupExpr*)expr, name);
        case ast::Expr::TupUpdate:
            return resolveTupUpdate(b, targetType, (ast::TupUpdateExpr*)expr, name, used);
        case ast::Expr::Array:
            return resolveArray(b, targetType, (ast::ArrayExpr*)expr, name);
        case ast::Expr::Map:
            return resolveMap(b, (ast::MapExpr*)expr, name, used);
        case ast::Expr::Format:
            return resolveFormat(b, (ast::FormatExpr*)expr, name, used);
        case ast::Expr::Case:
            return resolveCase(b, targetType, (ast::CaseExpr*)expr, name, used);
        case ast::Expr::Ret:
            return resolveRet(b, (ast::RetExpr*)expr);
    }

    return nullptr;
}
