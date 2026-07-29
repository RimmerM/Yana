#pragma once

#include "../../compiler/context.h"
#include "../../util/container.h"

/*
 * A JavaScript syntax tree, and the last stage before text.
 *
 * It exists for two reasons that emitting characters directly would not serve. Operator precedence
 * is decided in one place - the formatter - rather than by whoever built the expression remembering
 * to parenthesize; and identifiers are chosen once, when a Name is created, so that a name is the
 * same everywhere it appears without the emitter having to re-derive it.
 *
 * It is deliberately a small subset of the language. Analysis-JS.md §3.6 rules out `with`, `delete`
 * and dynamic property names because all three deoptimize the containing function in every engine,
 * so none of them is expressible here at all; the only computed member access this tree can build
 * is an array index, which is what the compiler-built constant tables are read through.
 *
 * The tree lives in a region of its own, on the same terms as the resolve IR and the lower IR: a
 * node is a 32-bit offset rather than a pointer, nothing is freed individually, and the whole file
 * goes away at once. Two properties come with that and both are used here - a node is half the size
 * of a pointer-linked one on 64-bit, and a JsPtr keeps meaning the same node across a rebuild.
 */
namespace js {

struct Expr;
struct Stmt;
struct JsRegion;

using JsBase = RegionBase<JsRegion>;

template<class T>
using JsPtr = RegionPtr<JsRegion, T>;

template<class T, bool allowEmbed = true>
using JsList = SmallList<JsRegion, T, allowEmbed>;

/*
 * One JavaScript identifier.
 *
 * `text` is the final name, decided when the Name is created rather than when it is printed:
 * sanitizing `Num(Int).+` into something a parser accepts is a question with more than one answer,
 * and two functions whose names sanitize alike have to end up with different identifiers. Doing it
 * once is what makes that a solvable problem instead of a rule every use site has to agree on.
 */
struct Name {
    StringId text = 0;
};

enum class UnaryOp: U8 {
    Neg,     // -x
    Not,     // !x
    BitNot,  // ~x
};

enum class BinaryOp: U8 {
    Mul, Div, Rem,
    Add, Sub,
    Shl, Shr, Sar,
    Lt, Le, Gt, Ge,
    Eq, Ne,

    // `==` and `!=`, used only where one side is a reference. Two objects compare the same either
    // way, and the difference is the one that matters here: a property nothing attached reads back
    // as `undefined`, and `undefined == null` is exactly "there is no reference here".
    LooseEq, LooseNe,

    And, Xor, Or,
    LogicalAnd, LogicalOr,
};

struct Expr {
    enum Kind: U8 {
        Number,
        BigInt,
        String,
        Bool,
        Null,
        Undefined,
        Var,
        Field,
        Index,
        Array,
        Object,
        Unary,
        Binary,
        Ternary,
        Assign,
        Call,
        Function,
    };

    explicit Expr(Kind kind): kind(kind) {}
    Kind kind;
};

// A `number` literal. `integral` is set for a value that has to print without an exponent or a
// fraction, since the same double is a valid array index in one place and a float in another.
struct NumberExpr: Expr {
    NumberExpr(F64 value, bool integral): Expr(Number), value(value), integral(integral) {}
    F64 value;
    bool integral;
};

// A `bigint` literal - `123n`. This is what `Long` and `ULong` are (Analysis-JS.md §2.1).
struct BigIntExpr: Expr {
    BigIntExpr(U64 value, bool isSigned): Expr(BigInt), value(value), isSigned(isSigned) {}
    U64 value;
    bool isSigned;
};

struct StringExpr: Expr {
    explicit StringExpr(StringId value): Expr(String), value(value) {}
    StringId value;
};

struct BoolExpr: Expr {
    explicit BoolExpr(bool value): Expr(Bool), value(value) {}
    bool value;
};

struct NullExpr: Expr {
    NullExpr(): Expr(Null) {}
};

struct UndefinedExpr: Expr {
    UndefinedExpr(): Expr(Undefined) {}
};

struct VarExpr: Expr {
    explicit VarExpr(Name name): Expr(Var), name(name) {}
    Name name;
};

// `object.field`. The property is a Name rather than an expression, so there is no way to build a
// dynamic property access with it.
struct FieldExpr: Expr {
    FieldExpr(JsPtr<Expr> object, Name field): Expr(Field), object(object), field(field) {}
    JsPtr<Expr> object;
    Name field;
};

// `array[index]` - the one computed access this tree can express, for the compiler-built constant
// tables of Implementation-Generics.md and for host arrays.
struct IndexExpr: Expr {
    IndexExpr(JsPtr<Expr> array, JsPtr<Expr> index): Expr(Index), array(array), index(index) {}
    JsPtr<Expr> array;
    JsPtr<Expr> index;
};

struct ArrayExpr: Expr {
    ArrayExpr(): Expr(Array) {}
    JsList<JsPtr<Expr>, false> values;
};

struct Property {
    Name key;
    JsPtr<Expr> value;
};

// An object literal with fixed keys. Every value of one record type is built by exactly one of
// these, with the fields in one order, which is what gives the type one hidden class -
// Analysis-JS.md §2.3's "construction order is the JS equivalent of field offsets".
struct ObjectExpr: Expr {
    ObjectExpr(): Expr(Object) {}
    JsList<Property, false> properties;
};

struct UnaryExpr: Expr {
    UnaryExpr(UnaryOp op, JsPtr<Expr> value): Expr(Unary), value(value), op(op) {}
    JsPtr<Expr> value;
    UnaryOp op;
};

struct BinaryExpr: Expr {
    BinaryExpr(BinaryOp op, JsPtr<Expr> lhs, JsPtr<Expr> rhs): Expr(Binary), lhs(lhs), rhs(rhs), op(op) {}
    JsPtr<Expr> lhs;
    JsPtr<Expr> rhs;
    BinaryOp op;
};

struct TernaryExpr: Expr {
    TernaryExpr(JsPtr<Expr> cond, JsPtr<Expr> then, JsPtr<Expr> otherwise):
        Expr(Ternary), cond(cond), then(then), otherwise(otherwise) {}

    JsPtr<Expr> cond;
    JsPtr<Expr> then;
    JsPtr<Expr> otherwise;
};

struct AssignExpr: Expr {
    AssignExpr(JsPtr<Expr> target, JsPtr<Expr> value): Expr(Assign), target(target), value(value) {}
    JsPtr<Expr> target;
    JsPtr<Expr> value;
};

struct CallExpr: Expr {
    explicit CallExpr(JsPtr<Expr> callee): Expr(Call), callee(callee) {}
    JsPtr<Expr> callee;
    JsList<JsPtr<Expr>, false> args;

    /*
     * Set for the host intrinsics the integer tower reaches for - `Math.imul`, `BigInt.asIntN`,
     * `Number` - and for nothing else.
     *
     * They are calls in the emitted text and arithmetic in every other respect: they read nothing,
     * write nothing, and are there because JS has no operator for what they do. Saying so is what
     * lets `var v = Math.imul(a, b) | 0; p.x = v;` collapse the way the `+` next to it does.
     */
    bool pure = false;
};

using StmtList = JsList<JsPtr<Stmt>, false>;

/*
 * `function(a, b) { ... }` - an anonymous function expression, and what a closure is on this target.
 *
 * A function value is a host function rather than a `{code, env}` pair (see gen.cpp), so building
 * one is building a function that has the captures in scope. That is a closure in the host's own
 * sense: the engine allocates the context, the call is an ordinary call, and nothing has to be
 * unpacked at the call site.
 *
 * Exactly one of these is emitted per lifted lambda, inside the factory that supplies the
 * environment - see genFunction. There is no nesting beyond that, which is why this carries a body
 * rather than the generator carrying a stack of them.
 */
struct FunValueExpr: Expr {
    FunValueExpr(): Expr(Function) {}

    JsList<Name, false> args;
    StmtList body;
};

struct Stmt {
    enum Kind: U8 {
        Block,
        Expression,
        If,
        Forever,
        Break,
        Continue,
        Labelled,
        Return,
        Decl,
        Fun,
        Comment,
    };

    explicit Stmt(Kind kind): kind(kind) {}
    Kind kind;
};

/*
 * `L: { ... }` when labelled - which is how a forward jump is spelled in a language with no `goto`.
 *
 * A `match` compiles to a decision tree whose arms fall through to the next test, so several blocks
 * have two predecessors without being anybody's loop or anybody's `if` join. Each of those becomes
 * a labelled block that everything reaching it leaves through, and the block's own code follows it.
 */
struct BlockStmt: Stmt {
    explicit BlockStmt(StmtList body): Stmt(Block), body(body) {}
    StmtList body;
};

struct ExprStmt: Stmt {
    explicit ExprStmt(JsPtr<Expr> value): Stmt(Expression), value(value) {}
    JsPtr<Expr> value;
};

struct IfStmt: Stmt {
    IfStmt(JsPtr<Expr> cond, StmtList then, StmtList otherwise):
        Stmt(If), cond(cond), then(then), otherwise(otherwise) {}

    JsPtr<Expr> cond;
    StmtList then;
    StmtList otherwise;
};

/*
 * `for(;;) { ... }` - the only loop this tree builds.
 *
 * A back edge in the resolve IR is not a `while` condition, and recovering one would mean pattern
 * matching on the shape of the header block. A labelled infinite loop plus `break`/`continue` says
 * exactly what the CFG says, for every CFG the resolver can produce.
 */
struct ForeverStmt: Stmt {
    explicit ForeverStmt(StmtList body): Stmt(Forever), body(body) {}
    StmtList body;
};

struct BreakStmt: Stmt {
    explicit BreakStmt(Name label): Stmt(Break), label(label) {}
    Name label;
};

struct ContinueStmt: Stmt {
    explicit ContinueStmt(Name label): Stmt(Continue), label(label) {}
    Name label;
};

struct LabelledStmt: Stmt {
    LabelledStmt(Name label, JsPtr<Stmt> content): Stmt(Labelled), content(content), label(label) {}
    JsPtr<Stmt> content;
    Name label;
};

struct ReturnStmt: Stmt {
    explicit ReturnStmt(JsPtr<Expr> value): Stmt(Return), value(value) {}
    JsPtr<Expr> value;
};

/*
 * A binding. `var` rather than `let` for a value, and it is not a style choice.
 *
 * A resolve value is defined once and used wherever its definition dominates - which is not the
 * same as "wherever it is lexically enclosed" once a CFG has been structured into `if` and `for`.
 * A value defined inside a loop body and read after the loop is ordinary SSA and illegal under
 * `let`. `var` is function-scoped, so the emitted binding means what the IR means; the alternative
 * is hoisting every declaration to the top of the function, which says the same thing and reads
 * worse. `const` is used for the module-level constant tables, which have neither problem.
 */
struct DeclStmt: Stmt {
    DeclStmt(Name name, JsPtr<Expr> value, bool constant):
        Stmt(Decl), value(value), name(name), constant(constant) {}

    JsPtr<Expr> value;
    Name name;
    bool constant;
};

struct FunStmt: Stmt {
    explicit FunStmt(Name name): Stmt(Fun), name(name) {}

    JsList<Name, false> args;
    StmtList body;
    Name name;
};

// A `//` line, for the header the file opens with and for naming the module a group of functions
// came from. Dropped when minifying.
struct CommentStmt: Stmt {
    explicit CommentStmt(StringId text): Stmt(Comment), text(text) {}
    StringId text;
};

struct File {
    explicit File(Size maxMemory): arena(maxMemory) {}

    Region<JsRegion> arena;
    StmtList statements;
};

} // namespace js
