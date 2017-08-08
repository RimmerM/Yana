#include "../parse/ast.h"
#include "module.h"

template<class... T>
static void error(FunBuilder* b, const char* message, const Node* node, T&&... p) {
    b->context.diagnostics.error(message, node, nullptr, forward<T>(p)...);
}

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

// Adds code to implicitly convert a value if possible.
// This adds code in the block the value is defined in, not the current one.
// If isConstruct is set, the conversion is less strict since the target type is explicitly defined.
// Returns the new value or null if no conversion is possible.
static Value* implicitConvert(FunBuilder* b, Value* v, Type* targetType, bool isConstruct) {
    auto type = v->type;
    if(compareTypes(&b->context, type, targetType)) return v;

    auto kind = type->kind;
    auto targetKind = targetType->kind;
    auto name = v->name;

    // TODO: Support conversions between equivalent tuple types.
    // TODO: Support conversions to generic types.
    if(kind == Type::Ref && targetKind == Type::Ref) {
        // Different reference kinds can be implicitly converted in some cases.
        auto ref = (RefType*)type;
        auto targetRef = (RefType*)targetType;

        // This case is handled implicitly by the code generators.
        if(ref->isMutable && !targetRef->isMutable) {
            return v;
        }

        // A local reference can be implicitly converted to a traced one.
        // Following the semantics of the language, the contained value is copied.
        if(ref->isLocal && targetRef->isTraced) {
            v->name = 0;
            auto newRef = alloc(v->block, name, targetRef->to, targetRef->isMutable, false);
            load(v->block, 0, v);
            store(v->block, 0, newRef, v);
            return newRef;
        }

        // All other cases are unsafe (traced to untraced)
        // or would have unexpected side-effects (copying untraced to traced).
    } else if(kind == Type::Ref) {
        // A returned reference can be implicitly loaded to a register.
        auto ref = (RefType*)type;
        if(compareTypes(&b->context, ref->to, targetType)) {
            v->name = 0;
            return load(v->block, name, v);
        }
    } else if(kind == Type::Int && targetKind == Type::Int) {
        // Integer types can be implicitly extended to a larger type.
        auto intType = (IntType*)type;
        auto targetInt = (IntType*)targetType;
        if(intType->bits == targetInt->bits) {
            return v;
        } else if(intType->bits < targetInt->bits) {
            v->name = 0;
            return sext(v->block, name, v, targetType);
        } else if(isConstruct) {
            v->name = 0;
            return trunc(v->block, name, v, targetType);
        } else {
            error(b, "cannot implicitly convert an integer to a smaller type", nullptr);
        }
    } else if(kind == Type::Float && targetKind == Type::Float) {
        // Float types can be implicitly extended to a larger type.
        auto floatType = (FloatType*)type;
        auto targetFloat = (FloatType*)targetType;
        if(floatType->bits == targetFloat->bits) {
            return v;
        } else if(floatType->bits < targetFloat->bits) {
            v->name = 0;
            return fext(v->block, name, v, targetType);
        } else if(isConstruct) {
            v->name = 0;
            return ftrunc(v->block, name, v, targetType);
        } else {
            error(b, "cannot implicitly convert a float to a smaller type", nullptr);
        }
    } else if(isConstruct && kind == Type::Float && targetKind == Type::Int) {
        v->name = 0;
        return ftoi(v->block, name, v, targetType);
    } else if(isConstruct && kind == Type::Int && targetKind == Type::Float) {
        v->name = 0;
        return itof(v->block, name, v, targetType);
    }

    error(b, "cannot implicitly convert to type", nullptr);
    return nullptr;
}

// Checks if the two provided types are the same. If not, it tries to implicitly convert to a common type.
static bool generalizeTypes(FunBuilder* b, Value*& lhs, Value*& rhs) {
    // Try to do an implicit conversion each way.
    if(auto v = implicitConvert(b, lhs, rhs->type, false)) {
        lhs = v;
        return true;
    } else if(auto v = implicitConvert(b, rhs, lhs->type, false)) {
        rhs = v;
        return true;
    }

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

Value* resolveLit(FunBuilder* b, ast::LitExpr* expr, Id name, bool used) {
    switch(expr->literal.type) {
        case ast::Literal::Int:
            return constInt(b->block, expr->literal.i);
        case ast::Literal::Float:
            return constFloat(b->block, expr->literal.f);
        case ast::Literal::Char:
            return nullptr;
        case ast::Literal::String: {
            auto string = b->context.find(expr->literal.s);
            return constString(b->block, string.text, string.textLength);
        }
        case ast::Literal::Bool: {
            auto c = constInt(b->block, expr->literal.b);
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
    if(!asRV && (value->type->kind == Type::Ref)) {
        return load(b->block, 0, value);
    } else {
        return value;
    }
}

Value* resolveVar(FunBuilder* b, ast::VarExpr* expr, bool asRV) {
    auto value = findVar(b, expr->name);
    if(!value) {
        error(b, "identifier not found", expr);
        return nullptr;
    }

    return useValue(b, value, asRV);
}

static void createCallArgs(FunBuilder* b, Value* firstArg, List<ast::TupArg>* argList) {

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
        auto v = implicitConvert(b, args[i], funType->args[i].type, false);
        if(!v) {
            error(b, "incompatible type for function argument", nullptr);
        }

        args[i] = v;
    }

    return callDyn(b->block, name, callee, args, argCount);
}

static Value* resolveStaticCall(FunBuilder* b, Id funName, Value* firstArg, List<ast::TupArg>* argList, Id name) {
    auto fun = findFun(&b->context, b->fun->module, funName);
    if(!fun) {
        error(b, "no function found for this name", nullptr);
        return nullptr;
    }

    // Make sure the function definition is finished.
    resolveFun(&b->context, fun);

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
        auto v = implicitConvert(b, args[i], fun->args[i].type, false);
        if(!v) {
            error(b, "incompatible type for function argument", nullptr);
        }

        args[i] = v;
    }

    return call(b->block, name, fun, args, argCount);
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
    if(cond->type != &intTypes[IntType::Bool]) {
        error(b, "if condition must be a boolean", expr);
    }

    auto then = block(b->fun);
    auto otherwise = block(b->fun);

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
            hasElse = true;
            break;
        } else {
            auto then = block(b->fun);
            auto next = block(b->fun);

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
            if(i > 0) {
                // This will update the value stored in the alt if needed.
                if(!generalizeTypes(b, alts[i - 1].value, alts[i].value)) {
                    error(b, "if and else branches produce differing types", expr);
                }
            }

            if(!alts[i].value) {
                error(b, "if expression doesn't produce a result in every case", expr);
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
            b->fun->blocks.remove(next - b->fun->blocks.pointer());
            b->block = &*b->fun->blocks.back();
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

Value* resolveDecl(FunBuilder* b, ast::DeclExpr* expr) {
    auto content = expr->content;
    if(!content) {
        error(b, "variables must be initialized on declaration", expr);
        return nullptr;
    }

    switch(expr->mut) {
        case ast::DeclExpr::Immutable: {
            // Immutable values are stored as registers, so we just have to resolve the creation expression.
            resolveExpr(b, expr->content, expr->name, true);
            return nullptr;
        }
        case ast::DeclExpr::Val:
        case ast::DeclExpr::Ref: {
            auto value = resolveExpr(b, expr->content, 0, true);
            auto var = alloc(b->block, expr->name, value->type, true, expr->mut == ast::DeclExpr::Val);
            store(b->block, 0, var, value);
            return var;
        }
    }
}

Value* resolveWhile(FunBuilder* b, ast::WhileExpr* expr, Id name, bool used) {
    auto condBlock = block(b->fun);
    jmp(b->block, condBlock);
    b->block = condBlock;

    auto cond = resolveExpr(b, expr->cond, 0, true);
    if(cond->type != &intTypes[IntType::Bool]) {
        error(b, "while condition must be a boolean", expr);
    }

    auto bodyBlock = block(b->fun);
    auto exitBlock = block(b->fun);
    je(b->block, cond, bodyBlock, exitBlock);
    b->block = bodyBlock;

    resolveExpr(b, expr->loop, 0, false);
    jmp(b->block, condBlock);

    b->block = exitBlock;
    return nullptr;
}

Value* resolveAssign(FunBuilder* b, ast::AssignExpr* expr) {
    auto target = expr->target;
    switch(target->type) {
        case ast::Expr::Var: {
            auto var = resolveVar(b, (ast::VarExpr*)target, true);
            if(var->type->kind != Type::Ref) {
                error(b, "type is not assignable", target);
            }

            auto val = resolveExpr(b, expr->value, 0, true);
            store(b->block, 0, var, val);
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
    auto target = resolveExpr(b, expr->target, name, used);
    auto type = resolveType(&b->context, b->fun->module, expr->kind);
    return implicitConvert(b, target, type, true);
}

Value* resolveField(FunBuilder* b, ast::FieldExpr* expr, Id name, bool used) {
    auto target = resolveExpr(b, expr->target, 0, true);
    auto field = testStaticField(b, name, target, expr->field);
    if(field) return field;

    // TODO: Handle array and map loads.
    error(b, "type does not contain the requested field", expr->target);
    return nullptr;
}

Value* resolveTup(FunBuilder* b, ast::TupExpr* expr, Id name, bool used) {
    return nullptr;
}

static Value* explicitConstruct(FunBuilder* b, Type* type, ast::ConExpr* expr, Id name) {
    // There are a whole bunch of possible cases we could support here,
    // but most can't even be created by the parser.
    // For now we handle aliases and primitive types.
    if(type->kind == Type::Int || type->kind == Type::Float) {
        if(!expr->args || expr->args->next) {
            error(b, "incorrect number of arguments to type constructor", expr);
            return nullptr;
        }

        auto arg = resolveExpr(b, expr->args->item.value, name, true);
        return implicitConvert(b, arg, type, true);
    } else if(type->kind == Type::String) {
        if(!expr->args || expr->args->next) {
            error(b, "incorrect number of arguments to string constructor", expr);
            return nullptr;
        }

        auto arg = resolveExpr(b, expr->args->item.value, name, true);
        if(arg->type->kind != Type::String) {
            error(b, "strings must be constructed with a string", expr);
            return nullptr;
        }

        return arg;
    } else if(type->kind == Type::Alias) {
        // TODO: Handle generic instantiation here.
        return explicitConstruct(b, ((AliasType*)type)->to, expr, name);
    } else if(type->kind == Type::Tup) {
        // TODO: Handle tuple types.
    }

    error(b, "cannot construct this type", expr->type);
    return nullptr;
}

static Value* resolveMiscCon(FunBuilder* b, ast::ConExpr* expr, Id name) {
    auto type = findType(&b->context, b->fun->module, expr->type->con);
    if(!type) {
        error(b, "cannot find type", expr->type);
        return nullptr;
    }

    return explicitConstruct(b, type, expr, name);
}

Value* resolveCon(FunBuilder* b, ast::ConExpr* expr, Id name) {
    auto con = findCon(&b->context, b->fun->module, expr->type->con);
    if(!con) {
        return resolveMiscCon(b, expr, name);
    }

    // If we have more than one argument, the constructor must contain a tuple type.
    auto arg = expr->args;
    if(!arg) {
        if(con->count > 0) {
            error(b, "incorrect number of arguments to constructor", expr);
        }

        return record(b->block, name, con, nullptr, 0);
    }

    U32 argCount = 0;
    while(arg) {
        argCount++;
        arg = arg->next;
    }

    if(argCount != con->count) {
        error(b, "incorrect number of arguments to constructor", expr);
    }

    auto args = (Value**)b->mem.alloc(sizeof(Value*) * argCount);
    arg = expr->args;
    for(U32 i = 0; i < argCount; i++) {
        args[i] = resolveExpr(b, arg->item.value, arg->item.name, true);
        arg = arg->next;
    }

    auto value = record(b->block, name, con, args, argCount);
    return value;
}

Value* resolveTupUpdate(FunBuilder* b, ast::TupUpdateExpr* expr, Id name, bool used) {
    return nullptr;
}

Value* resolveArray(FunBuilder* b, ast::ArrayExpr* expr, Id name, bool used) {
    return nullptr;
}

Value* resolveMap(FunBuilder* b, ast::MapExpr* expr, Id name, bool used) {
    return nullptr;
}

Value* resolveFormat(FunBuilder* b, ast::FormatExpr* expr, Id name, bool used) {
    return nullptr;
}

Value* resolveCase(FunBuilder* b, ast::CaseExpr* expr, Id name, bool used) {
    return nullptr;
}

Value* resolveRet(FunBuilder* b, ast::RetExpr* expr, Id name, bool used) {
    return ret(b->block, resolveExpr(b, expr->value, 0, true));
}

Value* resolveExpr(FunBuilder* b, ast::Expr* expr, Id name, bool used) {
    switch(expr->type) {
        case ast::Expr::Error:
            return nullptr;
        case ast::Expr::Multi:
            return resolveMulti(b, (ast::MultiExpr*)expr, name, used);
        case ast::Expr::Lit:
            return resolveLit(b, (ast::LitExpr*)expr, name, used);
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
            return resolveDecl(b, (ast::DeclExpr*)expr);
        case ast::Expr::While:
            return resolveWhile(b, (ast::WhileExpr*)expr, name, used);
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
            return resolveTup(b, (ast::TupExpr*)expr, name, used);
        case ast::Expr::TupUpdate:
            return resolveTupUpdate(b, (ast::TupUpdateExpr*)expr, name, used);
        case ast::Expr::Array:
            return resolveArray(b, (ast::ArrayExpr*)expr, name, used);
        case ast::Expr::Map:
            return resolveMap(b, (ast::MapExpr*)expr, name, used);
        case ast::Expr::Format:
            return resolveFormat(b, (ast::FormatExpr*)expr, name, used);
        case ast::Expr::Case:
            return resolveCase(b, (ast::CaseExpr*)expr, name, used);
        case ast::Expr::Ret:
            return resolveRet(b, (ast::RetExpr*)expr, name, used);
    }

    return nullptr;
}