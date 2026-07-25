#pragma once

#include "../util/container.h"

namespace ast {

struct ParseRegion;

struct Decl;
struct Expr;
struct Type;
struct Pat;
struct Constraint;
struct TupArg;
struct TupField;
struct ArgDecl;

using ParseBase = RegionBase<ParseRegion>;

template<class T>
using ParsePtr = RegionPtr<ParseRegion, T>;

template<class T>
using ParseList = SmallList<ParseRegion, T, false>;

using ConstraintList = ParseList<Constraint>;
using DeclList = ParseList<Decl>;

enum class BindType: U8 {
    Borrow,
    Ref,
    Sink,
    Set,
};

struct Attribute {
    LocationId source;
    StringId name;
    ParseList<TupArg> args;
};

using AttrList = ParseList<Attribute>;

// The type of literals is stored in the container referencing them;
// otherwise we would double the amount of memory used by them.
// We want this to be 4-byte aligned, so the 64-bit members have accessor functions instead.
struct Literal {
    enum Kind: U8 {
        Int,
        Double,
        Float,
        Char,
        String,
        Bool,
    };

    union Access {
        U32 a[2];
        U64 i;
        double d;
    };

    union {
        U32 a[2];
        float f;
        WChar32 c;
        StringId s;
        bool b;
    };

    double d() const {
        Access v { .a = { a[0], a[1] } };
        return v.d;
    }

    void d(double d) {
        Access v { .d = d };
        a[0] = v.a[0];
        a[1] = v.a[1];
    }

    U64 i() const {
        Access v { .a = { a[0], a[1] } };
        return v.i;
    }

    void i(U64 i) {
        Access v { .i = i };
        a[0] = v.a[0];
        a[1] = v.a[1];
    }
};

/*
 * Types
 */

struct FunType;
struct AppType;

struct Type {
    enum Kind: U8 {
        Error, // Placeholder for parse errors.
        Unit,  // The empty unit type.
        Con,   // A type name for a named type.
        Ptr,   // A raw pointer to a type.
        Ref,   // A reference to a type.
        Gen,   // A generic or polymorphic named type.
        Tup,   // A tuple type with optionally named fields.
        Fun,   // A function type.
        App,   // Application of higher-kinded type.
        Arr,   // An array of a type.
        Map,   // A map from one type to another.
    };

    struct MapPayload {
        ParsePtr<Type> from;
        ParsePtr<Type> to;
    };

    struct ArrPayload {
        ParsePtr<Type> type;
        ParsePtr<Expr> length;
    };

    union {
        StringId name;
        ParsePtr<Type> to;
        ParsePtr<FunType> fun;
        ParsePtr<AppType> app;

        struct {
            ParseList<TupField> fields;
        } tup;

        struct MapPayload map;
        struct ArrPayload arr;
    };

    ParsePtr<AttrList> attributes;
    LocationId source: 28;
    Kind kind: 4;
};

struct FunType {
    ParseList<ArgDecl> args;
    Type ret;
};

struct AppType {
    Type base;
    ParseList<Type> args;
};

/*
 * Pats
 */

struct FieldPat {
    StringId field;
    ParsePtr<Pat> pat;
};

struct Pat {
    enum Kind: U8 {
        Error, // Placeholder for parse errors.
        Var,
        Any,
        Tup,
        Con,
        Arr,
        Rest,
        Range,
        Lit, // Must be last; the literal type is (kind - Kind::Lit).
    };

    union {
        StringId var;
        Literal lit;
        ParseList<FieldPat> tup;
        ParseList<Pat> arr;

        struct {
            StringId name;
            ParsePtr<Pat> pats;
        } con;

        struct {
            ParsePtr<Pat> from;
            ParsePtr<Pat> to;
        } range;
    };

    StringId asVar;
    LocationId source: 27;
    Kind kind: 5;
};

/*
 * Exprs
 */

struct AppExpr;
struct InfixExpr;
struct PrefixExpr;
struct IfExpr;
struct FunExpr;
struct TupUpdateExpr;
struct ConExpr;
struct FieldExpr;
struct CoerceExpr;
struct AssignExpr;
struct ForExpr;
struct WhileExpr;
struct MatchExpr;
struct RangeExpr;

struct IfCase;
struct Alt;
struct FormatChunk;
struct MapArg;
struct VarDecl;

struct Expr {
    enum Kind: U8 {
        Error, // Placeholder for parse errors.
        Multi,
        Var,
        App,
        Sub,
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
        Match,
        Range,
        Ret,
        Lit, // Must be last; the literal type is (kind - Kind::Lit).
    };

    union {
        ParseList<Expr> multi;
        ParsePtr<Expr> nested;
        Literal lit;
        StringId var;
        ParsePtr<AppExpr> app;
        ParsePtr<AppExpr> sub;
        ParsePtr<InfixExpr> infix;
        ParsePtr<PrefixExpr> prefix;
        ParsePtr<IfExpr> singleIf;
        ParseList<IfCase> multiIf;
        ParseList<FormatChunk> format;
        ParsePtr<Expr> ret;
        ParsePtr<FunExpr> fun;
        ParseList<MapArg> map;
        ParseList<Expr> arr;
        ParseList<TupArg> tup;
        ParsePtr<TupUpdateExpr> tupUpdate;
        ParsePtr<ConExpr> con;
        ParsePtr<FieldExpr> field;
        ParsePtr<CoerceExpr> coerce;
        ParsePtr<AssignExpr> assign;
        ParsePtr<WhileExpr> whileLoop;
        ParsePtr<ForExpr> forLoop;
        ParseList<VarDecl> decl;
        ParsePtr<MatchExpr> match;
        ParsePtr<RangeExpr> range;
    };

    LocationId source: 26;
    Kind kind: 6;
};

struct IfCase {
    Expr cond;
    Expr then;
};

struct Alt {
    Pat pat;
    Expr expr;
};

struct Arg {
    LocationId source;
    StringId name;
    ParsePtr<Type> type;    // nullable
    ParsePtr<Expr> def;     // nullable
    BindType bind;
};

struct FormatChunk {
    StringId string;
    ParsePtr<Expr> format;  // nullable
};

struct TupField {
    StringId name;
    Type type;
    ParsePtr<Expr> def;     // nullable
};

struct TupArg {
    StringId name;
    Expr value;
};

struct ArgDecl {
    Type type;
    StringId name;
    BindType bind;
};

struct MapArg {
    Expr key;
    Expr value;
};

struct VarDecl {
    Pat pat;
    ParsePtr<Expr> content;    // nullable
    ParsePtr<Expr> in;         // nullable; if this is set, content must also be set.
    ParseList<Alt> alts; // if this is set, content must also be set.
    BindType bind;
};

// -----------------------------------------------

struct AppExpr {
    Expr callee;
    ParseList<TupArg> args;
};

struct InfixExpr {
    Expr lhs;
    Expr rhs;
    Expr op;
    bool ordered = false;
};

struct PrefixExpr {
    Expr on;
    Expr op;
};

struct IfExpr {
    Expr cond;
    Expr then;
    Maybe<Expr> otherwise;
};

struct FunExpr {
    ParseList<Arg> args;
    Expr body;
};

struct TupUpdateExpr {
    Expr value;
    ParseList<TupArg> args;
};

struct ConExpr {
    Type type;
    ParseList<TupArg> args;
};

struct FieldExpr {
    Expr target;
    Expr field;
};

struct CoerceExpr {
    Expr target;
    Type type;
};

struct AssignExpr {
    Expr target;
    Expr value;
};

struct ForExpr {
    Pat pat;
    Expr from;
    Expr body;
    ParsePtr<Expr> to, step;
    bool reverse;
};

struct WhileExpr {
    Expr cond;
    Expr body;
};

struct MatchExpr {
    Expr pivot;
    ParseList<Alt> alts;
};

struct RangeExpr {
    Expr from;
    Expr to;
    bool reverse;
};

inline bool isLiteral(const Expr& e) {
    return e.kind >= Expr::Lit;
}

inline bool isTerminating(const Expr& e) {
    return e.kind == Expr::Ret;
}

/*
 * Decls
 */

struct SimpleType {
    StringId name;
    ParseList<StringId> kind;
};

struct Con {
    StringId name;
    ParsePtr<Type> content;
    AttrList attributes;
    LocationId source;
};

struct Decl {
    enum Kind: U8 {
        Error, // Placeholder for parse errors.
        Fun,
        Alias,
        Data,
        Trait,
        Instance,
        Foreign,
        Stmt,
        Attr,
    };

    union {
        Expr stmt;

        struct {
            SimpleType type;
            Type target;
        } alias;

        struct {
            SimpleType type;
            ConstraintList constraints;
            DeclList decls;
        } trait;

        struct {
            Type type;
            DeclList decls;
        } instance;

        struct {
            StringId externName;
            StringId localName;
            StringId from;
            Type type;
        } foreign;

        struct {
            StringId name;
            Type type;
        } attr;

        struct {
            ParseList<Con> cons;
            SimpleType type;
            ConstraintList constraints;
        } data;

        struct {
            StringId name;
            ConstraintList constraints;
            ParseList<Arg> args;
            ParsePtr<Type> ret;  // nullable.
            ParsePtr<Expr> body; // nullable.
            bool implicitReturn;
        } fun;
    };

    AttrList attributes;
    LocationId source: 26;
    Kind kind: 4;
    bool exported: 1;
    bool qualified: 1;
};

/*
 * Type constraints.
 */

struct Constraint {
    enum Kind: U8 {
        Error,     // Placeholder for parse errors.
        Any,       // Any type allowed.
        Class,     // Type must implement this class.
        Field,     // Type must have a field with this name and type.
        Function,  // There must exist a function with this signature.
    };

    union {
        StringId name;
        SimpleType type;

        struct {
            StringId typeName;
            StringId fieldName;
            ParsePtr<Type> type;
        } field;

        struct {
            StringId name;
            ParsePtr<Type> type;
        } fun;
    };

    LocationId source: 28;
    Kind kind: 4;
};

struct Import {
    StringId from;
    StringId localName;

    ParseList<StringId> include;
    ParseList<StringId> exclude;

    LocationId source: 31;
    bool qualified: 1;
};

struct Fixity {
    enum Kind: U8 {
        Left, Right
    };

    StringId op;
    LocationId source;
    U32 precedence: 30;
    Kind kind: 2;
};

struct Module {
    Region<ParseRegion> region;
    StringId name;

    ParseList<Import> imports;
    ParseList<Decl> decls;
    ParseList<Fixity> ops;

    U32 errorCount = 0;
    U32 warningCount = 0;
};

} // namespace ast
