#pragma once

#include "ast.h"
#include "Net/Stream.h"
#include "../../compiler/context.h"

// The compiler-owned property keys - `jsir::key`. A *representation* decision, so the table is in
// the tier that records a shape's final key and this directory reads it from there; see the note
// above it for what two spellings of one key cost.
#include "../../resolve_ssa/js/name.h"

/*
 * What every file of the JavaScript backend works against - Analysis-IR-JS.md §8.1.
 *
 * The JSIR pipeline (`resolve_ssa/js` -> `structure.cpp` -> `assemble.cpp`) is the half that reads
 * a program. The rest - `name.cpp`, `wide.cpp`, `bits.cpp`, `helpers.cpp`, `opt.cpp` and
 * `format.cpp` - knows only the syntax tree, the identifiers in it and the helper families the
 * integer tower reaches for. The state those files need is *this* struct, and `Assembly` in `jsir.h`
 * derives from it. It was split out while a second generator, reading `compiler/resolve`, derived
 * from it too; that generator went with its stage in J173, and the split stays for what it says
 * about the files that take only this.
 *
 * What is deliberately not here: anything that knows what a Yana program is. No `mir::Program`, no
 * `TypePtr`, no layout table. A field that needed one of those would be a field the tree-only files
 * could not be written against, and the include list above is what says so.
 */
namespace js {

constexpr U32 kNoBlock = maxLimit<U32>;

/*
 * One block an enclosing construct can be left through, and how.
 *
 * A `Loop` entry is its header, so reaching it is `continue`; a `Forward` entry is a labelled block
 * whose end is that block, so reaching it is `break`. Between them these are every edge JS has no
 * `goto` for. `structure.cpp` pushes them and `Assembly::exits` holds the open ones.
 */
struct Exit {
    U32 block;
    Name label;
    bool loop;
};

/*
 * Which member of a bit-family helper one call wants.
 *
 * The selector the four slot functions index by, in a vocabulary no IR owns: the helper families
 * know only the tree, so the table is keyed on this and `structure.cpp` translates its `JsHelperOp`
 * once, at the one site that asks.
 */
enum class BitFamily: U8 {
    CountBits, LeadingZeros, TrailingZeros,
    Rol, Ror,
    BitsUpTo, GatherBits, ScatterBits,
};

// One 33-to-53-bit helper this file has asked for, interned per (operation, width, signedness) and
// emitted once at the end. See wide.cpp. `WideOp` is in ast.h, since a call node carries one.
struct WideHelper {
    Name name;
    WideOp op;
    U16 bits;
    bool isSigned;
};

// One saturating float-to-`bigint` conversion, per target type - see `saturatingLongHelperAt`.
struct SatHelper {
    Name name;
    U16 bits;
    bool isSigned;
};

/*
 * One accessor for a bit range whose position is only known at run time - see bits.cpp.
 *
 * The static ranges are two or three operations against constants and are emitted where they are
 * used. A range reached *through a reference* is not: the scale is a variable, so the read is a
 * divide, a floor and a mask, and the write is that plus the arithmetic to put the field back. Every
 * borrow of a narrow value in the program spells the same one out, and a program that borrows is a
 * program that does it everywhere.
 */
struct BitHelper {
    Name name;
    U16 bits;
    bool isSigned;
    bool store;

    // Whether the position arrives as a literal pair the body multiplies by, or as the single
    // forward scale a reference carries and the body divides with. See BitPosition.
    bool packed;
};

struct JsShared {
    Context& context;
    File& file;
    JsBase base;

    /*
     * Identifiers already handed out - see name.cpp.
     *
     * Module-level names are checked by every local name as well, so that a local can never shadow a
     * function it might want to call. `interned` is the text this generator has already copied into
     * the string arena, since `Context::addUnqualifiedName` keeps the pointer it is given.
     */
    HashSet<StringId> moduleNames;
    HashSet<StringId> localNames;
    HashSet<StringId> interned;

    /*
     * The module-level `var`s a call can write, by name - the one kind of bare identifier whose read
     * is a read of *storage*.
     *
     * The optimizer's rewrites all rest on one premise, which `propagateCopy` states outright: a
     * local `var` is invisible to every callee, so nothing but this function's own text can change
     * one. A module-level global is exactly the thing that premise is false of, and without this,
     * reading one looks as inert as reading a parameter.
     *
     * Filled by `assemble.cpp` from `Global::isWritten`, because which globals a program assigns is
     * a fact about the program and this is the half of the directory that does not read one.
     */
    HashSet<StringId> mutableGlobals;

    // The 33-to-53-bit helpers this file has asked for, interned per (operation, width, signedness)
    // and emitted once at the end. See wide.cpp.
    HashMap<U32, Name> wideHelpers;
    Array<WideHelper> wideHelperOrder;

    // The dynamic bit-range accessors, on the same terms, interned per (width, signedness,
    // direction). See bits.cpp.
    HashMap<U32, Name> bitHelpers;
    Array<BitHelper> bitHelperOrder;

    // The saturating float-to-`bigint` conversions, interned per (width, signedness). The number
    // case needs no helper; `saturatingToNumber` in structure.cpp is that case.
    HashMap<U32, Name> satHelpers;
    Array<SatHelper> satHelperOrder;

    /*
     * The scratch pair a `Float`/`I32` bitcast goes through - see `floatBitsHelperAt`.
     *
     * A `Float` here is a `number` that `Math.fround` has made exactly representable, so its
     * thirty-two bits are well defined and completely unreachable: this target has no operator that
     * sees them. Two views over one buffer is the only way, and one buffer per program is enough
     * because the write and the read that follows it are the whole of the operation.
     *
     * Named lazily, so a program that never reinterprets a float emits neither declaration.
     */
    Name floatBitsBuffer;
    Name floatBitsInts;
    Name floatToBitsHelper;
    Name bitsToFloatHelper;

    /*
     * The same pair at 64 bits - a `Float64Array` and a `BigInt64Array` over its buffer.
     *
     * Two pairs and not one because the *element type* is what does the reinterpretation, and the
     * integer side of this one hands back a `bigint` where the 32-bit side hands back a `number`.
     */
    Name doubleBitsBuffer;
    Name doubleBitsInts;
    Name doubleToBitsHelper;
    Name bitsToDoubleHelper;

    // `round`, which is the one of the four roundings the host does not already have - `Math.round`
    // breaks ties toward positive infinity and the language's `round` breaks them away from zero.
    Name roundAwayHelper;

    // `$swap16`, `$swap32` and `$swap64` - the byte reversal, which this host has no operator for at
    // any width. Indexed by width: 16, 32 and 64 are slots 0, 1 and 2.
    Name byteSwapHelpers[3];

    // `$popcount32` / `$ctz32` and their three 64-bit partners. Indexed by `bitCountHelperSlot`:
    // the operation, then the width. There is no `$clz32`, `Math.clz32` being the host's own.
    Name bitCountHelpers[6];

    // `$rol32` and its nine partners - the two rotations, at each of the five widths a scalar
    // `Integral` instance is generated for. Indexed by `rotateHelperSlot`.
    Name rotateHelpers[10];

    // `$bzhi32` and its five partners - `Core.Bits.bitsUpTo` and the two directions of
    // `Core.BitPermute`. Indexed by `bitOpHelperSlot`: the operation, then the width.
    Name bitOpHelpers[6];

    /*
     * `$div` and `$rem` - the two divisors the language answers for, where the host does not.
     *
     * `x / 0` is 0 and `x % 0` is `x` (doc/spec/types.md), and JavaScript agrees with exactly one
     * half of one of them. One pair of helpers serves all three integer bands, which is what
     * `b ? a / b : b` buys: the zero arm returns the *divisor*, so the answer carries the operand's
     * own type and neither `0` nor `0n` has to be written.
     */
    Name divideByZeroHelper;
    Name remainderByZeroHelper;

    // The one helper the typed row of Implementation-Containers.md §14 needs - see
    // NativeOp::HostGrow. Written over `a.constructor`, so one function serves every element type.
    Name growHelper;

    // `$bitCell(o, k, s, m, i)` and the prototype its boxes share - see `emitBitCellHelper`.
    Name bitCellHelper;
    Name bitCellProto;

    /*
     * `$fd` - a function value's release, and the whole of what a collected target owes one.
     *
     * One function for the program rather than one per function type, which is the native stage's
     * own decision (`lower/fun.cpp`'s `funTeardown`): the pair is two words whatever it was made
     * from, so a drop of any `Fun` is one call and every `Fun` shares the callee. What it asks is
     * the same two questions - whether this *value* holds an environment (the second word), and
     * whether *that lambda* owes anything (the header hung on the code word) - in the same order,
     * and for the same reason: only a lambda with an environment has a header, so the null test is
     * what keeps the read off a plain function's thunk.
     *
     * Named lazily, so a program with nothing to release emits neither the helper nor a call.
     */
    Name funDropHelper;

    // The word transfers, and the `DataView` they ride on - see NativeOp::HostWordRead. The slot is
    // the width: 0 is 16 bits, 1 is 32 and 2 is 64.
    Name viewHelper;
    Name readWordHelper[3];
    Name writeWordHelper[3];

    // The heading each family of helpers is emitted under, so that a family the peephole emptied
    // takes its own comment with it - see removeDeadHelpers.
    JsPtr<Stmt> wideHelperComment = nullptr;
    JsPtr<Stmt> bitHelperComment = nullptr;

    /*
     * The property names that are the compiler's rather than the program's.
     *
     * The names are the target's convention - `jsir::key` is the table - rather than one file's
     * decision, and two spellings of one key are two hidden classes for one record.
     */
    Name tagField;
    Name payloadField;
    Name boxField;
    Name codeField;
    Name envField;
    Name headerField;

    // Where statements being built go, and the counter the generated labels come from. Per function;
    // `collect` is what swaps the first.
    StmtList* body = nullptr;
    U32 labelCounter = 0;
};

// The compiler's own property names, interned once per file, immediately after the state is
// constructed, so that `$tag` is one StringId however the tree was built.
void nameSharedFields(JsShared& g);

/*
 * Allocation.
 */

template<class T, class... A>
T* make(JsShared& g, A&&... args) {
    return new (g.file.arena) T(forward<A>(args)...);
}

template<class T>
JsPtr<Expr> asExpr(JsShared& g, T* value) {
    return (Expr*)value - g.base;
}

template<class T>
JsPtr<Stmt> asStmt(JsShared& g, T* value) {
    return (Stmt*)value - g.base;
}

template<class T>
void emit(JsShared& g, T* stmt) {
    g.body->push(g.file.arena, asStmt(g, stmt));
}

// Builds one statement list, with everything emitted while `f` runs going into it.
template<class F>
StmtList collect(JsShared& g, F&& f) {
    StmtList list;
    auto previous = g.body;
    g.body = &list;
    f();
    g.body = previous;
    return list;
}

/*
 * Names. The rest of naming is in name.cpp; these two are here because every builder below reaches
 * for them.
 */

// A name this generator spelled out itself rather than one derived from the program - `$tag`,
// `Math`, `BigInt`. No disambiguation, because nothing else may claim these.
Name literalName(JsShared& g, StringView text);

// A property name. These are not identifiers in a scope, so they need no disambiguation - two
// records may both have an `x`, and a reserved word is a legal property name.
Name propertyName(JsShared& g, StringView text);

/*
 * Expression builders.
 */

inline JsPtr<Expr> variable(JsShared& g, Name name) {
    return asExpr(g, make<VarExpr>(g, name));
}

inline JsPtr<Expr> field(JsShared& g, JsPtr<Expr> object, Name name) {
    return asExpr(g, make<FieldExpr>(g, object, name));
}

inline JsPtr<Expr> number(JsShared& g, F64 value, bool integral = true) {
    return asExpr(g, make<NumberExpr>(g, value, integral));
}

// `2**bits` as an exact double, by doubling rather than by `pow`: every power of two up to 2^53 is
// representable, and this is a compile-time constant in the emitted text either way.
inline F64 powerOfTwo(U32 bits) {
    F64 result = 1;
    for(U32 i = 0; i < bits; i++) result *= 2;
    return result;
}

inline JsPtr<Expr> bigInt(JsShared& g, U64 value, bool isSigned) {
    return asExpr(g, make<BigIntExpr>(g, value, isSigned));
}

/*
 * A `bigint` literal's value, or false where this is not one.
 *
 * The `bigint` half of `constantNumber` below, and it is a separate question rather than a widening
 * of that one: the two domains do not mix in this language's output any more than they do in the
 * host's, so a fold that took either would be a fold that could write `0n === 0`.
 *
 * `value` is the raw sixty-four bits and `isSigned` says which number they spell, so a caller
 * comparing two of them has to agree about that first - see `foldBigIntComparison`.
 */
inline bool constantBigInt(JsShared& g, JsPtr<Expr> pointer, U64& into, bool& isSigned) {
    auto expr = g.base[pointer];
    if(expr->kind != Expr::BigInt) return false;

    into = ((BigIntExpr*)expr)->value;
    isSigned = ((BigIntExpr*)expr)->isSigned;
    return true;
}

inline JsPtr<Expr> boolean(JsShared& g, bool value) {
    return asExpr(g, make<BoolExpr>(g, value));
}

inline JsPtr<Expr> nullValue(JsShared& g) {
    return asExpr(g, make<NullExpr>(g));
}

inline JsPtr<Expr> elementAt(JsShared& g, JsPtr<Expr> array, JsPtr<Expr> index) {
    return asExpr(g, make<IndexExpr>(g, array, index));
}

inline JsPtr<Expr> index(JsShared& g, JsPtr<Expr> array, U32 slot) {
    return elementAt(g, array, number(g, F64(slot)));
}

/*
 * Evaluating the host's operators, and the one rule about when an answer may be written down.
 *
 * Every bit range this target reaches is built out of `&`, `|`, `<<` and `~` against numbers the
 * emitter worked out - a field's mask, its offset, the hole its neighbours must keep - and where the
 * value being written is a literal too, the whole read-modify-write is arithmetic over constants.
 * `Boxed {k: 7, a: 1, b: 4095}` emitted `~(4095 << 20)` and `(4095 & 4095) << 20`, and opt.cpp could
 * not take it back: that pass reasons about *ranges*, so it can drop a mask that provably does
 * nothing but cannot evaluate one that does something.
 *
 * These are stated here rather than at either use site because there are three of them - the emitter
 * below, the peephole's `foldConstantOp`, and the helper evaluator that reads a `$p20u$set` body and
 * works out what the call comes to. Three implementations of what `>>>` means is three chances to
 * disagree with the host about one of them.
 *
 * ## Two rules keep this exact
 *
 * **An operand is a *finite* number literal that is not negative zero**, tested on the bit pattern
 * rather than by comparison - see `constantNumber`. `-0` compares equal to `0`, so `x * -0` is the
 * one product whose sign cannot be recovered from its value, and the cheapest answer is for the sign
 * never to enter.
 *
 * **An answer is only ever written down as an exact integer** - see `numberLiteral`. The arithmetic
 * itself runs in `double`, because `Math.floor(w * 2**-32)` is what a bit range above the operators
 * is *made of* and there is no integer form of it; but a fraction is never emitted, so nothing here
 * depends on the printer round-tripping one, and a `-0` result is not representable in what comes
 * out. Intermediates stay in `F64` where they belong.
 *
 * The compiler is built at -O3 rather than -Ofast for exactly this reason - see the note in the root
 * CMakeLists.txt. Each operation below is one correctly-rounded IEEE 754 binary64 operation on both
 * sides, which is what makes evaluating it here the same as evaluating it there.
 */

// 2^53: past it a double stops counting by ones, so an integer answer beyond this is one the host
// would not have been able to state either.
constexpr F64 kExactIntegerLimit = 9007199254740992.0;

inline bool isNegativeZero(F64 value) {
    union { F64 f; U64 bits; } punned = { value };
    return punned.bits == (U64(1) << 63);
}

/*
 * Whether two number literals are the *same* literal - by bits, because `==` says `0` and `-0` are
 * one number and they are two answers (`1 / x`). Every "these two arms give the same literal" test
 * on this target asks this rather than `==`: `structure.cpp`'s map spellings and `opt.cpp`'s
 * `foldAssignIf`, which once folded `x ? -0 : 0` to `0` (J91). NaN against NaN of another payload
 * says no, which only ever keeps two arms apart.
 */
inline bool sameNumberBits(F64 x, F64 y) {
    union { F64 f; U64 bits; } a = { x }, b = { y };
    return a.bits == b.bits;
}

inline bool isFiniteNumber(F64 value) {
    union { F64 f; U64 bits; } punned = { value };
    return (punned.bits & (U64(0x7ff) << 52)) != (U64(0x7ff) << 52);
}

// Whether a value is an integer the host counts by ones at, which is the only shape an answer may
// be written back as.
inline bool isExactInteger(F64 value) {
    return value >= -kExactIntegerLimit && value <= kExactIntegerLimit && F64(I64(value)) == value;
}

inline bool constantNumber(JsShared& g, JsPtr<Expr> pointer, F64& into) {
    auto expr = g.base[pointer];
    if(expr->kind != Expr::Number) return false;

    auto value = ((NumberExpr*)expr)->value;
    if(!isFiniteNumber(value) || isNegativeZero(value)) return false;

    into = value;
    return true;
}

// The host's ToInt32 - truncate towards zero, then keep the low thirty-two bits. Only defined here
// for a value inside the exactly-representable range, which every operand the emitter writes is;
// the operators below decline anything else rather than guessing at the modulo.
inline I32 toInt32(F64 value) {
    return I32(U32(U64(I64(value))));
}

inline bool fitsInt32Conversion(F64 value) {
    return value >= -kExactIntegerLimit && value <= kExactIntegerLimit;
}

// `Math.floor`, without libm: the truncation is exact inside the range above, and a negative value
// that lost a fraction on the way rounds one further down.
inline F64 floorOf(F64 value) {
    auto truncated = F64(I64(value));
    return (value < 0 && truncated != value) ? truncated - 1 : truncated;
}

/*
 * One host binary operator over two known values. False where the answer is not one this may state -
 * a `%` or `/` by zero, a bitwise operand too large to say what ToInt32 makes of it.
 *
 * The comparisons are not here: they answer a boolean rather than a number, and only the helper
 * evaluator has anywhere to put one.
 */
inline bool applyJsBinary(BinaryOp op, F64 a, F64 b, F64& into) {
    switch(op) {
        case BinaryOp::Add: into = a + b; break;
        case BinaryOp::Sub: into = a - b; break;
        case BinaryOp::Mul: into = a * b; break;

        case BinaryOp::Div:
            if(b == 0) return false;
            into = a / b;
            break;

        // The host's `%` is a remainder with the dividend's sign, which for two exact integers is
        // the integer remainder. Restricted to that case rather than reaching for `fmod`, since the
        // helpers that use it - `$w40u$wrap` and its family - reduce an integer by a power of two.
        case BinaryOp::Rem:
            if(b == 0 || !isExactInteger(a) || !isExactInteger(b)) return false;
            into = F64(I64(a) % I64(b));
            break;

        default: {
            if(!fitsInt32Conversion(a) || !fitsInt32Conversion(b)) return false;

            auto x = toInt32(a);
            auto y = toInt32(b);

            // A shift count is masked to five bits by the host, so this masks it too rather than
            // declining: `1 << 32` is `1` there, and any other answer would be a difference between
            // the compiled and the emitted form rather than a missed opportunity.
            auto by = U32(y) & 31;

            switch(op) {
                case BinaryOp::And: into = F64(x & y); break;
                case BinaryOp::Or:  into = F64(x | y); break;
                case BinaryOp::Xor: into = F64(x ^ y); break;
                case BinaryOp::Shl: into = F64(I32(U32(x) << by)); break;
                case BinaryOp::Sar: into = F64(x >> by); break;
                case BinaryOp::Shr: into = F64(U32(x) >> by); break;
                default: return false;
            }
        }
    }

    return isFiniteNumber(into);
}

inline bool applyJsUnary(UnaryOp op, F64 value, F64& into) {
    switch(op) {
        case UnaryOp::Neg:
            into = -value;
            break;
        case UnaryOp::BitNot:
            if(!fitsInt32Conversion(value)) return false;
            into = F64(~toInt32(value));
            break;
        default:
            return false;
    }

    return isFiniteNumber(into);
}

// An answer as a literal, or null where it is not one this may write down - see the rule above.
inline JsPtr<Expr> numberLiteral(JsShared& g, F64 value) {
    if(!isExactInteger(value) || isNegativeZero(value)) return nullptr;
    return number(g, value);
}

inline JsPtr<Expr> foldBinaryOp(JsShared& g, BinaryOp op, JsPtr<Expr> lhs, JsPtr<Expr> rhs) {
    F64 left, right, result;
    if(!constantNumber(g, lhs, left) || !constantNumber(g, rhs, right)) return nullptr;
    if(!applyJsBinary(op, left, right, result)) return nullptr;

    return numberLiteral(g, result);
}

inline JsPtr<Expr> foldUnaryOp(JsShared& g, UnaryOp op, JsPtr<Expr> value) {
    F64 operand, result;
    if(!constantNumber(g, value, operand)) return nullptr;
    if(!applyJsUnary(op, operand, result)) return nullptr;

    return numberLiteral(g, result);
}

/*
 * The comparisons, which answer a boolean and so are not `applyJsBinary`'s.
 *
 * `===` and `!==` on two numbers are value equality, so they fold like the ordering ones. The loose
 * pair is deliberately absent: this tree only builds one against a *reference* (see BinaryOp), where
 * what it tests is whether a property was ever attached, and neither side is a number.
 */
inline JsPtr<Expr> foldComparison(JsShared& g, BinaryOp op, JsPtr<Expr> lhs, JsPtr<Expr> rhs) {
    F64 a, b;
    if(!constantNumber(g, lhs, a) || !constantNumber(g, rhs, b)) return nullptr;

    switch(op) {
        case BinaryOp::Lt: return boolean(g, a <  b);
        case BinaryOp::Le: return boolean(g, a <= b);
        case BinaryOp::Gt: return boolean(g, a >  b);
        case BinaryOp::Ge: return boolean(g, a >= b);
        case BinaryOp::Eq: return boolean(g, a == b);
        case BinaryOp::Ne: return boolean(g, a != b);
        default: return nullptr;
    }
}

/*
 * The same over two `bigint` literals, which is the domain the 64-bit types live in here.
 *
 * Only the comparisons, and deliberately: an *arithmetic* fold would have to wrap at the operand's
 * declared width, which is a property of the type rather than of the literal and is not in hand at
 * this call. A comparison answers a `Bool` and needs nothing but the two values.
 *
 * The literals must agree about signedness before their bits are read as numbers - the same
 * sixty-four bits are two different values otherwise, and every pair the emitter actually builds
 * agrees, because both sides of a comparison have been coerced to one operand type.
 */
/*
 * `<literal> === null` and its negation, which is what a **niche-folded** tag test comes to once the
 * value it is asked about turns out to be a constructor written out in full.
 *
 * `Just(4)` is the number four on this target, so `match Just(4)` reads as `4 === null` - a test
 * whose answer is in the node and which nothing else here decides, because `constantNumber` declines
 * `null` and `null` declines every numeric rule. Above this tier the constructor is still an
 * aggregate; here the encoding is what makes it a comparison, so the fold belongs beside the other
 * comparisons rather than in the resolver.
 *
 * Only the **inert** literals, which is what lets the operand simply go: a number, a bigint, a
 * string, a boolean and `null` itself. An object or an array literal is not `null` either and is an
 * allocation whose identity the program may have asked for, so neither is folded here.
 */
inline JsPtr<Expr> foldNullComparison(JsShared& g, BinaryOp op, JsPtr<Expr> lhs, JsPtr<Expr> rhs) {
    if(op != BinaryOp::Eq && op != BinaryOp::Ne) return nullptr;

    auto known = [&](JsPtr<Expr> side, bool& isNull) {
        switch(g.base[side]->kind) {
            case Expr::Null:   isNull = true;  return true;
            case Expr::Number:
            case Expr::BigInt:
            case Expr::String:
            case Expr::Bool:   isNull = false; return true;
            default:           return false;
        }
    };

    auto leftNull = false;
    auto rightNull = false;
    if(!known(lhs, leftNull) || !known(rhs, rightNull)) return nullptr;
    if(!leftNull && !rightNull) return nullptr;

    auto same = leftNull == rightNull;
    return boolean(g, op == BinaryOp::Eq ? same : !same);
}

inline JsPtr<Expr> foldBigIntComparison(JsShared& g, BinaryOp op, JsPtr<Expr> lhs, JsPtr<Expr> rhs) {
    U64 a, b;
    bool leftSigned, rightSigned;

    if(!constantBigInt(g, lhs, a, leftSigned) || !constantBigInt(g, rhs, b, rightSigned)) return nullptr;
    if(leftSigned != rightSigned) return nullptr;

    switch(op) {
        case BinaryOp::Lt: return boolean(g, leftSigned ? I64(a) <  I64(b) : a <  b);
        case BinaryOp::Le: return boolean(g, leftSigned ? I64(a) <= I64(b) : a <= b);
        case BinaryOp::Gt: return boolean(g, leftSigned ? I64(a) >  I64(b) : a >  b);
        case BinaryOp::Ge: return boolean(g, leftSigned ? I64(a) >= I64(b) : a >= b);
        case BinaryOp::Eq: return boolean(g, a == b);
        case BinaryOp::Ne: return boolean(g, a != b);
        default: return nullptr;
    }
}

inline JsPtr<Expr> binary(JsShared& g, BinaryOp op, JsPtr<Expr> lhs, JsPtr<Expr> rhs) {
    if(auto folded = foldBinaryOp(g, op, lhs, rhs)) return folded;
    if(auto folded = foldComparison(g, op, lhs, rhs)) return folded;
    if(auto folded = foldBigIntComparison(g, op, lhs, rhs)) return folded;

    return asExpr(g, make<BinaryExpr>(g, op, lhs, rhs));
}

inline JsPtr<Expr> unary(JsShared& g, UnaryOp op, JsPtr<Expr> value) {
    if(auto folded = foldUnaryOp(g, op, value)) return folded;
    return asExpr(g, make<UnaryExpr>(g, op, value));
}

inline JsPtr<Expr> ternary(JsShared& g, JsPtr<Expr> cond, JsPtr<Expr> then, JsPtr<Expr> otherwise) {
    return asExpr(g, make<TernaryExpr>(g, cond, then, otherwise));
}

inline JsPtr<Expr> assign(JsShared& g, JsPtr<Expr> target, JsPtr<Expr> value) {
    return asExpr(g, make<AssignExpr>(g, target, value));
}

// The call an emitter builds when it knows its arguments: `call(g, f, a, b)`.
template<class... A>
JsPtr<Expr> call(JsShared& g, JsPtr<Expr> callee, A... args) {
    auto node = make<CallExpr>(g, callee);
    (node->args.push(g.file.arena, args), ...);
    return asExpr(g, node);
}

// A call this generator built out of a host intrinsic rather than out of the program - see
// CallExpr::pure, which is what the two builders below exist to set.
inline JsPtr<Expr> asPureCall(JsShared& g, JsPtr<Expr> value) {
    ((CallExpr*)g.base[value])->pure = true;
    return value;
}

/*
 * A property read that is a host container's `.length` - see `Expr::valueBits`.
 *
 * The one range that is the *host's* specification rather than a Yana type's, which is why it is
 * said here rather than left to `noteValueType`: `hostLength` answers a `Size`, so the type would
 * claim a signed 32-bit number, and what the host guarantees is an unsigned one.
 */
inline JsPtr<Expr> asHostLength(JsShared& g, JsPtr<Expr> value) {
    auto expr = g.base[value];
    expr->valueBits = 32;
    expr->valueSigned = false;
    return value;
}

// `Namespace.member(...)` for the handful of host intrinsics the integer tower needs - `Math.imul`,
// `BigInt.asIntN`. Nothing else in the emitted code reaches for the host.
template<class... A>
JsPtr<Expr> hostCall(JsShared& g, StringView object, StringView member, A... args) {
    return asPureCall(g, call(g, field(g, variable(g, literalName(g, object)),
                                       literalName(g, member)), args...));
}

// `Name(...)` - `BigInt(x)`, `Number(x)`. These are conversions rather than members of anything.
template<class... A>
JsPtr<Expr> globalCall(JsShared& g, StringView name, A... args) {
    return asPureCall(g, call(g, variable(g, literalName(g, name)), args...));
}

/*
 * Statement builders.
 */

inline void emitExpr(JsShared& g, JsPtr<Expr> value) {
    emit(g, make<ExprStmt>(g, value));
}

// `var name = value;`, and the name is what everything downstream uses for the value.
inline JsPtr<Expr> declare(JsShared& g, Name name, JsPtr<Expr> value) {
    emit(g, make<DeclStmt>(g, name, value, false));
    return variable(g, name);
}

/*
 * name.cpp - identifiers.
 */

// Copies text the generator built into the string arena before interning it, since
// Context::addUnqualifiedName keeps the pointer it is given rather than copying.
StringId internText(JsShared& g, StringView text);

// A module-level or local identifier, disambiguated against every name already handed out.
Name uniqueName(JsShared& g, StringView text, bool local);

/*
 * `$div(a, b)` and `$rem(a, b)`, naming the helper on its first use - see
 * `JsShared::divideByZeroHelper` for what the two are and why one pair serves all three bands.
 *
 * Here rather than in `structure.cpp` because it knows only the syntax tree - a call to a helper
 * named on demand - which is what this header is for. It came here in J80, when a second generator
 * wrote the same six lines.
 *
 * Pure, and that is not a formality: the whole point of the ruling these implement is that an
 * integer division computes a value and does nothing else, so the emitter is free to move one.
 */
inline JsPtr<Expr> divisionHelperCall(JsShared& g, Name& helper, StringView helperName,
                                      JsPtr<Expr> lhs, JsPtr<Expr> rhs) {
    if(!helper.text) helper = uniqueName(g, helperName, false);

    auto node = make<CallExpr>(g, variable(g, helper));
    node->args.push(g.file.arena, lhs);
    node->args.push(g.file.arena, rhs);
    node->pure = true;
    return asExpr(g, node);
}

inline JsPtr<Expr> divideCall(JsShared& g, JsPtr<Expr> lhs, JsPtr<Expr> rhs) {
    return divisionHelperCall(g, g.divideByZeroHelper, "$div"_v, lhs, rhs);
}

inline JsPtr<Expr> remainderCall(JsShared& g, JsPtr<Expr> lhs, JsPtr<Expr> rhs) {
    return divisionHelperCall(g, g.remainderByZeroHelper, "$rem"_v, lhs, rhs);
}

// `v7` for a value the source never named, so that the emitted code and the resolve dump agree on
// what to call it.
Name generatedName(JsShared& g, StringView prefix, U32 index);



/*
 * wide.cpp - integers of 33 to 53 bits.
 */

// The name of one helper, emitting it on first use. Callers normally want `wideCall`.
Name wideHelper(JsShared& g, WideOp op, U32 bits, bool isSigned);

// `$w53i$and(a, b)`. `b` is null for the one-operand operations.



// The same, for a caller that has a width rather than a type - a bit range of a packed word is one
// of these without being a value of any 33-to-53-bit type.
JsPtr<Expr> wideCallAt(JsShared& g, WideOp op, U32 bits, bool isSigned, JsPtr<Expr> a, JsPtr<Expr> b);

// `v >= 2**(bits-1) ? v - 2**bits : v` - an unsigned bit pattern read back as a signed value.
JsPtr<Expr> resignExpr(JsShared& g, JsPtr<Expr> value, U32 bits);

/*
 * A bit range of a packed word, taken apart and put back - `bits.cpp`, on `JsShared` so that one
 * layout has one spelling.
 *
 * Where a range sits, as the two numbers the wide form multiplies by. `down` brings the bits at and
 * above the range to bit zero and `up` puts a field back, and the two are reciprocals - so a
 * position could be one number and is two because of *which* number each end has. A constant offset
 * knows both and multiplies twice, which is what a `Math.floor` wants in front of it. A reference
 * carries the forward scale alone, and deriving its reciprocal would be a divide per access either
 * way, so that end divides instead. `divides` is that difference and it is the only thing separating
 * the two families of helper `bitHelper` interns.
 */
struct BitPosition {
    JsPtr<Expr> down = nullptr;   // multiply the word by this; null means one
    JsPtr<Expr> up = nullptr;     // multiply the field by this; null means one
    bool divides = false;         // `word / up` rather than `word * down`
};

// `scale` is a run-time multiplier of `2**offset` a reference carried, or null for a constant range.
BitPosition bitPosition(JsShared& g, U32 offset, JsPtr<Expr> scale);

// `wide` is the arithmetic form, for a word wider than the host's 32-bit operators; `word` is how
// wide that word can be, which a store needs and a load does not. See bits.cpp.
JsPtr<Expr> decodeRange(JsShared& g, JsPtr<Expr> owner, U32 offset, U32 width, BitPosition position,
                        bool wide, bool isSigned);
JsPtr<Expr> encodeRange(JsShared& g, JsPtr<Expr> owner, U32 offset, U32 width, U32 word,
                        BitPosition position, bool wide, JsPtr<Expr> value);

/*
 * A range a reference's **scale** positions - JSIR's `BitRange` with a `bitScale` (J84). Always the
 * borrowed `$bNx$get/set` helper, since nothing at the site knows the word's width. `whole` is the
 * reference's own content, for which a scale of zero means "the storage holds the value" - see
 * `JsPlace::bitWhole` - and costs a test at the site rather than a second helper family.
 */
JsPtr<Expr> decodeScaledRange(JsShared& g, JsPtr<Expr> owner, JsPtr<Expr> scale, U32 offset,
                              U32 width, bool isSigned, bool whole);
JsPtr<Expr> encodeScaledRange(JsShared& g, JsPtr<Expr> owner, JsPtr<Expr> scale, U32 offset,
                              U32 width, bool isSigned, bool whole, JsPtr<Expr> value);

// `$bitCell(owner, key, scale, 2**width, signed)` - a range reference as one `{$o, $k}` value.
JsPtr<Expr> bitCellCall(JsShared& g, JsPtr<Expr> owner, JsPtr<Expr> key, JsPtr<Expr> scale,
                        U32 width, bool isSigned);
void emitBitCellHelper(JsShared& g);

// A constant range named by numbers - JSIR's `BitRange` place. `word` is the word's width in bits.
JsPtr<Expr> decodeBitRange(JsShared& g, JsPtr<Expr> owner, U32 offset, U32 width, U32 word,
                           bool isSigned);
JsPtr<Expr> encodeBitRange(JsShared& g, JsPtr<Expr> owner, U32 offset, U32 width, U32 word,
                           bool isSigned, JsPtr<Expr> value);

// A word built from its fields, one `(value, offset, width)` per field - JSIR's `Pack`.
JsPtr<Expr> packBitRanges(JsShared& g, Buffer<const JsPtr<Expr>> values, Buffer<const U32> offsets,
                          Buffer<const U32> widths, U32 word);

// The interned accessor for one (width, signedness, direction, family), declared on first ask.
Name bitHelper(JsShared& g, U32 bits, bool isSigned, bool store, bool packed);

// Every accessor `bitHelper` was asked for, as function declarations over a word of `wordBits`.
// Emitted *before* the wide helpers, since a body here may ask for one of those and the list it
// would be appended to is already being walked.
void emitBitHelpers(JsShared& g, U32 wordBits);

// A value crossing between a wide type and a narrower integer, in the widening direction.

// A comparison's host boolean, as the number a `Bool` is on this target - see `boolNumber` in
// helpers.cpp. Only the two instructions that produce a `Bool` from a test need it; everything else
// already holds the number.
JsPtr<Expr> boolNumber(JsShared& g, JsPtr<Expr> test);

// The saturating float-to-`bigint` conversion for one width, emitted once. Neither of the number
// case's two shortcuts survives at sixty-four bits: `2^63 - 1` is not a double, so a clamp in the
// float domain answers `2^63 - 1024` past the end, and `BigInt(NaN)` throws rather than producing
// anything to coerce. See `emitSaturationHelpers`.
Name saturatingLongHelperAt(JsShared& g, U16 bits, bool isSigned);

/*
 * helpers.cpp - the helper families, named on first ask and emitted once at the end.
 *
 * Each answers the *name* and reserves whatever partners the body of that helper calls; the caller
 * builds the call node and applies its own coercion, which is what lets one function serve both
 * signednesses of a width.
 */

// The two directions of a float reinterpretation, and the typed-array pair behind them.
Name floatBitsHelperAt(JsShared& g, bool toBits, bool wide);

// `$swap16`, `$swap32`, `$swap64`.
Name byteSwapHelperAt(JsShared& g, U32 bits);

// The three counts. `kNoHelperName` where the host answers directly, which is `LeadingZeros` at
// thirty-two bits and `Math.clz32`.
Name bitCountHelperAt(JsShared& g, BitFamily of, bool wide);

// The two rotations, at the five widths a scalar `Integral` instance is generated for. An empty
// name where the width has no slot, which the caller reports.
Name rotateHelperAt(JsShared& g, BitFamily of, U32 bits);

// The three `Core.Bits`/`Core.BitPermute` operations, at the two widths they are declared over.
Name bitOpHelperAt(JsShared& g, BitFamily of, bool wide);

// `$fd(p)` - a function value's release, which is the pair's two questions. See `emitFunDropHelper`.
Name funDropHelperAt(JsShared& g);
void emitFunDropHelper(JsShared& g);

/*
 * The range a float-to-integer conversion saturates into - Design-Vector §3.4's ruling, which is a
 * scalar one: the native backends state the same two numbers in `lower_inst.h`'s conversions.
 *
 * **A value outside the target's range clamps to the nearest end of it, and NaN becomes zero.** The
 * alternative was three answers from one source: `cvttsd2si` produces the integer indefinite value
 * on x86, ARM saturates already, and JS wraps modulo 2^32 through `|0`. Saturation is where WASM,
 * Rust and Swift converged, and it costs a compare and a select.
 *
 * The bounds are the type's own, as `F64`. Every one of them is exact except a 64-bit maximum -
 * `2^63 - 1` and `2^64 - 1` are not doubles - so a clamp in the *float* domain must use a strict
 * comparison against the returned high bound and produce the integer maximum itself, rather than
 * converting the clamped float. `highIsExact` says which case it is in.
 */
struct SaturationRange {
    F64 low;
    F64 high;
    bool highIsExact;
};

inline SaturationRange saturationRange(U32 held, bool isSigned) {
    auto bits = held >= 64 ? 64u : held;

    if(!isSigned) {
        auto high = bits >= 64 ? 18446744073709551616.0 : F64((U64(1) << bits) - 1);
        return SaturationRange { 0.0, high, bits <= 53 };
    }

    auto magnitude = F64(U64(1) << (bits - 1));
    return SaturationRange { -magnitude, bits >= 64 ? magnitude : magnitude - 1.0, bits <= 54 };
}

/*
 * The widest integer a host `number` holds every value of - above it consecutive integers stop being
 * representable, so an integer type wider than this is a `bigint` here.
 */
constexpr U16 kMaxNumberBits = 53;

/*
 * helpers.cpp and wide.cpp - the runtime helpers a file declares once, emitted after its functions.
 */

// A tree of wide bitwise calls as one unpack-operate-pack, or null where this one is not that.
// Asked at the root of a tree and never below it - see opt.cpp, which is the only caller.
JsPtr<Expr> fuseWideBitwise(JsShared& g, JsPtr<Expr> pointer);

// Every helper asked for, as function declarations. Called once, by `assemble.cpp`.
void emitWideHelpers(JsShared& g);

// The saturating float-to-`bigint` conversions a program asked for. See `saturatingLongHelperAt`.
void emitSaturationHelpers(JsShared& g);

// The typed-array pair and the two functions a `Float`/`I32` bitcast goes through, where a program
// asked for one. See `floatBitsHelperAt`.
void emitFloatBitsHelpers(JsShared& g);

// `function $round(v) { return v < 0 ? -Math.round(-v) : Math.round(v) }` - see JsShared::roundAwayHelper.
void emitRoundAwayHelper(JsShared& g);

// The byte reversals a program asked for, one function per width. See JsShared::byteSwapHelpers.
void emitByteSwapHelpers(JsShared& g);

// The bit counts a program asked for, one function per (operation, width). See
// JsShared::bitCountHelpers.
void emitBitCountHelpers(JsShared& g);

// The rotations a program asked for, one function per (direction, width). See JsShared::rotateHelpers.
void emitRotateHelpers(JsShared& g);

// The three BMI2 operations a program asked for, one function per (operation, width). See
// JsShared::bitOpHelpers.
void emitBitOpHelpers(JsShared& g);

/*
 * Which of the ten slots one (direction, width) is, or `kNoRotateHelper` for a width no instance is
 * generated at.
 *
 * The five widths are the ones `defineIntegerTypes` creates plus the two canonical ones, which is
 * where the list comes from rather than from anything about JavaScript: a `@bits` refinement
 * dispatches to the instances of the type it refines, so a rotation of a `@bits(40) U64` arrives
 * here as a 64-bit one and no sixth width exists.
 */
static const Size kNoRotateHelper = ~Size(0);

inline Size rotateHelperSlot(BitFamily of, U32 bits) {
    auto width = bits == 8 ? 0 : bits == 16 ? 1 : bits == 32 ? 2 : bits == 53 ? 3 : bits == 64 ? 4 : -1;
    if(width < 0) return kNoRotateHelper;

    return Size((of == BitFamily::Rol ? 0 : 5) + width);
}

// Which of the six slots one (operation, width) is. The operation, then the width - so the two
// widths of a count sit beside each other and the unused slot (a 32-bit leading count, which is
// `Math.clz32` and no helper) is simply never filled.
inline Size bitCountHelperSlot(BitFamily of, bool wide) {
    auto op = of == BitFamily::CountBits ? 0 : of == BitFamily::LeadingZeros ? 1 : 2;
    return Size(op * 2 + (wide ? 1 : 0));
}

// Which of the six slots one (operation, width) is - the same shape `bitCountHelperSlot` has, and
// the widths are the two `Core.Bits` is declared over.
inline Size bitOpHelperSlot(BitFamily of, bool wide) {
    auto op = of == BitFamily::BitsUpTo ? 0 : of == BitFamily::GatherBits ? 1 : 2;
    return Size(op * 2 + (wide ? 1 : 0));
}

// `function $div(a, b) { return b ? a / b : b }` and `$rem`, where a program asked for one. See
// JsShared::divideByZeroHelper.
void emitDivisionHelpers(JsShared& g);

// The typed array's growth, where a program asked for one. See JsShared::growHelper.
void emitGrowHelper(JsShared& g);

// The `DataView` cache and the word transfers a program asked for. See JsShared::viewHelper for
// what the cache is and what it was measured against.
void emitWordHelpers(JsShared& g);

/*
 * opt.cpp - the peephole between the tree and the text.
 */

// Removes the bindings nothing needed and folds the writes that build a value into the literal that
// builds it. Runs over the finished file, because how many readers a binding has is not known until
// the function it is in has been emitted.
void optimizeFile(JsShared& g);

// Calls to empty functions removed, and functions nothing reaches - see opt.cpp.
void sweepFunctions(JsShared& g);

// The tree above as text - `format.cpp`.
void formatFile(Net::Writer& writer, Context& context, File& file, bool minify);

} // namespace js
