#pragma once

#include <vector>
#include "../compiler/context.h"
#include "../compiler/diagnostics.h"

/*
 * Types
 */

struct Type: Node {
    enum Kind {
        Error, // Placeholder for parse errors.
        Unit,  // The empty unit type.
        Con,   // A type name for a named type.
        Ptr,   // A pointer to a type.
        Gen,   // A generic or polymorphic named type.
        Tup,   // A tuple type with optionally named fields.
        Fun,   // A function type.
        App,   // Application of higher-kinded type.
        Arr,   // An array of a type.
        Map,   // A map from one type to another.
    } kind;

    Type(Kind k) : kind(k) {}
    bool is(Kind kind) const {return this->kind == kind;}
};

struct TupField {
    TupField(Type* type, Id name, struct Expr* def) : type(type), def(def), name(name) {}

    Type* type;
    struct Expr* def;
    Id name;
};

struct ArgDecl {
    ArgDecl(Type* type, Id name): type(type), name(name) {}
    Type* type;
    Id name;
};

struct TupType: Type {
    TupType(List<TupField>* fields) : Type(Tup), fields(fields) {}
    List<TupField>* fields;
};

struct AppType: Type {
    AppType(Type* base, List<Type*>* apps): Type(App), base(base), apps(apps) {}
    Type* base;
    List<Type*>* apps;
};

struct ConType: Type {
    ConType(Id con): Type(Con), con(con) {}
    Id con;
};

struct GenType: Type {
    GenType(Id con): Type(Gen), con(con) {}
    Id con;
};

struct FunType: Type {
    FunType(List<ArgDecl>* args, Type* ret): Type(Fun), args(args), ret(ret) {}
    List<ArgDecl>* args;
    Type* ret;
};

struct PtrType: Type {
    PtrType(Type* type): Type(Ptr), type(type) {}
    Type* type;
};

struct ArrType: Type {
    ArrType(Type* type): Type(Arr), type(type) {}
    Type* type;
};

struct MapType: Type {
    MapType(Type* from, Type* to): Type(Map), from(from), to(to) {}
    Type* from;
    Type* to;
};

/*
 * Pats
 */

struct Literal {
    enum Type {
        Float,
        Int,
        Char,
        String,
        Bool
    };

    union {
        double f;
        U64 i;
        WChar32 c;
        Id s;
        bool b;
    };

    Type type;
};

struct Pat: Node {
    enum Kind {
        Error, // Placeholder for parse errors.
        Var,
        Lit,
        Any,
        Tup,
        Con
    };

    Id asVar;
    Kind kind;

    Pat(Kind k, Id asVar = 0) : asVar(asVar), kind(k) {}
};

struct VarPat: Pat {
    VarPat(Id var, Id asVar = 0): Pat(Var, asVar), var(var) {}
    Id var;
};

struct LitPat: Pat {
    LitPat(Literal lit, Id asVar = 0): Pat(Lit, asVar), lit(lit) {}
    Literal lit;
};

struct FieldPat {
    FieldPat(Id field, Pat* pat): field(field), pat(pat) {}
    Id field;
    Pat* pat;
};

struct TupPat: Pat {
    TupPat(List<FieldPat>* fields, Id asVar = 0): Pat(Tup, asVar), fields(fields) {}
    List<FieldPat>* fields;
};

struct ConPat: Pat {
    ConPat(Id constructor, List<Pat*>* patterns): Pat(Con), constructor(constructor), patterns(patterns) {}
    Id constructor;
    List<Pat*>* patterns;
};

/*
 * Exprs
 */

struct Expr;

struct TupArg {
    TupArg(Id name, Expr* value): name(name), value(value) {}
    Id name;
    Expr* value;
};

struct IfCase {
    IfCase(Expr* cond, Expr* then) : cond(cond), then(then) {}
    Expr* cond;
    Expr* then;
};

struct MapArg {
    MapArg(Expr* key, Expr* value): key(key), value(value) {}
    Expr* key;
    Expr* value;
};

struct Alt {
    Pat* pat;
    Expr* expr;
};

struct Arg {
    Id name;
    Type* type;
    Expr* def;
};

/// Formatted strings are divided into chunks.
/// Each chunk consists of a string part and an expression to format and insert after it.
/// The expression may be null if this chunk is the first one in a literal.
struct FormatChunk {
    Id string;
    Expr* format;
};

struct Expr: Node {
    enum Type {
        Error, // Placeholder for parse errors.
        Unit,
        Multi,
        Lit,
        Var,
        App,
        Fun,
        Infix,
        Prefix,
        If,
        MultiIf,
        Decl,
        While,
        Assign,
        Nested,
        Coerce,
        Field,
        Con,
        Tup,
        TupUpdate,
        Array,
        Map,
        Format,
        Case,
        Ret,
    } type;

    Expr(Type t) : type(t) {}
    bool is(Type type) const {return this->type == type;}
};

struct MultiExpr : Expr {
    MultiExpr(List<Expr*>* exprs): Expr(Multi), exprs(exprs) {}
    List<Expr*>* exprs;
};

// This is used to represent parenthesized expressions.
// We need to keep all ordering information for the reordering pass later.
struct NestedExpr: Expr {
    NestedExpr(Expr* expr): Expr(Nested), expr(expr) {}
    Expr* expr;
};

struct LitExpr: Expr {
    LitExpr(Literal lit): Expr(Lit), literal(lit) {}
    Literal literal;
};

struct VarExpr: Expr {
    VarExpr(Id n): Expr(Var), name(n) {}
    Id name;
};

struct AppExpr: Expr {
    AppExpr(Expr* callee, List<TupArg>* args): Expr(App), callee(callee), args(args) {}
    Expr* callee;
    List<TupArg>* args;
};

struct InfixExpr: Expr {
    InfixExpr(VarExpr* op, Expr* lhs, Expr* rhs): Expr(Infix), lhs(lhs), rhs(rhs), op(op) {}
    Expr* lhs, *rhs;
    VarExpr* op;
    bool ordered = false;
};

struct PrefixExpr: Expr {
    PrefixExpr(VarExpr* op, Expr* dst): Expr(Prefix), dst(dst), op(op) {}
    Expr* dst;
    VarExpr* op;
};

struct IfExpr: Expr {
    IfExpr(Expr* cond, Expr* then, Expr* otherwise): Expr(If), cond(cond), then(then), otherwise(otherwise) {}
    Expr* cond;
    Expr* then;
    Expr* otherwise;
};

struct MultiIfExpr: Expr {
    MultiIfExpr(List<IfCase>* cases): Expr(MultiIf), cases(cases) {}
    List<IfCase>* cases;
};

struct DeclExpr: Expr {
    DeclExpr(Id name, Expr* content, bool constant): Expr(Decl), name(name), content(content), constant(constant) {}
    Id name;
    Expr* content;
    bool constant;
};

struct WhileExpr: Expr {
    WhileExpr(Expr* cond, Expr* loop): Expr(While), cond(cond), loop(loop) {}
    Expr* cond;
    Expr* loop;
};

struct AssignExpr: Expr {
    AssignExpr(Expr* target, Expr* value): Expr(Assign), target(target), value(value) {}
    Expr* target;
    Expr* value;
};

struct CoerceExpr: Expr {
    CoerceExpr(Expr* target, ::Type* kind): Expr(Coerce), target(target), kind(kind) {}
    Expr* target;
    ::Type* kind;
};

struct FieldExpr: Expr {
    FieldExpr(Expr* target, Expr* field): Expr(Field), target(target), field(field) {}
    Expr* target; // Either a var, literal or a complex expression.
    Expr* field;  // Field to apply to.
};

struct ConExpr: Expr {
    ConExpr(ConType* type, List<Expr*>* args): Expr(Con), type(type), args(args) {}
    ConType* type;
    List<Expr*>* args;
};

struct TupExpr: Expr {
    TupExpr(List<TupArg>* args): Expr(Tup), args(args) {}
    List<TupArg>* args;
};

struct TupUpdateExpr: Expr {
    TupUpdateExpr(Expr* value, List<TupArg>* args): Expr(TupUpdate), value(value), args(args) {}
    Expr* value;
    List<TupArg>* args;
};

struct ArrayExpr: Expr {
    ArrayExpr(List<Expr*>* args): Expr(Array), args(args) {}
    List<Expr*>* args;
};

struct MapExpr: Expr {
    MapExpr(List<MapArg>* args): Expr(Map), args(args) {}
    List<MapArg>* args;
};

struct FunExpr: Expr {
    FunExpr(List<Arg>* args, Expr* body): Expr(Fun), args(args), body(body) {}
    List<Arg>* args;
    Expr* body;
};

struct FormatExpr: Expr {
    FormatExpr(List<FormatChunk>* format): Expr(Format), format(format) {}
    List<FormatChunk>* format;
};

struct CaseExpr: Expr {
    CaseExpr(Expr* pivot, List<Alt>* alts): Expr(Case), pivot(pivot), alts(alts) {}
    Expr* pivot;
    List<Alt>* alts;
};

struct RetExpr: Expr {
    RetExpr(Expr* value): Expr(Ret), value(value) {}
    Expr* value;
};

/*
 * Decls
 */

struct SimpleType {
    SimpleType(Id name, List<Id>* kind) : name(name), kind(kind) {}
    Id name;
    List<Id>* kind;
};

struct Con {
    Con(Id name, Type* content) : name(name), content(content) {}
    Id name;
    Type* content;
};

struct Decl: Node {
    enum Kind {
        Error, // Placeholder for parse errors.
        Fun,
        Alias,
        Data,
        Class,
        Instance,
        Foreign
    } kind;

    bool exported;

    Decl(Kind t): kind(t) {}
};

struct FunDecl: Decl {
    FunDecl(Id name, Expr* body, List<Arg>* args, Type* ret) :
        Decl(Fun), name(name), args(args), ret(ret), body(body) {}

    Id name;
    List<Arg>* args;
    Type* ret; // If the function explicitly defines one.
    Expr* body;
};

struct AliasDecl: Decl {
    AliasDecl(SimpleType* type, Type* target): Decl(Alias), type(type), target(target) {}
    SimpleType* type;
    Type* target;
};

struct ClassDecl: Decl {
    ClassDecl(SimpleType* type, List<Decl*>* decls): Decl(Class), type(type), decls(decls) {}
    SimpleType* type;
    List<Decl*>* decls;
};

struct InstanceDecl: Decl {
    InstanceDecl(SimpleType* type, List<Decl*>* decls): Decl(Instance), type(type), decls(decls) {}
    SimpleType* type;
    List<Decl*>* decls;
};

struct ForeignDecl: Decl {
    ForeignDecl(Id externName, Id localName, Id from, Type* type): Decl(Foreign), externName(externName), localName(localName), from(from), type(type) {}
    Id externName;
    Id localName;
    Id from;
    Type* type;
};

struct DataDecl: Decl {
    DataDecl(SimpleType* type, List<Con>* cons): Decl(Data), cons(cons), type(type) {}
    List<Con>* cons;
    SimpleType* type;
};

/*
 * Modules
 */

struct Import: Node {
    Id from;
    bool qualified;
    Id localName;
    ASTArray<Id> include;
    ASTArray<Id> exclude;
};

struct Fixity: Node {
    enum Kind {
        Left, Right
    };

    Id op;
    U32 precedence;
    Kind kind;
};

struct Export: Node {
    Id name;
    Id exportName;
    bool qualified;
};

struct Module {
    Id name;
    ASTArray<Import> imports;
    ASTArray<Decl*> decls;
    ASTArray<Fixity> ops;
    ASTArray<Export> exports;
};