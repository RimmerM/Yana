#include "../parse/ast.h"
#include "module.h"

Value* resolveExpr(FunBuilder* b, ast::Expr* expr, Id name, bool used);

struct FunBuilder {
    Function* fun;
    Block* block;
    Context& context;
    Arena& mem;
    Size funCounter = 0;
};

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
            return constString(b->block, string.name, string.length);
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

Value* resolveInfix(FunBuilder* b, ast::InfixExpr* expr, Id name, bool used) {

}

Value* resolvePrefix(FunBuilder* b, ast::PrefixExpr* expr, Id name, bool used) {

}

Value* resolveIf(FunBuilder* b, ast::IfExpr* expr, Id name, bool used) {

}

Value* resolveMultiIf(FunBuilder* b, ast::MultiIfExpr* expr, Id name, bool used) {

}

Value* resolveDecl(FunBuilder* b, ast::DeclExpr* expr, Id name, bool used) {

}

Value* resolveWhile(FunBuilder* b, ast::WhileExpr* expr, Id name, bool used) {

}

Value* resolveAssign(FunBuilder* b, ast::AssignExpr* expr, Id name, bool used) {

}

Value* resolveNested(FunBuilder* b, ast::NestedExpr* expr, Id name, bool used) {

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