#pragma once

#include "../compiler/context.h"
#include "../compiler/diagnostics.h"

namespace ast {

struct TupArg;
struct Constraint;
struct Expr;

struct Attribute: Node {
    Attribute(Id name, List<TupArg>* args): name(name), args(args) {}
    Id name;
    List<TupArg>* args;

    // This is easier than trying to make a special template function for values.
    Attribute* operator -> () {
        return this;
    }
};

/*
 * Types
 */

struct Type: Node {
    enum Kind {
        Error, // Placeholder for parse errors.
        Unit,  // The empty unit type.
        Con,   // A type name for a named type.
        Ptr,   // A raw pointer to a type.
        Ref,   // A reference to a type.
        Val,   // A flattened type.
        Gen,   // A generic or polymorphic named type.
        Tup,   // A tuple type with optionally named fields.
        Fun,   // A function type.
        App,   // Application of higher-kinded type.
        Arr,   // An array of a type.
        Map,   // A map from one type to another.
    } kind;

    List<Attribute>* attributes = nullptr;

    Type(Kind k) : kind(k) {}
    bool is(Kind kind) const {return this->kind == kind;}
};

struct TupField {
    TupField(Type* type, Id name, Expr* def) : type(type), def(def), name(name) {}

    Type* type;
    Expr* def;
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

struct RefType: Type {
    RefType(Type* type): Type(Ref), type(type) {}
    Type* type;
};

struct ValType: Type {
    ValType(Type* type): Type(Val), type(type) {}
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
        Bool,
    };

    union {
        double f;
        I64 i;
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
        Con,
        Array,
        Rest,
        Range
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
    ConPat(Id constructor, Pat* pats): Pat(Con), constructor(constructor), pats(pats) {}
    Id constructor;
    Pat* pats;
};

struct ArrayPat: Pat {
    ArrayPat(List<Pat*>* pats): Pat(Array), pats(pats) {}
    List<Pat*>* pats;
};

struct RestPat: Pat {
    RestPat(Id var, Id asVar = 0): Pat(Rest, asVar), var(var) {}
    Id var;
};

struct RangePat: Pat {
    RangePat(Pat* from, Pat* to): Pat(Range), from(from), to(to) {}
    Pat* from;
    Pat* to;
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

struct Arg: Node {
    Id name;
    Type* type;
    Expr* def;

    Arg* operator -> () {
        return this;
    }
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
        For,
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

struct VarDecl: Node {
    enum Mutability {
        Immutable,
        Ref,
        Val,
    };

    VarDecl(Pat* pat, Expr* content, Expr* in, List<Alt>* alts, Mutability mut):
        pat(pat), content(content), in(in), alts(alts), mut(mut) {}

    Pat* pat;
    Expr* content;
    Expr* in; // if this is set, content must also be set.
    List<Alt>* alts; // if this is set, content must also be set.
    Mutability mut;

    VarDecl* operator -> () {
        return this;
    }
};

struct DeclExpr: Expr {
    DeclExpr(List<VarDecl>* decls):
        Expr(Decl), decls(decls) {}

    List<VarDecl>* decls;
    bool isGlobal = false; // Whether this variable was defined in a global scope.
};

struct WhileExpr: Expr {
    WhileExpr(Expr* cond, Expr* loop): Expr(While), cond(cond), loop(loop) {}
    Expr* cond;
    Expr* loop;
};

struct ForExpr: Expr {
    ForExpr(Id var, Expr* from, Expr* to, Expr* body, Expr* step, bool reverse):
        Expr(For), var(var), from(from), to(to), body(body), step(step), reverse(reverse) {}

    Id var;
    Expr* from;
    Expr* to;
    Expr* body;
    Expr* step;
    bool reverse;
};

struct AssignExpr: Expr {
    AssignExpr(Expr* target, Expr* value): Expr(Assign), target(target), value(value) {}
    Expr* target;
    Expr* value;
};

struct CoerceExpr: Expr {
    CoerceExpr(Expr* target, ::ast::Type* kind): Expr(Coerce), target(target), kind(kind) {}
    Expr* target;
    ::ast::Type* kind;
};

struct FieldExpr: Expr {
    FieldExpr(Expr* target, Expr* field): Expr(Field), target(target), field(field) {}
    Expr* target; // Either a var, literal or a complex expression.
    Expr* field;  // Field to apply to.
};

struct ConExpr: Expr {
    ConExpr(ConType* type, List<TupArg>* args): Expr(Con), type(type), args(args) {}
    ConType* type;
    List<TupArg>* args;
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

struct Con: Node {
    Con(Id name, Type* content) : name(name), content(content) {}
    Id name;
    Type* content;
    List<Attribute>* attributes = nullptr;

    // This is easier than trying to make a special template function for values.
    Con* operator -> () {
        return this;
    }
};

struct Decl: Node {
    enum Kind {
        Error, // Placeholder for parse errors.
        Fun,
        Alias,
        Data,
        Class,
        Instance,
        Foreign,
        Stmt,
        Attr,
    } kind;

    List<Attribute>* attributes = nullptr;
    bool exported = false;

    Decl(Kind t): kind(t) {}
};

struct FunDecl: Decl {
    FunDecl(Id name, List<Constraint*>* constraints, Expr* body, List<Arg>* args, Type* ret, bool implicitReturn) :
        Decl(Fun), name(name), constraints(constraints), args(args), ret(ret), body(body), implicitReturn(implicitReturn) {}

    Id name;
    List<Constraint*>* constraints;
    List<Arg>* args;
    Type* ret; // If the function explicitly defines one.
    Expr* body;
    bool implicitReturn;
};

struct AliasDecl: Decl {
    AliasDecl(SimpleType* type, Type* target): Decl(Alias), type(type), target(target) {}
    SimpleType* type;
    Type* target;
};

struct ClassDecl: Decl {
    ClassDecl(SimpleType* type, List<Constraint*>* constraints, List<FunDecl*>* decls):
        Decl(Class), type(type), constraints(constraints), decls(decls) {}

    SimpleType* type;
    List<Constraint*>* constraints;
    List<FunDecl*>* decls;
};

struct InstanceDecl: Decl {
    InstanceDecl(Type* type, List<Decl*>* decls): Decl(Instance), type(type), decls(decls) {}
    Type* type;
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
    DataDecl(SimpleType* type, List<Con>* cons, List<Constraint*>* constraints, bool qualified):
        Decl(Data), cons(cons), type(type), constraints(constraints), qualified(qualified) {}

    List<Con>* cons;
    SimpleType* type;
    List<Constraint*>* constraints;
    bool qualified;
};

struct StmtDecl: Decl {
    StmtDecl(Expr* expr): Decl(Stmt), expr(expr) {}
    Expr* expr;
};

struct AttrDecl: Decl {
    AttrDecl(Id name, Type* type): Decl(Attr), name(name), type(type) {}
    Id name;
    Type* type;
};

/*
 * Type constraints.
 */

struct Constraint: Node {
    enum Kind {
        Error,     // Placeholder for parse errors.
        Any,       // Any type allowed.
        Class,     // Type must implement this class.
        Field,     // Type must have a field with this name and type.
        Function,  // There must exist a function with this signature.
    } kind;

    Constraint(Kind k) : kind(k) {}
};

struct AnyConstraint: Constraint {
    AnyConstraint(Id name): Constraint(Any), name(name) {}
    Id name;
};

struct ClassConstraint: Constraint {
    ClassConstraint(SimpleType* type) : Constraint(Class), type(type) {}
    SimpleType* type;
};

struct FieldConstraint: Constraint {
    FieldConstraint(Id typeName, Id fieldName, Type* type):
        Constraint(Field), typeName(typeName), fieldName(fieldName), type(type) {}

    Id typeName;
    Id fieldName;
    Type* type;
};

struct FunctionConstraint: Constraint {
    FunctionConstraint(FunType type, Id name): Constraint(Function), name(name), type(type) {}
    Id name;
    FunType type;
};

/*
 * Modules
 */

struct Import: Node {
    Id from;
    bool qualified;
    Id localName;
    List<Id>* include;
    List<Id>* exclude;
};

struct Fixity: Node {
    enum Kind {
        Left, Right
    };

    Fixity(Id op, U32 precedence, Kind kind): op(op), precedence(precedence), kind(kind) {}
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
    Module(Id name): name(name) {}

    Id name;
    Array<Import> imports;
    Array<Decl*> decls;
    Array<Fixity> ops;
    Array<Export> exports;

    Arena buffer;

    U32 errorCount = 0;
    U32 warningCount = 0;
};

} // namespace ast
