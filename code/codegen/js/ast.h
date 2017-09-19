#pragma once

#include "../../util/types.h"
#include "../../util/array.h"

struct Expr;
struct Stmt;

struct Variable {
    Id fullName;
    U32 refCount;
};

struct VarDecl {
    Variable* v;
    Expr* value;
};

struct Expr {
    enum Type {
        String,
        Float,
        Int,
        Bool,
        Null,
        Undefined,
        Array,
        Object,
        Var,
        Field,
        Prefix,
        Infix,
        If,
        Assign,
        Call,
        Fun
    } type;

    Expr(Type t) : type(t) {}
    bool is(Type type) const {return this->type == type;}
};

struct StringExpr: Expr {
    StringExpr(Id string): Expr(String), string(string) {}
    Id string;
};

struct FloatExpr: Expr {
    FloatExpr(double f): Expr(Float), f(f) {}
    Id f;
};

struct IntExpr: Expr {
    IntExpr(U64 i): Expr(Int), i(i) {}
    U64 i;
};

struct BoolExpr: Expr {
    BoolExpr(bool b): Expr(Bool), b(b) {}
    bool b;
};

struct NullExpr: Expr {
    NullExpr(): Expr(Null) {}
};

struct UndefinedExpr: Expr {
    UndefinedExpr(): Expr(Undefined) {}
};

struct ArrayExpr: Expr {
    ArrayExpr(Expr** content, U32 count): Expr(Array), content(content), count(count) {}
    Expr** content;
    U32 count;
};

struct ObjectExpr: Expr {
    ObjectExpr(Expr** keys, Expr** values, U32 count): Expr(Object), keys(keys), values(values), count(count) {}
    Expr** keys;
    Expr** values;
    U32 count;
};

struct VarExpr: Expr {
    VarExpr(Variable* var): Expr(Var), var(var) {}
    Variable* var;
};

struct FieldExpr: Expr {
    FieldExpr(Expr* arg, Expr* field): Expr(Field), arg(arg), field(field) {}
    Expr* arg;
    Expr* field;
};

struct PrefixExpr: Expr {
    PrefixExpr(Id op, Expr* arg): Expr(Prefix), op(op), arg(arg) {}
    Id op;
    Expr* arg;
};

struct InfixExpr: Expr {
    InfixExpr(Id op, Expr* lhs, Expr* rhs): Expr(Infix), op(op), lhs(lhs), rhs(rhs) {}
    Id op;
    Expr* lhs;
    Expr* rhs;
};

struct IfExpr: Expr {
    IfExpr(Expr* cond, Expr* then, Expr* otherwise): Expr(If), cond(cond), then(then), otherwise(otherwise) {}
    Expr* cond;
    Expr* then;
    Expr* otherwise;
};

struct AssignExpr: Expr {
    AssignExpr(Id op, Expr* target, Expr* value): Expr(Assign), op(op), target(target), value(value) {}
    Id op;
    Expr* target;
    Expr* value;
};

struct CallExpr: Expr {
    CallExpr(Expr* target, Expr** args, U32 count): Expr(Call), target(target), args(args), count(count) {}
    Expr* target;
    Expr** args;
    U32 count;
};

struct FunExpr: Expr {
    FunExpr(Id name, Variable** args, U32 argCount, Stmt** body, U32 stmtCount): Expr(Fun), name(name), args(args), argCount(argCount), body(body), stmtCount(stmtCount) {}
    Id name;
    Variable** args;
    Stmt** body;
    U32 argCount;
    U32 stmtCount;
};

struct Stmt {
    enum Type {
        Block,
        Exp,
        If,
        While,
        DoWhile,
        Break,
        Continue,
        Labelled,
        Return,
        Var,
        Fun,
    } type;

    Stmt(Type t) : type(t) {}
    bool is(Type type) const {return this->type == type;}
};

struct BlockStmt: Stmt {
    BlockStmt(Stmt** stmts, U32 count): Stmt(Block), stmts(stmts), count(count) {}
    Stmt** stmts;
    U32 count;
};

struct ExprStmt: Stmt {
    ExprStmt(Expr* expr): Stmt(Exp), expr(expr) {}
    Expr* expr;
};

struct IfStmt: Stmt {
    IfStmt(Expr* cond, Stmt* then, Stmt* otherwise): Stmt(If), cond(cond), then(then), otherwise(otherwise) {}
    Expr* cond;
    Stmt* then;
    Stmt* otherwise;
};

struct WhileStmt: Stmt {
    WhileStmt(Expr* cond, Stmt* body): Stmt(While), cond(cond), body(body) {}
    Expr* cond;
    Stmt* body;
};

struct DoWhileStmt: Stmt {
    DoWhileStmt(Expr* cond, Stmt* body): Stmt(DoWhile), cond(cond), body(body) {}
    Expr* cond;
    Stmt* body;
};

struct BreakStmt: Stmt {
    BreakStmt(Id label): Stmt(Break), label(label) {}
    Id label;
};

struct ContinueStmt: Stmt {
    ContinueStmt(Id label): Stmt(Continue), label(label) {}
    Id label;
};

struct LabelledStmt: Stmt {
    LabelledStmt(Id name, Stmt* content): Stmt(Labelled), name(name), content(content) {}
    Id name;
    Stmt* content;
};

struct ReturnStmt: Stmt {
    ReturnStmt(Expr* value): Stmt(Return), value(value) {}
    Expr* value;
};

struct VarStmt: Stmt {
    VarStmt(VarDecl* values, U32 count): Stmt(Var), values(values), count(count) {}
    VarDecl* values;
    U32 count;
};

struct FunStmt: Stmt {
    FunStmt(Id name, Variable** args, U32 argCount, Stmt** body, U32 stmtCount): Stmt(Fun), name(name), args(args), argCount(argCount), body(body), stmtCount(stmtCount) {}
    Id name;
    Variable** args;
    Stmt** body;
    U32 argCount;
    U32 stmtCount;
};

struct VarScope {
    Id base;
    VarScope* parent;

    Array<Id> definedNames;
    U32 varCounter;
    U32 funCounter;
};