#pragma once

#include "../compiler/context.h"
#include "../compiler/diagnostics.h"

namespace ast {

struct TupArg;
struct Constraint;
struct Expr;

struct Attribute: Node {
    Attribute(StringId name, List<TupArg>* args): name(name), args(args) {}
    StringId name;
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
    TupField(Type* type, StringId name, Expr* def) : type(type), def(def), name(name) {}

    Type* type;
    Expr* def;
    StringId name;
};

struct ArgDecl {
    ArgDecl(Type* type, StringId name): type(type), name(name) {}
    Type* type;
    StringId name;
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
    ConType(StringId con): Type(Con), con(con) {}
    StringId con;
};

struct GenType: Type {
    GenType(StringId con): Type(Gen), con(con) {}
    StringId con;
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
        StringId s;
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

    StringId asVar;
    Kind kind;

    Pat(Kind k, StringId asVar = 0) : asVar(asVar), kind(k) {}
};

struct VarPat: Pat {
    VarPat(StringId var, StringId asVar = 0): Pat(Var, asVar), var(var) {}
    StringId var;
};

struct LitPat: Pat {
    LitPat(Literal lit, StringId asVar = 0): Pat(Lit, asVar), lit(lit) {}
    Literal lit;
};

struct FieldPat {
    FieldPat(StringId field, Pat* pat): field(field), pat(pat) {}
    StringId field;
    Pat* pat;
};

struct TupPat: Pat {
    TupPat(List<FieldPat>* fields, StringId asVar = 0): Pat(Tup, asVar), fields(fields) {}
    List<FieldPat>* fields;
};

struct ConPat: Pat {
    ConPat(StringId constructor, Pat* pats): Pat(Con), constructor(constructor), pats(pats) {}
    StringId constructor;
    Pat* pats;
};

struct ArrayPat: Pat {
    ArrayPat(List<Pat*>* pats): Pat(Array), pats(pats) {}
    List<Pat*>* pats;
};

struct RestPat: Pat {
    RestPat(StringId var, StringId asVar = 0): Pat(Rest, asVar), var(var) {}
    StringId var;
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
    TupArg(StringId name, Expr* value): name(name), value(value) {}
    StringId name;
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
    StringId name;
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
    StringId string;
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
    VarExpr(StringId n): Expr(Var), name(n) {}
    StringId name;
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
    ForExpr(StringId var, Expr* from, Expr* to, Expr* body, Expr* step, bool reverse):
        Expr(For), var(var), from(from), to(to), body(body), step(step), reverse(reverse) {}

    StringId var;
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
    SimpleType(StringId name, List<StringId>* kind) : name(name), kind(kind) {}
    StringId name;
    List<StringId>* kind;
};

struct Con: Node {
    Con(StringId name, Type* content) : name(name), content(content) {}
    StringId name;
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
    FunDecl(StringId name, List<Constraint*>* constraints, Expr* body, List<Arg>* args, Type* ret, bool implicitReturn) :
        Decl(Fun), name(name), constraints(constraints), args(args), ret(ret), body(body), implicitReturn(implicitReturn) {}

    StringId name;
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
    ForeignDecl(StringId externName, StringId localName, StringId from, Type* type): Decl(Foreign), externName(externName), localName(localName), from(from), type(type) {}
    StringId externName;
    StringId localName;
    StringId from;
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
    AttrDecl(StringId name, Type* type): Decl(Attr), name(name), type(type) {}
    StringId name;
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
    AnyConstraint(StringId name): Constraint(Any), name(name) {}
    StringId name;
};

struct ClassConstraint: Constraint {
    ClassConstraint(SimpleType* type) : Constraint(Class), type(type) {}
    SimpleType* type;
};

struct FieldConstraint: Constraint {
    FieldConstraint(StringId typeName, StringId fieldName, Type* type):
        Constraint(Field), typeName(typeName), fieldName(fieldName), type(type) {}

    StringId typeName;
    StringId fieldName;
    Type* type;
};

struct FunctionConstraint: Constraint {
    FunctionConstraint(FunType type, StringId name): Constraint(Function), name(name), type(type) {}
    StringId name;
    FunType type;
};

/*
 * Modules
 */

struct Import: Node {
    StringId from;
    bool qualified;
    StringId localName;
    List<StringId>* include;
    List<StringId>* exclude;
};

struct Fixity: Node {
    enum Kind {
        Left, Right
    };

    Fixity(StringId op, U32 precedence, Kind kind): op(op), precedence(precedence), kind(kind) {}
    StringId op;
    U32 precedence;
    Kind kind;
};

struct Export: Node {
    StringId name;
    StringId exportName;
    bool qualified;
};

struct Module {
    Module(StringId name): name(name) {}

    StringId name;
    Array<Import> imports;
    Array<Decl*> decls;
    Array<Fixity> ops;
    Array<Export> exports;

    Arena buffer;

    U32 errorCount = 0;
    U32 warningCount = 0;
};

} // namespace ast
