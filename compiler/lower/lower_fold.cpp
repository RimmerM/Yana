#include "lower_fold.h"
#include "lower_builder.h"

namespace {

// How many bits of a value of this type are the value. Only asked of an integer type; a pointer is
// never folded, because a constant address is not something this translation ever produces and
// `And`/`Or` at a pointer result mean the tagging arithmetic rather than plain bit operations.
U32 widthOf(LowerType type) {
    return type == LowerType::Int32 ? 32 : 64;
}

U64 maskOf(U32 bits) {
    return bits >= 64 ? maxLimit<U64> : (U64(1) << bits) - 1;
}

// Whether this value is an integer constant, and what it is - the low bits of its own type, so that
// a mask written as a 64-bit complement and one written as a 32-bit pattern are one constant at
// `Int32`. False for a float immediate, which nothing here folds.
bool lowerConstantOf(LowerBase base, LowerValue* value, U64& into) {
    auto inst = value->inst();
    if(inst->kind != LowerInst::Imm || !isInt(value->type)) return false;

    into = ((LowerImm*)inst)->i & maskOf(widthOf(value->type));
    return true;
}

// The same pattern read as a signed number of that width, which is what every operation that has a
// signed and an unsigned form needs and what a widening cast of a signed source produces.
I64 signedValue(U64 value, U32 bits) {
    if(bits >= 64) return I64(value);

    auto spare = 64 - bits;
    return I64(value << spare) >> spare;
}

// How many bits a constant needs to survive being written out at that width and read back
// sign-extended, which is what every immediate field of every encoding does with one. The unit the
// choice below is made in: two constants that are the same operation on this operand are ranked by
// how narrow a field each would fit in.
U32 immediateWidthOf(U64 value, U32 bits) {
    auto signed64 = signedValue(value, bits);

    for(U32 width = 8; width < 64; width *= 2) {
        if(signedValue(U64(signed64), width) == signed64) return width;
    }

    return 64;
}

/*
 * The top 64 bits of a 64x64 product, out of 32-bit pieces.
 *
 * By hand rather than through a wider integer type because there is no portable one: `__int128` is
 * a GNU extension and this builds under MSVC too. Four partial products, and the only subtle line
 * is the carry - the two cross terms and the top half of the low product all land in the same
 * 32-bit column, and their sum can reach into the next.
 */
U64 highProduct(U64 a, U64 b) {
    auto aLow = a & 0xffffffffu, aHigh = a >> 32;
    auto bLow = b & 0xffffffffu, bHigh = b >> 32;

    auto low = aLow * bLow;
    auto crossA = aHigh * bLow;
    auto crossB = aLow * bHigh;

    auto carry = ((low >> 32) + (crossA & 0xffffffffu) + (crossB & 0xffffffffu)) >> 32;
    return aHigh * bHigh + (crossA >> 32) + (crossB >> 32) + carry;
}

// The same half read as a signed number. The unsigned product treats each sign bit as magnitude
// worth 2^64, so a negative operand has contributed the other operand's whole value one place too
// high, and subtracting it once per negative operand is the correction.
I64 signedHighProduct(I64 a, I64 b) {
    auto high = I64(highProduct(U64(a), U64(b)));
    if(a < 0) high -= b;
    if(b < 0) high -= a;
    return high;
}

// Both operands known: the operation itself, at `bits`. False where the answer is not one this may
// state, which since the division rule below is only a shift by a distance the machine would have
// masked.
bool evaluate(LowerInst::Kind kind, U64 a, U64 b, U32 bits, U64& into) {
    auto sa = signedValue(a, bits);
    auto sb = signedValue(b, bits);
    auto lowest = signedValue(U64(1) << (bits - 1), bits);

    switch(kind) {
        case LowerInst::Add: into = a + b; break;
        case LowerInst::Sub: into = a - b; break;

        // Signed and unsigned multiplication answer the same low bits, which is why the IR keeps the
        // two apart only because the machine does - see the LLVM backend, which merges them too.
        case LowerInst::Mul:
        case LowerInst::IMul: into = a * b; break;

        // `x / 0` is 0 and `x % 0` is `x`, which the language answers rather than the machine - see
        // the ruling beside `Div` in resolve/inst.def. There is no divisor these may not state.
        case LowerInst::Div: into = b ? a / b : 0; break;
        case LowerInst::Rem: into = b ? a % b : a; break;

        // The signed pair has one more: the quotient the width cannot hold, which wraps back to the
        // minimum as every other signed overflow does, and whose remainder is 0. Both are written
        // out rather than computed, `INT64_MIN / -1` being undefined in the C++ this runs in too.
        case LowerInst::IDiv:
            if(!sb) into = 0;
            else if(sa == lowest && sb == -1) into = U64(lowest);
            else into = U64(sa / sb);
            break;
        case LowerInst::IRem:
            if(!sb) into = a;
            else if(sa == lowest && sb == -1) into = 0;
            else into = U64(sa % sb);
            break;

        // The half of the product that does not fit, which is the one thing here that has to be
        // computed at *twice* the operands' width. A 32-bit pair fits a U64; a 64-bit pair is the
        // schoolbook split, because there is no wider integer to hold it.
        case LowerInst::MulHi:
            into = bits <= 32 ? (a * b) >> bits : highProduct(a, b);
            break;
        case LowerInst::IMulHi:
            into = bits <= 32 ? U64(sa * sb) >> bits : U64(signedHighProduct(sa, sb));
            break;

        case LowerInst::Shl: if(b >= bits) return false; into = a << b; break;
        case LowerInst::Shr: if(b >= bits) return false; into = a >> b; break;
        case LowerInst::Sar: if(b >= bits) return false; into = U64(sa >> b); break;

        /*
         * The rotations, which unlike the three above have an answer for *every* distance: the count
         * is defined modulo the width (see the kinds), so there is no out-of-range case to decline.
         *
         * `a` arrives masked to the width already - `foldBinaryValue`'s callers narrow it - so the
         * halves are the value's own bits and nothing above them travels. The `% bits` before the
         * shifts is the modulus and the `(bits - n) % bits` is what keeps a zero count from shifting
         * by the whole width, which is the one distance C++ leaves undefined.
         */
        case LowerInst::Rol: {
            auto n = b % bits;
            into = (a << n) | (a >> ((bits - n) % bits));
            break;
        }
        case LowerInst::Ror: {
            auto n = b % bits;
            into = (a >> n) | (a << ((bits - n) % bits));
            break;
        }

        case LowerInst::And: into = a & b; break;
        case LowerInst::Or:  into = a | b; break;
        case LowerInst::Xor: into = a ^ b; break;

        /*
         * The three BMI2 operations, which unlike the shifts have an answer for every operand pair -
         * see the kinds. The count saturates rather than being refused, and a permutation has no
         * count at all.
         */
        case LowerInst::BitsUpTo: into = b >= bits ? a : a & maskOf(U32(b)); break;

        case LowerInst::GatherBits: {
            U64 out = 0;
            U64 target = 1;

            for(auto mask = b & maskOf(bits); mask != 0; mask &= mask - 1) {
                if(a & (mask & (0 - mask))) out |= target;
                target <<= 1;
            }

            into = out;
            break;
        }

        case LowerInst::ScatterBits: {
            U64 out = 0;
            U64 source = 1;

            for(auto mask = b & maskOf(bits); mask != 0; mask &= mask - 1) {
                auto low = mask & (0 - mask);
                if(a & source) out |= low;
                source <<= 1;
            }

            into = out;
            break;
        }

        default: return false;
    }

    into &= maskOf(bits);
    return true;
}

/*
 * What an operation comes to, before anywhere has been chosen to put it.
 *
 * The answer is separated from the placing because the two callers place it differently: the builder
 * appends, since the instruction it is standing in for had not been created yet, and the pass has to
 * put it exactly where the instruction it replaces was. Nothing below allocates or touches a block.
 */
struct Folded {
    enum Kind: U8 { None, Constant, Operand };

    Kind kind = None;
    U64 constant = 0;
    LowerValue* operand = nullptr;

    static Folded nothing() { return Folded {}; }
    static Folded value(U64 c) { return Folded { Constant, c, nullptr }; }
    static Folded forward(LowerValue* v) { return Folded { Operand, 0, v }; }
};

/*
 * A reinterpretation between two types that are the same type, which is nothing at all.
 *
 * These are written rather than sought out: `loadx`/`load` of a vector reads through a `Ptr`, and the
 * lowering names the address at the type it is about to read - so an address that is already a `Ptr`
 * gets a `bitcast` from `Ptr` to `Ptr`. The instruction is free to *encode* and not free to have: it
 * ends a value's live range and starts another, so a base and an index that would have addressed one
 * instruction between them become `mov` and `add` into a temporary instead of one `[base+index]`, and
 * the pointer-induction pass stops recognizing the address as the induction variable it is.
 *
 * Stated as an equality of `LowerType` rather than of width, because that is the whole of what a
 * bitcast at this level changes: two types of one width that are not one type - `i32x4` and `f32x4` -
 * are a real reinterpretation to every reader that asks which bank the value is in.
 */
bool isNoOpReinterpretation(LowerInst::Kind kind, LowerValue* arg, LowerType type) {
    return kind == LowerInst::Bitcast && arg->type == type;
}

Folded foldUnaryValue(LowerBase base, LowerInst::Kind kind, LowerValue* arg, LowerType type) {
    if(isNoOpReinterpretation(kind, arg, type)) return Folded::forward(arg);

    // The bytes put back the way they came, which is the one identity this operation has and the
    // shape a program that reads a format and writes it again is made of. Ahead of the constant
    // question below because neither of the two is one.
    if(kind == LowerInst::Bswap && arg->inst()->kind == LowerInst::Bswap && arg->type == type) {
        return Folded::forward(base[((LowerInstUnary*)arg->inst())->from]);
    }

    if(!isInt(type) || !isInt(arg->type)) return Folded::nothing();

    U64 value;
    if(!lowerConstantOf(base, arg, value)) return Folded::nothing();

    switch(kind) {
        case LowerInst::Neg: return Folded::value((U64(0) - value) & maskOf(widthOf(type)));
        case LowerInst::Not: return Folded::value(~value & maskOf(widthOf(type)));
        case LowerInst::Bswap: {
            // At the type's own width, which is the whole of what makes this well defined: `bswap`
            // of an `Int32` reverses four bytes and of an `Int64` eight, and a constant folded at
            // the wrong one of those is a different number rather than a wrong-looking one.
            U64 swapped = 0;
            for(U32 i = 0; i < widthOf(type); i += 8) swapped = (swapped << 8) | ((value >> i) & 0xff);

            return Folded::value(swapped);
        }
        default: return Folded::nothing();
    }
}

// Widening carries the sign bit up only for a source that has one and narrowing truncates either
// way, which is `CreateIntCast` and what the x64 selector's constant move does. Float conversions
// are left alone: `lowerConstantOf` declines a float immediate, so both directions fall through.
Folded foldCastValue(LowerBase base, LowerValue* arg, LowerType type, bool signedSource) {
    if(!isInt(type) || !isInt(arg->type)) return Folded::nothing();

    U64 value;
    if(!lowerConstantOf(base, arg, value)) return Folded::nothing();

    auto extended = signedSource ? U64(signedValue(value, widthOf(arg->type))) : value;
    return Folded::value(extended & maskOf(widthOf(type)));
}

/*
 * A binary operation over what is known about its operands.
 *
 * Two tiers, and the line between them is which questions a *pointer* result may be asked. Adding or
 * merging a zero is the identity at every width, so the pointer forms - address arithmetic, and the
 * shift a narrow reference is tagged with - take it too. Everything below that is stated at the
 * result's width, and only an integer type has one.
 */
Folded foldBinaryValue(LowerBase base, LowerInst::Kind kind, LowerValue* lhs, LowerValue* rhs,
                       LowerType type) {
    U64 a = 0, b = 0;
    auto knownLhs = lowerConstantOf(base, lhs, a);
    auto knownRhs = lowerConstantOf(base, rhs, b);
    if(!knownLhs && !knownRhs) return Folded::nothing();

    // Answering with an operand is only right where that operand is already the result's own type.
    // `sub p, q` is the one that says why: it answers a distance, and handing back `p` for it would
    // be an address where an integer was asked for.
    auto forward = [&](LowerValue* value) {
        return value->type == type ? Folded::forward(value) : Folded::nothing();
    };

    Folded zero = Folded::nothing();

    switch(kind) {
        case LowerInst::Add:
        case LowerInst::Or:
        case LowerInst::Xor:
            if(knownRhs && b == 0) zero = forward(lhs);
            else if(knownLhs && a == 0) zero = forward(rhs);
            break;

        case LowerInst::Sub:
            if(knownRhs && b == 0) zero = forward(lhs);
            break;

        default:
            break;
    }

    if(zero.kind != Folded::None) return zero;
    if(!isInt(type) || !isInt(lhs->type) || !isInt(rhs->type)) return Folded::nothing();

    auto bits = widthOf(type);

    if(knownLhs && knownRhs) {
        U64 result;
        if(!evaluate(kind, a, b, bits, result)) return Folded::nothing();

        return Folded::value(result);
    }

    /*
     * One side known, which is the case the masking and shifting produces wherever a field's width
     * fills the unit it sits in or its offset is zero. `maskToWidth` at a full-width type is `x & -1`
     * and `truncateToWidth` at a type whose sign bit is already the register's is `x << 0 >> 0`; both
     * were written to be folded here rather than guarded at every site that can produce one.
     */
    auto all = maskOf(bits);

    switch(kind) {
        case LowerInst::Mul:
        case LowerInst::IMul:
            if(knownRhs && b == 1) return forward(lhs);
            if(knownLhs && a == 1) return forward(rhs);
            if((knownRhs && b == 0) || (knownLhs && a == 0)) return Folded::value(0);
            break;

        // Only the zero, and no `forward` for it: the high half of a product is not one of the
        // operands at any multiplier, and at 1 it is zero unsigned and the sign extension signed.
        case LowerInst::MulHi:
        case LowerInst::IMulHi:
            if((knownRhs && b == 0) || (knownLhs && a == 0)) return Folded::value(0);
            break;

        // A mask keeping every bit its operand can have set, which is the identity - the literal
        // `all` is the case with nothing known, and the rest are the masks a bitfield writes against
        // a value already narrower than the type it is stated at. See `knownZeroBits`.
        case LowerInst::And:
            if(knownRhs && (b | knownZeroBits(base, lhs)) == maxLimit<U64>) return forward(lhs);
            if(knownLhs && (a | knownZeroBits(base, rhs)) == maxLimit<U64>) return forward(rhs);
            if((knownRhs && b == 0) || (knownLhs && a == 0)) return Folded::value(0);
            break;

        case LowerInst::Or:
            if((knownRhs && b == all) || (knownLhs && a == all)) return Folded::value(all);
            break;

        // A shift of zero is zero at every distance, including the ones the constant fold above
        // declines - the machine's masking cannot turn zero into anything else.
        case LowerInst::Shl:
        case LowerInst::Shr:
        case LowerInst::Sar:
            if(knownRhs && b == 0) return forward(lhs);
            if(knownLhs && a == 0) return Folded::value(0);
            break;

        // The same two identities for a rotation, and the zero-count one has to test the count
        // *modulo the width* rather than against zero: a rotation by the width is the identity where
        // a shift by it is not, which is the whole difference between the two kinds.
        case LowerInst::Rol:
        case LowerInst::Ror:
            if(knownRhs && (b % bits) == 0) return forward(lhs);
            if(knownLhs && a == 0) return Folded::value(0);
            break;

        /*
         * The mask a constant count comes to, which is what makes `bitfield` worth writing over this
         * rather than as a shift and a literal.
         *
         * A count at or above the width keeps everything and a count of zero keeps nothing; anything
         * between them is an `and` against a literal, which is one instruction where the operation is
         * three. That rewrite is the whole of why the count is *stated* as saturating rather than
         * left to a machine: the saturation is what the fold answers, so a constant count never
         * reaches a backend at all.
         *
         * The mask is compared against `knownZeroBits` on the same terms `And` above is - a count
         * that covers every bit the value can have set is the identity even where it is below the
         * width, which is the case a field read out of a narrow load produces.
         */
        case LowerInst::BitsUpTo:
            if(knownRhs) {
                if(b >= bits) return forward(lhs);
                if(b == 0) return Folded::value(0);
                if((maskOf(U32(b)) | knownZeroBits(base, lhs)) == maxLimit<U64>) return forward(lhs);
            }

            if(knownLhs && a == 0) return Folded::value(0);
            break;

        // A permutation through a mask that names every position is the value, and through one that
        // names none is nothing. Neither is a special case of the loop in `evaluate` above: this is
        // the half where only the *mask* is known, which is what a permutation against a literal is.
        case LowerInst::GatherBits:
        case LowerInst::ScatterBits:
            if(knownRhs && b == all) return forward(lhs);
            if(knownRhs && b == 0) return Folded::value(0);
            if(knownLhs && a == 0) return Folded::value(0);
            break;

        default:
            break;
    }

    return Folded::nothing();
}

/*
 * Whether this value is already one of the two integers a `Bool` is.
 *
 * A `Cmp` is the only instruction in this IR whose result is defined to be 0 or 1 - every other
 * producer of a truth value is one of these wrapped in something, which is exactly what the two
 * folds below unwrap. A `Select` between the literals 1 and 0 is admitted as well, so that the two
 * rules compose in one round rather than needing a second: `cmp_eq (select 1, 0, c), 1` is the shape
 * lowering actually emits, and answering it needs both halves at once.
 */
bool isBooleanValued(LowerBase base, LowerValue* value) {
    auto inst = value->inst();
    if(inst->kind == LowerInst::Cmp) return true;

    if(inst->kind != LowerInst::Select) return false;

    auto select = (LowerInstSelect*)inst;
    if(select->getEmbeddedCmp()) return false;

    U64 whenTrue, whenFalse;
    if(!lowerConstantOf(base, base[select->lhs], whenTrue)) return false;
    if(!lowerConstantOf(base, base[select->rhs], whenFalse)) return false;

    return whenTrue == 1 && whenFalse == 0;
}

/*
 * A truth value materialized and then asked whether it is true.
 *
 * Two rewrites, and they only pay together. Lowering a `Bool` produces `select 1, 0, %c` - the
 * comparison as a number - and lowering the `if` that reads it produces `cmp_eq %that, 1`, so the
 * four instructions a niche test compiles to are a comparison, a materialization of it, a comparison
 * against the materialization, and the branch. Both middle instructions are identities:
 *
 *   `select 1, 0, %c`  is  `%c`          for a `%c` that is already 0 or 1
 *   `cmp_eq %b, 1`     is  `%b`          for the same reason, and so is `cmp_neq %b, 0`
 *
 * `Maybe(Tree)` is what makes this worth a pass rather than a peephole. `node.left is Just(l)` is a
 * niche range test, and the four instructions above become `cmp; jbe` - seven instructions per node
 * down to three, twice in every walk of the tree.
 *
 * The inverses (`cmp_eq %b, 0`, `cmp_neq %b, 1`) are deliberately not folded. They are the *negation*
 * of `%b`, which is a new instruction rather than one of the operands, and `Folded` says either a
 * constant or an operand already there - inverting the comparison inside `%c` instead would rewrite
 * an instruction this pass does not own, and `%c` may have other readers that wanted it the first way.
 */
Folded foldBooleanValue(LowerBase base, LowerInst* inst) {
    if(inst->kind == LowerInst::Select) {
        auto select = (LowerInstSelect*)inst;
        if(select->getEmbeddedCmp()) return Folded::nothing();

        auto condition = base[select->cmp];
        if(!isBooleanValued(base, condition)) return Folded::nothing();
        if(condition->type != select->result.type) return Folded::nothing();

        U64 whenTrue, whenFalse;
        if(!lowerConstantOf(base, base[select->lhs], whenTrue)) return Folded::nothing();
        if(!lowerConstantOf(base, base[select->rhs], whenFalse)) return Folded::nothing();
        if(whenTrue != 1 || whenFalse != 0) return Folded::nothing();

        return Folded::forward(condition);
    }

    if(inst->kind != LowerInst::Cmp) return Folded::nothing();

    auto compare = (LowerInstCmp*)inst;
    auto kind = compare->getCmp();
    if(kind != LowerCmp::eq && kind != LowerCmp::neq) return Folded::nothing();

    // Either way round: the constant is the one side and the truth value the other.
    auto lhs = base[compare->lhs];
    auto rhs = base[compare->rhs];

    U64 constant;
    LowerValue* boolean = nullptr;

    if(lowerConstantOf(base, rhs, constant)) boolean = lhs;
    else if(lowerConstantOf(base, lhs, constant)) boolean = rhs;
    else return Folded::nothing();

    if(!isBooleanValued(base, boolean)) return Folded::nothing();
    if(boolean->type != compare->result.type) return Folded::nothing();

    auto identity = (kind == LowerCmp::eq && constant == 1) ||
                    (kind == LowerCmp::neq && constant == 0);

    return identity ? Folded::forward(boolean) : Folded::nothing();
}

// The same relation with its operands the other way round, for a comparison written with its
// constant on the left. `a < b` is `b > a`; the equalities read the same either way. Sound here
// because every comparison this file looks at is over integers - the exchange is the one the float
// ordering in the x64 transform has to be careful about, and no float reaches this.
LowerCmp swappedCmp(LowerCmp cmp) {
    switch(cmp) {
        case LowerCmp::gt:  return LowerCmp::lt;
        case LowerCmp::ge:  return LowerCmp::le;
        case LowerCmp::lt:  return LowerCmp::gt;
        case LowerCmp::le:  return LowerCmp::ge;
        case LowerCmp::igt: return LowerCmp::ilt;
        case LowerCmp::ige: return LowerCmp::ile;
        case LowerCmp::ilt: return LowerCmp::igt;
        case LowerCmp::ile: return LowerCmp::ige;
        default: return cmp;
    }
}

// What a comparison against a constant is rewritten into: the same relation over a different operand
// and a different immediate. Only ever `eq` or `neq`, because narrowing is the whole point.
struct Narrowed {
    LowerValue* value;
    U64 constant;
    LowerCmp cmp;
};

/*
 * A range test spelled as a subtraction and an unsigned comparison.
 *
 * Two rewrites, and as with the pair above they are worth much more together than apart. A niche
 * `Maybe` reaches here as the four instructions §10.1 left three of:
 *
 *   %a = sub %x, 1
 *   %b = cmp_le %a, 18446744073709551614      ; that is, `all - 1`
 *
 * which the x64 selector emits as `dec` and a `cmp` against a 64-bit immediate - so a `movabs` for
 * the constant as well. What it means is `x != 0`, which is `test %rax,%rax`.
 *
 *   an unsigned ordering whose true or false set holds one value   is  that equality
 *   `cmp_eq (sub %x, C), K`                                        is  `cmp_eq %x, K + C`
 *
 * The first is what turns the ordering into an equality; the second is what then lets the subtraction
 * go. Neither alone removes an instruction, which is why they are one function and why the pass loop
 * around it has to be able to apply the second to the first's result.
 *
 * The second is gated on the subtraction having exactly one reader. Where it has more it stays, and
 * moving the comparison off it would only lengthen the live range of what it reads - the rewrite is
 * an unqualified win precisely when it is what makes the arithmetic dead.
 *
 * The ends of a range - `x <=u all`, which is always true, and `x <u 0`, which is never - are
 * deliberately not answered here. They are a constant rather than a comparison, so they belong to
 * `Folded`, and nothing in this compiler emits one.
 */
bool narrowComparison(LowerBase base, LowerInst* inst, Narrowed& into) {
    if(inst->kind != LowerInst::Cmp) return false;

    auto compare = (LowerInstCmp*)inst;
    auto lhs = base[compare->lhs];
    auto rhs = base[compare->rhs];

    // Both sides the same integer type: a comparison of pointers reaches here as a `bitcast` and its
    // operands, and a float comparison is not one of these shapes at all.
    if(lhs->type != rhs->type || !isInt(lhs->type)) return false;

    auto cmp = compare->getCmp();
    LowerValue* value;
    U64 constant, other;

    if(!lowerConstantOf(base, rhs, constant)) {
        if(!lowerConstantOf(base, lhs, constant)) return false;

        value = rhs;
        cmp = swappedCmp(cmp);
    } else if(lowerConstantOf(base, lhs, other)) {
        // Both constant. Not this function's business, and answering it would be a rewrite that is
        // not a fixpoint - the loop below runs until nothing changes.
        return false;
    } else {
        value = lhs;
    }

    auto all = maskOf(widthOf(value->type));

    /*
     * The ordering, where one side of it admits a single value. Four relations and two ends each:
     * `<u k` is [0, k-1] and `>=u k` is its complement, so the interesting constants are the ones
     * that leave one number on one side or the other.
     */
    switch(cmp) {
        case LowerCmp::lt:
            if(constant == 1) { into = { value, 0, LowerCmp::eq }; return true; }
            if(constant == all) { into = { value, all, LowerCmp::neq }; return true; }
            break;

        case LowerCmp::le:
            if(constant == 0) { into = { value, 0, LowerCmp::eq }; return true; }
            if(constant == all - 1) { into = { value, all, LowerCmp::neq }; return true; }
            break;

        case LowerCmp::gt:
            if(constant == all - 1) { into = { value, all, LowerCmp::eq }; return true; }
            if(constant == 0) { into = { value, 0, LowerCmp::neq }; return true; }
            break;

        case LowerCmp::ge:
            if(constant == all) { into = { value, all, LowerCmp::eq }; return true; }
            if(constant == 1) { into = { value, 0, LowerCmp::neq }; return true; }
            break;

        default:
            break;
    }

    if(cmp != LowerCmp::eq && cmp != LowerCmp::neq) return false;

    /*
     * The equality, over a value that is itself a constant away from another one. Adding a constant
     * is a bijection on the machine's integers, so the wrapping needs no guard - the two sides move
     * together whatever they wrap through, which is why this holds where the ordering above would
     * not.
     */
    auto source = value->inst();
    if(source->kind != LowerInst::Add && source->kind != LowerInst::Sub) return false;
    if(((LowerInstSingle*)source)->result.uses.size() != 1) return false;

    auto binary = (LowerInstBinary*)source;
    auto from = base[binary->lhs];

    // Declines the pointer forms of both, which answer a different type than they read: `ptr - ptr`
    // is a distance, and there is no constant to move across it.
    if(from->type != value->type || !isInt(from->type)) return false;

    U64 offset;
    if(!lowerConstantOf(base, base[binary->rhs], offset)) return false;

    auto adjusted = source->kind == LowerInst::Sub ? constant + offset : constant - offset;
    into = { from, adjusted & all, cmp };
    return true;
}

// What a mask is rewritten into: the same `and` over a different operand and a different constant.
// The operand changes only for the second of the two rules below; the constant for either.
struct Masked {
    LowerValue* value;
    U64 constant;
};

/*
 * A mask that says more than it has to.
 *
 * A bitfield update reaches here as a four-byte unsigned load into a `Long`, a mask, a merge and a
 * four-byte store, and the masks the front end writes for it are stated at the full width of the
 * type rather than at the width of what was loaded:
 *
 *   %a = load %p, 4              ; bits 32..63 are zero, by what a four-byte load is
 *   %b = and %a, 0xffffffff3fffffff
 *
 * Every bit that mask clears above bit 31 was already zero, so the constant is free to say anything
 * at all up there - and `0x3fffffff` is the same operation written in four bytes rather than eight.
 * The one this backend emitted was the eight-byte one: `movabs` into a register, which in these
 * conventions is a callee-saved one, and an `and` against it. Its neighbour one line up wants the
 * *other* direction - `0xc0000000` and `0xffffffffc0000000` are equally correct there and only the
 * second fits a sign-extended field - which is why both candidates are formed and ranked rather than
 * the free bits simply being cleared.
 *
 * The second rule is what makes the chain collapse rather than only shrink. An `or` or an `xor`
 * whose one side contributes nothing under the mask is an operand the `and` need not read:
 *
 *   `and (or %a, %b), C`   is   `and %b, C`     where every bit of C is known zero in %a
 *
 * which is the whole of a read-modify-write of one bitfield once the load it reassembles has been
 * forwarded. It is gated on the merge having a single reader, for the reason `narrowComparison`
 * gates its second rewrite: where something else still needs the merge, taking this reader off it
 * removes no instruction and leaves two values live where there was one.
 */
bool narrowMask(LowerBase base, LowerInst* inst, Masked& into) {
    if(inst->kind != LowerInst::And) return false;

    auto binary = (LowerInstBinary*)inst;
    auto type = binary->result.type;
    if(!isInt(type)) return false;

    auto lhs = base[binary->lhs];
    auto rhs = base[binary->rhs];

    U64 constant, other;
    LowerValue* value;

    if(lowerConstantOf(base, rhs, constant)) {
        if(lowerConstantOf(base, lhs, other)) return false; // both constant: the plain fold's business
        value = lhs;
    } else if(lowerConstantOf(base, lhs, constant)) {
        value = rhs;
    } else {
        return false;
    }

    if(value->type != type) return false;

    auto bits = widthOf(type);
    auto all = maskOf(bits);
    auto original = value;

    // The merge whose one side the mask discards. Repeated, because a field assembled out of two
    // merges is two of them - each round takes one off, and the bound is the chain's own length.
    for(;;) {
        auto source = value->inst();
        if(source->kind != LowerInst::Or && source->kind != LowerInst::Xor) break;
        if(((LowerInstSingle*)source)->result.uses.size() != 1) break;

        auto merge = (LowerInstBinary*)source;
        auto left = base[merge->lhs];
        auto right = base[merge->rhs];
        if(left->type != type || right->type != type) break;

        if((knownZeroBits(base, left) & constant) == constant) value = right;
        else if((knownZeroBits(base, right) & constant) == constant) value = left;
        else break;
    }

    /*
     * And the constant, once the operand it is read against is settled. The free bits are the ones
     * that operand is known to have as zero: setting them changes nothing about the answer, so the
     * two extremes are formed and the narrower field wins. A tie goes to the cleared one, which is
     * the smaller number and the one a reader recognises as the mask that was meant.
     */
    auto free = knownZeroBits(base, value) & all;
    auto cleared = constant & ~free;
    auto filled = (constant | free) & all;

    auto chosen = immediateWidthOf(filled, bits) < immediateWidthOf(cleared, bits) ? filled : cleared;
    if(value == original && chosen == constant) return false;

    into = { value, chosen };
    return true;
}

/*
 * A branch on the negation of a truth value, which is the same branch the other way round.
 *
 * `foldBooleanValue` declines the inverses on purpose: `not %b` is a new instruction rather than one
 * of the operands, and `Folded` may only answer with something already standing there. At a *branch*
 * it is neither - it is the same terminator with its two arms exchanged, which costs nothing at all
 * and is what makes the value underneath it dead:
 *
 *   %c = cmp_eq %p, 0
 *   %n = xor %c, 1          ; that is, `not %c`
 *   je %n, taken, missed    is  je %c, missed, taken
 *
 * and what that buys is not the `xor`. It is the *flags*: `%c`'s only reader is now the branch, so
 * the x64 selector's flag window (§3.5.2 of codegen/x64/README.md) can carry it there and the whole
 * of `xor r, r; test; sete; xor $1; test; jne` becomes `test; je`. `allocateHeap`'s free-list probe
 * is that shape, in eight of the ten programs in the corpus.
 *
 * Three spellings of the negation, all of which the lowering emits: the explicit `xor %b, 1`, and
 * the two comparisons against a literal that `foldBooleanValue` recognises as identities the other
 * way up. Each needs `%b` to be *already* a truth value, since the rewrite reads it as one.
 */
bool negatedBranchCondition(LowerBase base, LowerInst* terminator, LowerValue*& into) {
    if(terminator->kind != LowerInst::Je) return false;

    auto je = (LowerInstJe*)terminator;
    if(je->getEmbeddedCmp()) return false; // already carried in the flags: not this pass's shape

    auto condition = base[je->cond];
    auto inst = condition->inst();
    if(inst->kind != LowerInst::Xor && inst->kind != LowerInst::Cmp) return false;

    // The truth value and the literal it is set against, either way round. Both spellings read one
    // of each; neither says anything when both sides or neither side is a constant.
    auto binary = (LowerInstBinary*)inst;
    auto lhs = base[binary->lhs];
    auto rhs = base[binary->rhs];

    U64 constant, other;
    LowerValue* boolean;

    if(lowerConstantOf(base, rhs, constant)) {
        if(lowerConstantOf(base, lhs, other)) return false;
        boolean = lhs;
    } else if(lowerConstantOf(base, lhs, constant)) {
        boolean = rhs;
    } else {
        return false;
    }

    // Whether this is the negation of `%b` rather than `%b` itself. `%b == 0` and `%b != 1` are the
    // inverses foldBooleanValue declines; `%b == 1` and `%b != 0` are the identities it has already
    // had its turn at.
    auto inverted = [&] {
        if(inst->kind == LowerInst::Xor) return constant == 1;

        auto kind = ((LowerInstCmp*)inst)->getCmp();
        return (kind == LowerCmp::eq && constant == 0) || (kind == LowerCmp::neq && constant == 1);
    };

    if(!inverted()) return false;
    if(boolean->type != condition->type || !isBooleanValued(base, boolean)) return false;

    into = boolean;
    return true;
}

// What an instruction that is already in a block comes to, for the pass below. One switch over the
// three shapes the folds above cover; everything else answers nothing.
Folded foldInstruction(LowerBase base, LowerInst* inst) {
    if(auto boolean = foldBooleanValue(base, inst); boolean.kind != Folded::None) return boolean;

    if(isCast(inst)) {
        // A `Bitcast` is a unary rather than a `LowerInstCast`, and the only thing asked of one here
        // is whether it reinterprets anything - which a promotion can be what decides, since the
        // operand it reads may not have had its final type when the instruction was built.
        if(inst->kind == LowerInst::Bitcast) {
            auto unary = (LowerInstUnary*)inst;
            return foldUnaryValue(base, inst->kind, base[unary->from], unary->result.type);
        }

        auto cast = (LowerInstCast*)inst;
        return foldCastValue(base, base[cast->from], cast->result.type, cast->isSignedSource());
    }

    if(isUnaryArith(inst)) {
        auto unary = (LowerInstUnary*)inst;
        return foldUnaryValue(base, inst->kind, base[unary->from], unary->result.type);
    }

    if(isBinary(inst) && inst->kind != LowerInst::Cmp) {
        auto binary = (LowerInstBinary*)inst;
        return foldBinaryValue(base, inst->kind, base[binary->lhs], base[binary->rhs],
                               binary->result.type);
    }

    return Folded::nothing();
}

/*
 * The answer as a value, for the builder.
 *
 * An operation that comes to one of its own operands is answered with that operand's *instruction*,
 * since that is what the builders return and what every caller reads `created().ptr` off - so it is
 * declined where the instruction produces more than one value, which would hand the caller the wrong
 * one. The name the operation would have carried is adopted where the operand has none, so that a
 * dump still says what the value was called.
 */
LowerInst* materialize(LowerBase base, LowerModule& module, LowerBlock& block, Folded folded,
                       LowerType type, StringId name) {
    if(folded.kind == Folded::Constant) {
        return block.addInst(base, new (module.arena) LowerImm(name, type, folded.constant));
    }

    if(folded.kind == Folded::Operand) {
        auto inst = folded.operand->inst();
        if(inst->createdCount != 1) return nullptr;

        if(name && !folded.operand->name) folded.operand->name = name;
        return inst;
    }

    return nullptr;
}

} // namespace

// See lower_fold.h. Bounded rather than a fixpoint: every shape this is asked about is the chain of
// masks, shifts and extensions between a narrow load and the operation being simplified, which the
// front end writes out in one block - a phi is what a fixpoint would buy and no caller reaches one.
// The bound is what keeps a query on a long arithmetic chain from walking the whole of it at every
// instruction a pass visits.
static constexpr U32 kMaxKnownBitsDepth = 6;

U64 knownZeroBits(LowerBase base, LowerValue* value, U32 depth) {
    if(!isInt(value->type)) return 0;

    auto bits = widthOf(value->type);
    auto hint = value->unsignedWidthHint();
    auto outside = ~maskOf(bits);
    if(hint && hint < bits) outside |= ~maskOf(hint);
    if(depth >= kMaxKnownBitsDepth) return outside;

    auto inst = value->inst();

    // An operand of the same type as the result, or nothing known. A narrower operand of a wider
    // operation is a value whose upper half is whatever the register happened to hold, which is a
    // question about the backend's representation rather than about the IR - so it is declined here
    // rather than answered from the operand's own width.
    auto zerosOf = [&](LowerPtr<LowerValue> operand) -> U64 {
        auto from = base[operand];
        if(from->type != value->type) return outside;

        return knownZeroBits(base, from, depth + 1);
    };

    // A shift whose distance is a literal inside the width, which is the only kind that says
    // anything: a variable distance leaves every bit of the answer in doubt.
    auto shiftAmount = [&](LowerPtr<LowerValue> operand, U64& into) {
        return lowerConstantOf(base, base[operand], into) && into < bits;
    };

    switch(inst->kind) {
        case LowerInst::Imm: {
            U64 constant;
            if(!lowerConstantOf(base, value, constant)) return outside;

            return outside | ~constant;
        }

        // A narrow unsigned load zeroes everything above what it read, which is where nearly every
        // answer this analysis gives starts: a bitfield is a four-byte load into a `Long`.
        case LowerInst::Load: {
            auto load = (LowerInstLoad*)inst;
            if(load->isSigned()) return outside;

            auto width = load->getWidth() * 8;
            return width >= bits ? outside : outside | ~maskOf(width);
        }

        // The one instruction whose result is defined to be zero or one.
        case LowerInst::Cmp:
            return outside | ~U64(1);

        case LowerInst::And: {
            auto binary = (LowerInstBinary*)inst;
            return outside | zerosOf(binary->lhs) | zerosOf(binary->rhs);
        }

        case LowerInst::Or:
        case LowerInst::Xor: {
            auto binary = (LowerInstBinary*)inst;
            return outside | (zerosOf(binary->lhs) & zerosOf(binary->rhs));
        }

        case LowerInst::Select: {
            auto select = (LowerInstSelect*)inst;
            return outside | (zerosOf(select->lhs) & zerosOf(select->rhs));
        }

        case LowerInst::Shl: {
            auto binary = (LowerInstBinary*)inst;

            U64 amount;
            if(!shiftAmount(binary->rhs, amount)) return outside;

            return outside | ((zerosOf(binary->lhs) << amount) & maskOf(bits)) | maskOf(U32(amount));
        }

        // And the two right shifts, which differ only in whether the bits coming in at the top are
        // known: a logical one brings in zeroes, and an arithmetic one brings in the sign bit, which
        // says the same thing only where that bit is itself known to be zero.
        case LowerInst::Shr:
        case LowerInst::Sar: {
            auto binary = (LowerInstBinary*)inst;

            U64 amount;
            if(!shiftAmount(binary->rhs, amount)) return outside;

            auto inner = zerosOf(binary->lhs) & maskOf(bits);
            auto arriving = maskOf(bits) & ~maskOf(U32(bits - amount));

            if(inst->kind == LowerInst::Sar && !(inner & (U64(1) << (bits - 1)))) return outside;
            return outside | (inner >> amount) | arriving;
        }

        case LowerInst::Cast: {
            auto cast = (LowerInstCast*)inst;
            auto from = base[cast->from];
            if(!isInt(from->type)) return outside;

            auto fromBits = widthOf(from->type);
            auto inner = knownZeroBits(base, from, depth + 1);

            // A truncation keeps what was known of the bits it kept. A widening extension brings in
            // zeroes unless the source is signed, in which case it brings in copies of a sign bit
            // and says nothing unless that bit is known.
            if(fromBits >= bits) return outside | (inner & maskOf(bits));
            if(cast->isSignedSource() && !(inner & (U64(1) << (fromBits - 1)))) {
                return outside | (inner & maskOf(fromBits));
            }

            return outside | inner;
        }

        // A copy, and a bitcast that changes nothing about the bits. A bitcast across widths is a
        // truncation this declines rather than describes; nothing produces one between two integer
        // types, since that is what `Cast` is for.
        case LowerInst::Set:
        case LowerInst::Bitcast: {
            auto unary = (LowerInstUnary*)inst;
            auto from = base[unary->from];
            if(!isInt(from->type) || widthOf(from->type) != bits) return outside;

            return outside | knownZeroBits(base, from, depth + 1);
        }

        default:
            return outside;
    }
}

LowerInst* foldUnaryArith(LowerBase base, LowerModule& module, LowerBlock& block,
                          LowerInst::Kind kind, LowerValue* arg, LowerType type, StringId name) {
    return materialize(base, module, block, foldUnaryValue(base, kind, arg, type), type, name);
}

LowerInst* foldCast(LowerBase base, LowerModule& module, LowerBlock& block, LowerValue* arg,
                    LowerType type, bool signedSource, StringId name) {
    return materialize(base, module, block, foldCastValue(base, arg, type, signedSource), type, name);
}

LowerInst* foldBinary(LowerBase base, LowerModule& module, LowerBlock& block, LowerInst::Kind kind,
                      LowerValue* lhs, LowerValue* rhs, LowerType type, StringId name) {
    return materialize(base, module, block, foldBinaryValue(base, kind, lhs, rhs, type), type, name);
}

// The other operand of a float multiply by exactly two, or nothing. Exactly two: the bit pattern is
// compared rather than the value, so no other power of two and no `-2.0` reaches this - the sign bit
// makes the doubling a subtraction from zero, which `x + x` is not.
Maybe<LowerPtr<LowerValue>> foldFloatDoubling(LowerBase base, LowerInst* inst) {
    if(inst->kind != LowerInst::Mul) return Nothing();

    auto binary = (LowerInstBinary*)inst;
    if(!isFloat(binary->result.type)) return Nothing();

    auto isTwo = [&](LowerPtr<LowerValue> operand) {
        auto value = base[operand]->inst();
        return value->kind == LowerInst::Imm && ((LowerImm*)value)->f == 2.0;
    };

    if(isTwo(binary->rhs)) return Just(binary->lhs);
    if(isTwo(binary->lhs)) return Just(binary->rhs);
    return Nothing();
}

void foldFunctionConstants(LowerBase base, LowerModule& module, LowerFunction& fun) {
    auto changed = true;

    while(changed) {
        changed = false;

        for(auto blockPtr: fun.blocks.contents(base)) {
            auto block = base[blockPtr];

            // Inline: one of these per block per round, holding the instructions of one block while
            // the list it came from is rewritten.
            SmallArray<LowerPtr<LowerInst>, 32> kept;
            auto rewrote = false;

            for(auto instPtr: block->instructions.contents(base)) {
                auto inst = base[instPtr];

                /*
                 * A comparison narrowed in place, before it is asked what it comes to. In place
                 * because the answer is another comparison rather than a constant or an operand
                 * already standing there, which is all `Folded` may say - and the result value is
                 * the same one, so every reader of it stays pointed at the right thing.
                 *
                 * The immediate it now reads is a new one placed immediately above it; the one it
                 * stopped reading is left to `removeDeadConstants` at the end of the pipeline, which
                 * is where every other rewrite here leaves its orphaned literals too.
                 *
                 * The addition or subtraction the second rewrite takes the comparison off is what
                 * `removeDeadValues` below is for, and it is the reason this pass needs one at all:
                 * every other rewrite here replaces the instruction it folded, so this is the only
                 * one that can leave a computation standing with nothing reading it.
                 */
                /*
                 * A float doubled is a float added to itself, which is exact for every value it can
                 * hold - including the infinities, the NaNs and both zeroes, since the two are the
                 * same operation on the same exponent and `x + x` never rounds.
                 *
                 * Worth a rewrite rather than an encoding note because of what it takes *away*. The
                 * multiply needs the constant 2.0 in a vector register, this backend has no constant
                 * pool to load one from (see `emitFloatImm`), and a loop that doubles something on
                 * every iteration therefore holds a whole xmm register for the literal from the
                 * function's entry to its exit - plus a `movaps` at the multiply, since the operation
                 * is two-address and the constant has to survive. `escape` in
                 * test/bench/programs/Float.yana is the measured case: `2.0 * x * y` in its inner
                 * loop is what the register and the copy were being spent on.
                 */
                if(auto doubled = foldFloatDoubling(base, inst)) {
                    auto value = base[doubled.unwrap()];
                    auto result = ((LowerInstSingle*)inst)->created().ptr;
                    auto sum = new (module.arena) LowerInstBinary(result->name, result->type,
                                                                 doubled.unwrap(), doubled.unwrap(),
                                                                 LowerInst::Add);

                    sum->block = blockPtr;
                    sum->source = inst->source;
                    value->uses.push(module.arena, (LowerInst*)sum - base);
                    value->uses.push(module.arena, (LowerInst*)sum - base);

                    detach(base, inst);
                    replaceUses(base, module.arena, result - base, &sum->result - base);
                    kept.push((LowerInst*)sum - base);

                    rewrote = true;
                    changed = true;
                    continue;
                }

                Narrowed narrowed;
                if(narrowComparison(base, inst, narrowed)) {
                    auto compare = (LowerInstCmp*)inst;
                    auto imm = new (module.arena) LowerImm(StringId(), narrowed.value->type,
                                                          narrowed.constant);
                    imm->block = blockPtr;
                    imm->source = inst->source;
                    kept.push(imm - base);

                    setOperand(base, module.arena, inst, compare->lhs, narrowed.value);
                    setOperand(base, module.arena, inst, compare->rhs, &imm->result);
                    compare->setCmp(narrowed.cmp);
                    rewrote = true;
                }

                // And a mask restated over the operand it is actually read against, the same way and
                // for the same reason - the answer is another `and`, which `Folded` cannot say. The
                // fold below then sees whichever of them is the identity, which after this rewrite
                // is most of them.
                Masked masked;
                if(narrowMask(base, inst, masked)) {
                    auto binary = (LowerInstBinary*)inst;
                    auto imm = new (module.arena) LowerImm(StringId(), binary->result.type,
                                                          masked.constant);
                    imm->block = blockPtr;
                    imm->source = inst->source;
                    kept.push(imm - base);

                    setOperand(base, module.arena, inst, binary->lhs, masked.value);
                    setOperand(base, module.arena, inst, binary->rhs, &imm->result);
                    rewrote = true;
                }

                auto folded = foldInstruction(base, inst);

                if(folded.kind == Folded::None) {
                    kept.push(instPtr);
                    continue;
                }

                auto result = ((LowerInstSingle*)inst)->created().ptr;
                auto replacement = folded.operand;

                /*
                 * A constant is a new immediate standing exactly where the instruction stood, which
                 * is what makes it dominate the same readers. `addInst` cannot place it - it appends,
                 * and the list is being rebuilt - so it is attached to the block by hand; an
                 * immediate reads nothing, so there is no use list for that to skip.
                 */
                if(folded.kind == Folded::Constant) {
                    auto imm = new (module.arena) LowerImm(result->name, result->type, folded.constant);
                    imm->block = blockPtr;
                    imm->source = inst->source;
                    kept.push(imm - base);
                    replacement = &imm->result;
                }

                detach(base, inst);
                replaceUses(base, module.arena, result - base, replacement - base);
                rewrote = true;
            }

            /*
             * And the terminator, which is not in the list above and is rewritten in place.
             *
             * The two arms exchange, so `outgoing` and the edge likelihoods exchange with them -
             * both are indexed by edge rather than named per block, which is exactly so that a
             * transform moving the edges around keeps them right by moving the same two entries.
             * `validateLowerModule` checks that `outgoing` still says what the terminator does.
             */
            LowerValue* condition;
            if(negatedBranchCondition(base, base[block->terminator], condition)) {
                auto je = (LowerInstJe*)base[block->terminator];

                setOperand(base, module.arena, je, je->cond, condition);
                ::swap(je->then, je->otherwise);
                ::swap(je->likelihood[0], je->likelihood[1]);
                ::swap(block->outgoing[0], block->outgoing[1]);
                changed = true;
            }

            if(!rewrote) continue;

            block->instructions.clear();
            for(auto instPtr: kept) block->instructions.push(module.arena, instPtr);
            changed = true;
        }

        // Inside the round rather than after it, and unconditionally: what the narrowing above
        // orphans is in whichever block defined it, which this round may already have walked past.
        if(removeDeadValues(base, module.arena, fun)) changed = true;
    }
}

bool isRepeatable(LowerInst* inst) {
    switch(inst->kind) {
        case LowerInst::Set:
        case LowerInst::Cast:  case LowerInst::Bitcast:
        case LowerInst::Neg:   case LowerInst::Not:
        case LowerInst::Add:   case LowerInst::Sub:
        case LowerInst::Mul:   case LowerInst::IMul:
        case LowerInst::Div:   case LowerInst::IDiv:
        case LowerInst::Rem:   case LowerInst::IRem:
        case LowerInst::MulHi: case LowerInst::IMulHi:
        case LowerInst::Shl:   case LowerInst::Shr: case LowerInst::Sar:
        case LowerInst::Rol:   case LowerInst::Ror:
        case LowerInst::And:   case LowerInst::Or:  case LowerInst::Xor:
        case LowerInst::BitsUpTo:
        case LowerInst::GatherBits: case LowerInst::ScatterBits:
        case LowerInst::Cmp:
        case LowerInst::Select:

        // A pure function of one operand, and the reason it is a kind rather than an intrinsic:
        // this list is what an intrinsic is not on.
        case LowerInst::Bswap:

        // The two floating-point operations, which are pure functions of their operands exactly as
        // the arithmetic above is. `Sqrt` needs nothing else - it is a Unary and every pass that
        // asks about one reaches it through `isUnary`. `Fma` is a third arity and had to be told to
        // each of them by name; see `sameComputation`, which would otherwise have read it as a
        // binary and compared two operands at the wrong offsets.
        case LowerInst::Sqrt:
        case LowerInst::Trunc:
        case LowerInst::Floor:
        case LowerInst::Ceil:
        case LowerInst::Round:
        case LowerInst::Fma:

        // All five, and for the same reason the arithmetic above is here: each is a pure function of
        // its operands and of the fields it carries. It is what lets a splat of a loop-invariant
        // scalar be hoisted out of the loop, which is §3.4's highest-value vector optimization and
        // is free - the existing passes do it once the instruction says it may be repeated.
        case LowerInst::VecSplat:
        case LowerInst::VecLane:
        case LowerInst::VecWithLane:
        case LowerInst::VecShuffle:
        case LowerInst::VecReduce:
            return true;
        default:
            return false;
    }
}

bool removeDeadValues(LowerBase base, Region<LowerRegion>& arena, LowerFunction& fun) {
    auto changed = true;
    auto dropped = false;

    while(changed) {
        changed = false;

        for(auto blockPtr: fun.blocks.contents(base)) {
            auto block = base[blockPtr];

            /*
             * The phis first, because they are where a dead chain usually begins.
             *
             * A phi is a pure choice between its alternatives and computes nothing else, so one
             * nothing reads is exactly as dead as an addition nothing reads - and until this was
             * here, nothing removed one: the sweep below walks `instructions`, and a phi is not in
             * that list (see LowerBlock::addInst). What that cost is the *operands*, which stay
             * alive as long as the phi naming them does. `indexOfVectors` in Core is the shape - the
             * position an early `return Just(..)` would have handed back is joined with the one the
             * exhausted-scan path computes, and on the path that returns `Nothing` the join is read
             * by nobody, so the addition feeding it was written out on every no-match tail.
             *
             * Two phis that only read each other are not removed, and the loop below is why: each is
             * a use of the other, so neither is ever seen with an empty use list. That is a whole
             * dead cycle left in place rather than an unsound removal, and it is the one thing a
             * use-count sweep cannot see.
             */
            SmallArray<LowerInstPhi*, 8> deadPhis;
            for(auto phiPtr: block->phis.contents(base)) {
                auto phi = base[phiPtr];
                if(phi->result.uses.isEmpty()) deadPhis.push(phi);
            }

            for(auto phi: deadPhis) {
                detach(base, (LowerInst*)phi);

                for(Size i = 0; i < block->phis.size(); i++) {
                    if(base[block->phis.get(base, i)] != phi) continue;

                    block->phis.remove(base, i);
                    break;
                }

                changed = true;
                dropped = true;
            }

            SmallArray<LowerPtr<LowerInst>, 32> kept;
            auto rewrote = false;

            for(auto instPtr: block->instructions.contents(base)) {
                auto inst = base[instPtr];

                if(isRepeatable(inst) && ((LowerInstSingle*)inst)->created().ptr->uses.isEmpty()) {
                    detach(base, inst);
                    rewrote = true;
                    continue;
                }

                kept.push(instPtr);
            }

            if(!rewrote) continue;

            block->instructions.clear();
            for(auto instPtr: kept) block->instructions.push(arena, instPtr);
            changed = true;
            dropped = true;
        }
    }

    return dropped;
}

void removeDeadConstants(LowerBase base, Region<LowerRegion>& arena, LowerFunction& fun) {
    auto changed = true;

    while(changed) {
        changed = false;

        for(auto blockPtr: fun.blocks.contents(base)) {
            auto block = base[blockPtr];

            // Inline: one of these per block per round of a loop that runs until nothing changes,
            // holding the instructions of one block.
            SmallArray<LowerPtr<LowerInst>, 32> kept;
            auto dropped = false;

            for(auto instPtr: block->instructions.contents(base)) {
                auto inst = base[instPtr];

                if(inst->kind == LowerInst::Imm && inst->created().ptr->uses.isEmpty()) {
                    dropped = true;
                    continue;
                }

                kept.push(instPtr);
            }

            if(!dropped) continue;

            block->instructions.clear();
            for(auto instPtr: kept) block->instructions.push(arena, instPtr);
            changed = true;
        }
    }
}
