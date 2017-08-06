#include "../parse/ast.h"
#include "module.h"

struct FunBuilder {
    Function* fun;
    Block* block;
    Context& context;
    Arena& mem;
    Size funCounter = 0;
};

Value* resolveExpr(FunBuilder* b, ast::Expr* expr, Id name, bool used);

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

// Checks if the two provided types are the same. If not, it tries to implicitly convert to a common type.
static bool generalizeTypes(FunBuilder* b, Value* lhs, Value* rhs) {
    // TODO: Support implicit box/unbox, tuple conversions.
    return compareTypes(&b->context, lhs->type, rhs->type);
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

Value* resolveVar(FunBuilder* b, ast::VarExpr* expr, Id name, bool used) {

}

Value* resolveApp(FunBuilder* b, ast::AppExpr* expr, Id name, bool used) {

}

Value* resolveFun(FunBuilder* b, ast::FunExpr* expr, Id name, bool used) {

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
        jmp(thenBlock, after);

        if(!elseValue || !elseBlock || elseBlock->complete || thenBlock->complete) {
            error(b, "if expression doesn't produce a result in every case", expr);
            return phi(after, name, nullptr, 0);
        }

        generalizeTypes(b, thenValue, elseValue);
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

}

Value* resolveDecl(FunBuilder* b, ast::DeclExpr* expr, Id name, bool used) {

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

Value* resolveAssign(FunBuilder* b, ast::AssignExpr* expr, Id name, bool used) {

}

Value* resolveNested(FunBuilder* b, ast::NestedExpr* expr, Id name, bool used) {
    return resolveExpr(b, expr->expr, name, used);
}

Value* resolveCoerce(FunBuilder* b, ast::CoerceExpr* expr, Id name, bool used) {

}

Value* resolveField(FunBuilder* b, ast::FieldExpr* expr, Id name, bool used) {

}

Value* resolveCon(FunBuilder* b, ast::ConExpr* expr, Id name, bool used) {

}

Value* resolveTup(FunBuilder* b, ast::TupExpr* expr, Id name, bool used) {

}

Value* resolveTupUpdate(FunBuilder* b, ast::TupUpdateExpr* expr, Id name, bool used) {

}

Value* resolveArray(FunBuilder* b, ast::ArrayExpr* expr, Id name, bool used) {

}

Value* resolveMap(FunBuilder* b, ast::MapExpr* expr, Id name, bool used) {

}

Value* resolveFormat(FunBuilder* b, ast::FormatExpr* expr, Id name, bool used) {

}

Value* resolveCase(FunBuilder* b, ast::CaseExpr* expr, Id name, bool used) {

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
            return resolveVar(b, (ast::VarExpr*)expr, name, used);
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
            return resolveWhile(b, (ast::WhileExpr*)expr, name, used);
        case ast::Expr::Assign:
            return resolveAssign(b, (ast::AssignExpr*)expr, name, used);
        case ast::Expr::Nested:
            return resolveNested(b, (ast::NestedExpr*)expr, name, used);
        case ast::Expr::Coerce:
            return resolveCoerce(b, (ast::CoerceExpr*)expr, name, used);
        case ast::Expr::Field:
            return resolveField(b, (ast::FieldExpr*)expr, name, used);
        case ast::Expr::Con:
            return resolveCon(b, (ast::ConExpr*)expr, name, used);
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