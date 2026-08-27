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
struct TupUpdateArg;
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
};

// Marks a function type, function declaration, or lambda literal as compiling
// via the CPS/stack-sharing strategy instead of an ordinary call: `Lens` shares
// the caller's stack for a single continuation invocation, `Iter` generalizes
// this to a resumable, possibly-multi-yield coroutine.
enum class FunKind: U8 {
    Plain,
    Lens,
    Iter,
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
        String,
        Bool, // Unreachable from source today: True/False are ConIDs (nullary constructors), not literal tokens.
    };

    union Access {
        U32 a[2];
        U64 i;
        double d;
    };

    union {
        U32 a[2];
        float f;
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
        Error,  // Placeholder for parse errors.
        Unit,   // The empty unit type.
        Con,    // A type name for a named type.
        Ptr,    // An unchecked raw pointer to a type (sigil '%', aliased Ptr(a)).
        Ref,    // A checked reference to a type (sigil '*', aliased Ref(a)).
        Borrow, // An exclusive, writable reference to a type (sigil '&').
        Shared, // A shared reference to a type (sigil '\''). Analysis-Borrows.md §3.2: the two
                // differ in capability and in nothing else, which is why `&` no longer means one
                // thing on a parameter and another on a result.
        Gen,    // A generic or polymorphic named type.
        Tup,    // A tuple type with optionally named fields.
        Fun,    // A function type.
        App,    // Application of higher-kinded type.
        Arr,    // An array of a type.

        /*
         * `[T *_]` - a fixed array whose count is the literal written at it.
         *
         * A kind of its own rather than a flag on `ArrPayload`, because the payload is two
         * `ParsePtr`s and a third field would widen the eight-byte union every `ast::Type` carries.
         * `length` is null here, exactly as it is for the growable `[T]`, and what separates the two
         * is which kind was written.
         *
         * It resolves *only* where a literal supplies the count - the `::` in an expression and the
         * one in a constant. `resolveType` reports it anywhere else, since a parameter, a field or a
         * return type has nothing to take a count from.
         */
        ArrInferred,

        Map,    // A map from one type to another.

        /*
         * An integer literal written where a type is - `Vec(Float, 4)`.
         *
         * The one position that has a number in it and no expression around it. `[T *n]` already has
         * one and reads it as an `Expr`, because `*n` is written outside the type; a type argument
         * has no such syntax to hang it off, so the number arrives as an argument and says here that
         * it is one.
         *
         * It is deliberately *not* accepted as a type: `resolveType` reports it, and the only reader
         * is the vector constructor, which is asking for a lane count rather than for a type. That
         * keeps this from being the beginning of const generics - a feature this is not - while
         * still letting the one constructor that needs a number take one.
         */
        Lit,

        /*
         * A loan group written where a type argument is - `Store(kept', a)`, §4.7 and §9.3.
         *
         * A kind of its own for the same reason `Lit` is one: the position takes a type, and this
         * is the one thing written there that is not one. A *bare* positional group cannot work -
         * `Store('kept)` already parses as `Store` applied to a shared reference to the variable
         * `kept`, which `test/parser/Type.yana` pins - so the trailing tick is what says the
         * argument fills a slot rather than a type parameter.
         *
         * `name` is the group's label. Like `Lit`, it is deliberately not accepted as a type:
         * resolveType reports it, and the only readers are the two positions that are asking for a
         * slot rather than for a type.
         */
        Loan,
    };

    struct MapPayload {
        ParsePtr<Type> from;
        ParsePtr<Type> to;
    };

    struct ArrPayload {
        ParsePtr<Type> type;
        ParsePtr<Expr> length;
    };

    /*
     * `Kind::Borrow` and `Kind::Shared` - the reference, and the loan group it was written in.
     *
     * `to` is first so that it aliases the union's bare `to`, which is what every reader of a
     * reference's target already uses and what keeps this from touching them. `group` is null for
     * the signature's one anonymous group, which is Analysis-Borrows.md §4.8 rule 1 and every
     * reference written in `lib/`.
     *
     * A name here and an index in the semantic type: a group is signature-local (see LoanGroup), so
     * the name exists to match one occurrence against another within one signature and to say which
     * one a diagnostic is about, and nothing outside the signature can refer to it.
     */
    struct RefPayload {
        ParsePtr<Type> to;
        StringId group;
    };

    union {
        StringId name;
        ParsePtr<Type> to;
        struct RefPayload ref;
        ParsePtr<FunType> fun;
        ParsePtr<AppType> app;

        // `Kind::Lit` - the integer literal a lane count is written as.
        ParsePtr<Expr> lit;

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
    FunKind kind = FunKind::Plain;

    // `iter () -> ->T` - see Decl::fun::retBind, which is the same fact on a declaration. Here so
    // that the convention composes into a function type and through a generic parameter with no
    // further rule, which is what makes it a fact about the signature rather than about the body.
    BindType retBind = BindType::Borrow;
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
        Section,
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

        // `a..b` matches the half-open interval and `a..=b` the closed one, the same two spellings
        // a `for` header uses. Either bound may be absent (`_..b`, `a.._`), which makes the range
        // one-sided rather than open at that end.
        struct {
            ParsePtr<Pat> from;
            ParsePtr<Pat> to;
            bool inclusive;
        } range;

        // An operator section: the matched value is the operator's left operand and `bound` is
        // its right one, so `>0` matches a value greater than zero.
        struct {
            StringId op;
            ParsePtr<Pat> bound;
        } section;
    };

    StringId asVar;

    /*
     * How the names under this pattern reach what they name - the same three conventions a parameter
     * and a `let` are written with, in the same place and with the same meaning.
     *
     * `Borrow` is the default and is what a pattern has always done: the name refers to the storage
     * the pivot occupies, and the pivot keeps owning it. `Sink` is `Just(->v)`, which takes the
     * payload out and leaves the pivot moved from - the only way to get an owned value out of a
     * container of them, and the reason this field exists.
     *
     * `Ref` is refused on its own, and Analysis-Language.md §2 is why the refusal is narrower than
     * it looks: matching does not establish exclusive access, so `&` *meaning borrow* has nothing to
     * be true of. It is not the whole of what `&` means. `Just(&->v)` is `&` on the other axis -
     * this binding now owns writable storage - and that is a statement a move can make good on,
     * which is what `sink` beside `Ref` says.
     */
    BindType bind = BindType::Borrow;

    // `Just(&->v)`: the payload taken out of the pivot, into storage the name may write through.
    // Only ever set beside `Ref`; a bare `->` is `Sink` and says the same about the source alone.
    bool sink = false;

    /*
     * Set on a literal pattern written with a leading `-`.
     *
     * The sign is carried rather than folded into the magnitude, for the reason
     * `resolve/const.cpp`'s `WrittenNumber` gives at length: the lexer produces only the magnitude,
     * and what the number *is* is a question about the type it will be matched against. Folded here,
     * `-1` and `18446744073709551615` are one pattern, so a `U64` pivot cannot tell a mask it holds
     * from a negative it does not; and folding `I64`'s own minimum is signed overflow besides.
     */
    bool negative = false;

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
struct IsExpr;
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
        Yield,
        Break,
        Continue,
        Is,
        Try,
        Unwrap,
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
        ParsePtr<Expr> yield;
        ParsePtr<Expr> breakValue; // nullable
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
        ParsePtr<IsExpr> is;
        ParsePtr<Expr> tryValue; // The operand of a `?` suffix.

        /*
         * The operand of a `?.` suffix - what it unwraps.
         *
         * The *unwrapping* alone, with no suffix of its own: `a?.b` is a field of one of these,
         * `a?.[i]` a subscript of one and `a?.(x)` a call of one. That is what makes the three
         * spellings one node and one rule, and it is why the suffix after `?.` needs no special
         * case anywhere below the parser - a field of an unwrap is an ordinary field.
         */
        ParsePtr<Expr> unwrap;
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

    // Set by the `return` marker and by the `'` that replaces it: borrows in the function's result
    // may be rooted in this argument, in the signature's one anonymous group. A *labelled* group is
    // written on the parameter's type instead - `&a: src'T` - because that is where a group is
    // written everywhere else; see Parser::ArgBinding. The group is part of the function's type
    // rather than of this declaration.
    bool returnRoot = false;

    // Set by the `@lazy` marker: the argument is not evaluated at the call site, and reading the
    // parameter inside the callee is what runs it. Part of the function's type, like the two
    // markers above.
    bool lazy = false;

    /*
     * Set by `@caller` - Design-Test.md §11.1's F2. The compiler fills this position, at every call
     * that leaves it out, with a constant about the *call site*.
     *
     * Two fills, told apart by whether a source parameter was named. `@caller at: Site` is the
     * call's file, line, column and enclosing function; `@caller(source: p) text: String` is the
     * source text of the expression that reached parameter `p`. `callerSource` is that name, and 0
     * for the first form.
     *
     * Not part of the function's type, unlike the three markers above: what a `@caller` position
     * means to a caller is "you may leave this out", which is what a default already means. Nothing
     * about the *value* the callee receives differs from an ordinary defaulted argument.
     */
    bool caller = false;
    StringId callerSource {};
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

/*
 * One field replacement of a tuple update - `{v | origin: p}` or `{v | .origin.x: 1}`.
 *
 * The path is kept whole rather than rewritten into nested updates, because a rewrite would name
 * the source once per level: `{f() | .a.b: 1}` would call `f` twice, and `{v | .a.b: 1, .a.c: 2}`
 * would build two independent copies of `v.a` and let the second replace the first. One path into
 * one copy is both what the user wrote and the only reading under which two paths sharing a prefix
 * compose.
 */
struct TupUpdateArg {
    ParseList<StringId> path;
    Expr value;
};

struct ArgDecl {
    Type type;
    StringId name;
    BindType bind;
    bool returnRoot = false;  // See Arg::returnRoot; a function type carries the same marker.
    bool lazy = false;        // See Arg::lazy; likewise.
};

struct MapArg {
    Expr key;
    Expr value;
};

/*
 * A `let`, and the two questions its sigils answer - Analysis-Language.md §2.
 *
 * They are different questions and they had one slot. `->` says *where the value came from*: this is
 * a destructive read of a place somebody else owns. `&` says *how this binding may be used*:
 * writable. A binding can want either, both or neither, and while there was one `BindType` to say it
 * in, `let &f = openFile(p, a)?` was a program with no spelling - the two diagnostics it drew
 * pointed at each other.
 *
 * So `bind` is the use axis and carries `Borrow` or `Ref`, and `sink` is the source axis. The bare
 * `->` keeps `BindType::Sink` rather than becoming `Borrow` plus the flag, because a `->` binding is
 * still immutable and `Sink` is the name the rest of the compiler already reads that as.
 */
struct VarDecl {
    Pat pat;
    ParsePtr<Expr> content;    // nullable
    ParsePtr<Expr> in;         // nullable; if this is set, content must also be set.
    ParseList<Alt> alts; // if this is set, content must also be set.
    BindType bind;

    // `let &->x`: the initializer is a destructive read of a place that has another name, and the
    // storage it lands in is writable through this one. Only ever set beside `BindType::Ref` - a
    // bare `->` is `BindType::Sink`, which says the same thing about the source on its own.
    bool sink = false;

    // Attributes written before the binding - `@heap let big = ...`. Parsed but not interpreted
    // here; which ones mean anything is a resolve-stage question.
    AttrList attributes;
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

// `value is pat` - a condition that binds. The pattern is held by value, as a declaration's is,
// because the exhaustiveness space keeps patterns by address.
struct IsExpr {
    Expr value;
    Pat pat;
};

struct FunExpr {
    ParseList<Arg> args;
    Expr body;
    FunKind kind = FunKind::Plain;
};

struct TupUpdateExpr {
    Expr value;
    ParseList<TupUpdateArg> args;
    BindType bind = BindType::Borrow;
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

/*
 * `for pat in from [(`..`|`..=`|`downto`) to] [step s]: body`.
 *
 * With no `to`, the loop consumes an iterator and `reverse`/`inclusive` say nothing. With one, it
 * counts over an interval, and the two flags say which interval: `..` and `downto` both name the
 * half-open `[low, high)` and differ only in the direction they walk it, so `0..n` and `n downto 0`
 * cover the same values. `..=` closes the upper end; there is no descending form of that, since the
 * bound `downto` excludes is the one written first - see Design.md's Expressions.
 */
struct ForExpr {
    Pat pat;
    Expr from;
    Expr body;
    ParsePtr<Expr> to, step;
    bool reverse;
    bool inclusive;
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
    return e.kind == Expr::Ret || e.kind == Expr::Break || e.kind == Expr::Continue;
}

/*
 * Decls
 */

/*
 * One parameter of a declaration head - Implementation-Const-Generics.md §1.1.
 *
 * A bare `name` is a type parameter and `name: T` is a *value* parameter of type `T`, so the
 * annotation is what distinguishes the two and its absence is the whole of what says "a type". That
 * is the reason the annotation is a type rather than a keyword: nothing else in the grammar changes,
 * and admitting a new sort of const parameter later is a semantic ruling rather than a production.
 */
struct GenParam {
    StringId name;

    // Null for a type parameter. Parsed with parseAType and not parseType, so that the `->` of a
    // class head's functional dependency stays a separator - §1.4.
    ParsePtr<Type> type;

    /*
     * `a = Int`, `n: Int = 4` - what an application that omits this parameter gets, and null for a
     * parameter with no default.
     *
     * One field for both kinds, because the two spellings are the same question asked of a type and
     * of a number, and a written type argument is already one production either way - a `Con` and a
     * `Lit` both arrive here. Which of the two is admissible is decided by the parameter's own kind
     * when the default is resolved, not here.
     *
     * Parsed with parseAType for §1.4's reason, the same one the annotation beside it has: a class
     * head's `->` has to stay a separator, so `class C(a, b = Int -> c)` is a default of `Int` and
     * not a default of `Int -> c`.
     */
    ParsePtr<Type> def;

    // Where the parameter was written. A head parameter is a declaration like any other and every
    // diagnostic about one - a repeated name, an inadmissible annotation, a default at the wrong
    // kind - belongs on it rather than on the declaration it is part of.
    LocationId source = kNullLocation;
};

struct SimpleType {
    StringId name;
    ParseList<GenParam> kind;

    /*
     * Where a class head's `->` was written: the index of the first parameter the ones before it
     * determine, and 0 for a head with no arrow.
     *
     * `class Contiguous(c -> a)` is a promise that one `c` has one `a`, which is what lets a call
     * that binds only `c` read `a` back off the instance it selects. Zero is a safe sentinel
     * because the first parameter can never be a determined one - something has to determine it.
     *
     * Only a class head may carry one. `data` and `alias` share this production and pass false for
     * the arrow, since neither has instances for a dependency to be a promise about.
     */
    U16 determined = 0;
};

struct Con {
    StringId name;
    ParsePtr<Type> content;
    AttrList attributes;
    LocationId source;
};

/*
 * One class named in a `deriving (...)` clause - Analysis-Derive.md §3's `newtype` shape.
 *
 * The class alone, unapplied, because the type is the declaration the clause is attached to. That is
 * the same shape `default FromInt = Int` uses, and it is what makes the standalone form
 * `deriving Logic(OpenFlags)` a different production rather than this one with the argument written
 * out - see Analysis-Extensibility.md, which defines the clause as sugar for the declaration.
 *
 * The location is the class name's own, not the clause's: every diagnostic a derivation produces is
 * about one of the classes in the list, and pointing at the whole clause would make a list of four
 * report four times at the same caret.
 */
struct Derive {
    StringId name;
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

        // Kind::Error. The name of the declaration that did not parse, where one was read before
        // the parser gave up, and 0 where none was. A half-written declaration is still the one
        // the cursor is in, and an editor that can name it can complete inside it.
        StringId errorName;

        struct {
            SimpleType type;
            Type target;

            // The `deriving (...)` clause, empty where none was written. Only a *qualified* alias
            // can carry one - a plain alias is its target, so there is no second type for an
            // instance to be about - and that is reported in the parser rather than here.
            ParseList<Derive> derives;
        } alias;

        struct {
            SimpleType type;
            ConstraintList constraints;
            DeclList decls;
        } trait;

        struct {
            Type type;
            ConstraintList constraints;
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

            /*
             * `-> ->T` - the convention what this hands over is received under, and only for a
             * `lens`/`iter` (Analysis-Language.md §3a).
             *
             * A yielded value has a binding convention exactly as an argument does, and there was no
             * way to declare it anything but a borrow - so `?` could not reach an owned payload out
             * of a `for` body, and every consumer of a fallible iterator wrote a `match` instead.
             * It lives on the *type* rather than on the body, because a function summary is part of
             * its published interface: inferring it from `yield ->x` would make a signature depend
             * on whether the body is visible.
             */
            BindType retBind;
            ParsePtr<Expr> body; // nullable.
            bool implicitReturn;
            FunKind kind;
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

        // `n: Int` - a const parameter of this context, Implementation-Const-Generics.md §1.2. Read
        // as a family member of the two above rather than a rule of its own: `a.name: Int` says `a`
        // has a field, `f: (a) -> b` says `f` is callable, and this one says `n` is an `Int`.
        Const,
    };

    union {
        StringId name;

        /*
         * `Num(a)`, `Num(Vec(I16, n))` - Implementation-Const-Generics.md §10.2.
         *
         * A payload of its own rather than the `SimpleType` a declaration head uses, because the two
         * are opposite things that looked alike: a head *binds* parameters and its list is therefore
         * bare identifiers, while a constraint *applies* them and its list is arguments. They shared
         * one production only because until const generics every argument anyone wrote happened to
         * be a bare variable.
         *
         * The arguments are whole written types, read by `parseTypeApplicationArg` - the same
         * production a type application's argument uses, so `Vec(I16, n)`, `Vec(I16, 4)`, `[Int *n]`
         * and `Pair(k, v)` all arrive with no grammar written for any of them.
         */
        struct {
            StringId name;
            ParseList<Type> args;
        } klass;

        struct {
            StringId typeName;
            StringId fieldName;
            ParsePtr<Type> type;
        } field;

        struct {
            StringId name;
            ParsePtr<Type> type;
        } fun;

        struct {
            StringId name;
            ParsePtr<Type> type;

            // `n: Int = 0` - the same default a head parameter carries, and null without one. See
            // GenParam::def, which this is the context-list spelling of.
            ParsePtr<Type> def;
        } constant;
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

/*
 * How a file says which module it is part of - Analysis-Modules.md §2.1.
 *
 * `Directory` is the absent declaration and the common case: a file belongs to the module formed by
 * the directory it sits in, and says nothing. The two opt-outs are the only forms with syntax, and
 * the bare one is the one that reads as what the word means - "this file is a module".
 */
enum class Membership: U8 {
    Directory, /// Nothing written. The file is part of its directory's module.
    Own,       /// `module`. The file is a module of its own, named by its path.
    Named,     /// `module M`. The file joins M, which must be a proper prefix of its path name.
};

/*
 * One parsed file.
 *
 * `name` is the file's own path-derived name and not the module it ends up in - the two stopped
 * being the same thing when a module became a directory. It is what a `LocationId` is quoted
 * through and what a language server turns back into a URI, and both of those are questions about a
 * file; which module the file joined is `ModuleGroup::name`.
 *
 * There is no region here. Every AST in a compilation is allocated in `Context::parseRegion`, so
 * that the files of one module can be addressed through one `ParseBase` - see the comment there.
 */
struct Module {
    StringId name;

    ParseList<Import> imports;
    ParseList<Decl> decls;
    ParseList<Fixity> ops;

    Membership membership = Membership::Directory;

    // The name written in `module M`, or none. Checked against the file's path by whoever built the
    // module map, since it is the only thing that knows where the file is.
    StringId joins {};

    // Where the declaration was written, for the diagnostics the check above reports. Null when
    // nothing was written.
    LocationId membershipSource {};

    U32 errorCount = 0;
    U32 warningCount = 0;
};

/*
 * The files of one module, in the order the resolver reads them.
 *
 * Non-owning: the files belong to whoever parsed them - a `SourceEntry` for a project file and
 * `Program::embeddedAsts` for a library one - and this is the grouping laid over them. The order is
 * path order, which is what makes every pass over a module deterministic and what §2.3's
 * declaration order is built on.
 */
struct ModuleGroup {
    StringId name;

    /*
     * `SmallArray` rather than `Array`, and eight is the ordinary module - see util/README.md.
     *
     * There is one of these per module in the program, and most modules are one file, so an ordinary
     * `Array` was a heap allocation apiece to hold a single pointer. Past eight it behaves exactly
     * like one, which the two the library ships - `Core` and `Native` - are the only things in a
     * compilation that reach.
     *
     * Safe as an element of `ModuleMap::groups`, which grows while it is being filled: the inline
     * buffer is not pointed at from inside the object, so relocating one relocates it correctly.
     * Nothing may hold the address of an entry across a move of the group itself, and nothing does -
     * every reader indexes or iterates.
     */
    SmallArray<Module*, 8> files;
};

} // namespace ast
