#include "lower_fold.h"
#include "lower_builder.h"
#include "fold_arith.h"

// The two's-complement arithmetic itself is `fold_arith.h`, shared with `resolve_ssa/opt/fold.cpp`:
// what an operation means at a width is one question, and this file is the lower IR's half of the
// answer rather than a second copy of it.
using namespace foldArith;

namespace {

// How many bits of a value of this type are the value. Only asked of an integer type; a pointer is
// never folded, because a constant address is not something this translation ever produces and
// `And`/`Or` at a pointer result mean the tagging arithmetic rather than plain bit operations.
U32 widthOf(LowerType type) {
    return type == LowerType::Int32 ? 32 : 64;
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

/*
 * The same question for a float, and the rounding is the whole of what makes it a different one.
 *
 * `LowerImm` keeps every float constant as a `double` whatever the type says, and `emitFloatImm`
 * rounds a single-precision one to what it will actually be at the point its bits are taken. A fold
 * that read the stored double and wrote the stored double would therefore compute a *chain* at
 * double precision and round once at the end, which is not what the machine does - so the value is
 * put through the type's own precision on the way in as well as on the way out. `1.1f + 2.2f` is
 * the shape that tells the two apart.
 */
F64 narrowFloat(F64 value, LowerType type) {
    return type == LowerType::Float32 ? F64(float(value)) : value;
}

bool lowerFloatConstantOf(LowerBase base, LowerValue* value, F64& into) {
    auto inst = value->inst();
    if(inst->kind != LowerInst::Imm || !isFloat(value->type)) return false;

    into = narrowFloat(((LowerImm*)inst)->f, value->type);
    return true;
}

/*
 * Whether a float is one this may hand back as a constant, which is finiteness and nothing else.
 *
 * A NaN and an infinity are values the arithmetic reaches - dividing by zero is the way - and there
 * is no immediate to write either as: `LowerImm` would carry the pattern happily, but the folder
 * answering one would have decided that `0.0/0.0` is *a particular* NaN, and IEEE 754 does not say
 * which. Declined on both the operands and the result, so no rule below has to think about it.
 * `resolve_ssa/opt/pass.h`'s `isFoldableFloat` is the same predicate a tier up.
 */
bool isFoldableFloat(F64 value) { return value == value && value - value == 0.0; }

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
 * What an operation comes to, before anywhere has been chosen to put it.
 *
 * The answer is separated from the placing because the two callers place it differently: the builder
 * appends, since the instruction it is standing in for had not been created yet, and the pass has to
 * put it exactly where the instruction it replaces was. Nothing below allocates or touches a block.
 */
struct Folded {
    enum Kind: U8 { None, Constant, Real, Operand };

    Kind kind = None;
    U64 constant = 0;
    F64 real = 0;
    LowerValue* operand = nullptr;

    static Folded nothing() { return Folded {}; }
    static Folded value(U64 c) { return Folded { Constant, c, 0, nullptr }; }
    static Folded forward(LowerValue* v) { return Folded { Operand, 0, 0, v }; }

    // The float answer, declined where it is not one a literal can be written for - so every rule
    // below states its arithmetic and none of them states the NaN case.
    static Folded number(F64 f, LowerType type) {
        auto rounded = narrowFloat(f, type);
        return isFoldableFloat(rounded) ? Folded { Real, 0, rounded, nullptr } : nothing();
    }
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

    /*
     * The same identity for a reinterpretation: a value read as another type and back again is the
     * value. `isNoOpReinterpretation` above is the one-step case and this is the two-step one, which
     * is what an *address* written as a number looks like in this IR.
     *
     * `null()` is the site. Lower IR has no pointer literal - `LowerImm` carries an integer or a
     * double, and `lowerConstantOf` and everything under it are written for the integer lanes - so a
     * null pointer is the integer zero reinterpreted, and `Core/Text.yana`'s `Handle(null())` then
     * reads it back as a `Size`. That round trip is free to encode and was not free to have: the
     * folder could see a constant on neither side of it, so `handleAddress(nullHandle()) + 11`
     * stayed as an add, a truncation and an add of a value it should have known was zero.
     *
     * The intermediate type has to be at least as wide as the two ends, and that is the whole of the
     * condition. `validateBitcast` permits `Ptr` against an integer of *any* width, so a round trip
     * through a narrower type is a truncation this must not undo; through a wider one the spare bits
     * are unstated on the way in and discarded on the way out, so the low ones arrive unaltered.
     */
    if(kind == LowerInst::Bitcast && arg->inst()->kind == LowerInst::Bitcast) {
        auto inner = base[((LowerInstUnary*)arg->inst())->from];

        if(inner->type == type && arg->type.byteWidth() >= type.byteWidth()) {
            return Folded::forward(inner);
        }
    }

    /*
     * The one float unary that is arithmetic rather than a library call.
     *
     * `Neg` flips the sign bit and that is exactly specified for every value, so nothing about
     * rounding or about which NaN enters into it - `Folded::number` declines a non-finite operand
     * anyway, since it has no way to write one back. `Sqrt` and the four roundings are declined for
     * the reason `resolve_ssa/opt/fold.cpp` gives: each is one machine instruction where it stands,
     * so folding it would buy a constant and cost this file the host's math library.
     */
    if(isFloat(type) && kind == LowerInst::Neg) {
        F64 real;
        if(!lowerFloatConstantOf(base, arg, real)) return Folded::nothing();

        return Folded::number(-real, type);
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
Folded foldCastValue(LowerBase base, LowerValue* arg, LowerType type, bool signedSource,
                     bool signedResult) {
    /*
     * A conversion between two types that are one type, which converts nothing.
     *
     * This IR has two scalar integers and the tier above it has a dozen, so most of what the front
     * end writes as a conversion is a change of *name*: `U8` to `Int`, a `@bits(20)` refinement to
     * the type it refines, an enum's discriminant to the integer it is stored as. All of them arrive
     * here as `cast %x : Int` where `%x` is already an `Int`, and there is nothing for a widening or
     * a truncation to do between two ends of the same width - which `foldCastValue` below already
     * agrees with for a constant source, since sign-extending from `n` bits and masking to `n` bits
     * is the value it started with.
     *
     * They were not free. Each is a value with a live range, so the register allocator has one more
     * interval to colour and one more copy to try to coalesce, and `FormCastCopy` emits nothing only
     * where that coalescing succeeded. Over the 233 `test/resolve` programs **589 of 1827 casts are
     * this**, which is a third of them, and removing them takes 673 instructions out of the corpus -
     * the extra 84 being what the folds behind this one could then answer.
     *
     * Asked before the integer guard rather than inside it, because the statement is about the two
     * types being one type and nothing else: a vector converted to its own shape converts no lane, a
     * `f64` to `f64` rounds nothing. `VecPortable.yana` is where the vector pair lives.
     */
    if(arg->type == type) return Folded::forward(arg);

    /*
     * The value crossing between the two number lines, at whichever pair of ends this cast has.
     *
     * `nameForCast` in lower_print.cpp says which pair a cast is by asking the same two questions in
     * the same order, so `itof` and `ftoi` here are the instructions that print under those names.
     *
     * **Float to integer is the one that is bounded.** The machine truncates toward zero and answers
     * a value the integer cannot hold with whatever `cvttsd2si` produces, and this tier makes no
     * ruling that says otherwise - the language's saturation is decided a tier above (Design-Vector
     * §3.4; `js::saturationRange` in codegen/js/shared.h states it). So the fold is offered only
     * inside the range the result holds, and out of it the conversion stays and the machine answers
     * it, which is what `FoldFloat.yana` asserts.
     */
    if(isFloat(type)) {
        if(isFloat(arg->type)) {
            F64 source;
            if(!lowerFloatConstantOf(base, arg, source)) return Folded::nothing();

            return Folded::number(source, type);
        }

        if(!isInt(arg->type)) return Folded::nothing();

        U64 source;
        if(!lowerConstantOf(base, arg, source)) return Folded::nothing();

        // At the source's own width and sign, which is what decides whether the bits `lowerConstantOf`
        // handed back denote a negative number at all.
        auto bits = widthOf(arg->type);
        return Folded::number(signedSource ? F64(signedValue(source, bits)) : F64(source), type);
    }

    if(!isInt(type)) return Folded::nothing();

    if(isFloat(arg->type)) {
        F64 source;
        if(!lowerFloatConstantOf(base, arg, source)) return Folded::nothing();
        if(!isFoldableFloat(source)) return Folded::nothing();

        /*
         * The bounds as doubles, which is exact for every width this reaches: the largest `Int64` is
         * not a double, so the signed limit is `2^63` itself and the value one below it is the one
         * produced. Compared with `>=` and `<` rather than by rounding the ends, so a value that
         * truncates into range from just outside it is declined rather than folded to the boundary.
         */
        auto bits = widthOf(type);
        auto lowest = signedResult ? -F64(U64(1) << (bits - 1)) : 0.0;
        auto limit = signedResult ? F64(U64(1) << (bits - 1))
                     : bits >= 64 ? 18446744073709551616.0 : F64(U64(1) << bits);

        if(!(source >= lowest && source < limit)) return Folded::nothing();

        // Through the result's own signedness, which is what makes the host conversion defined: the
        // unsigned range above admits values no `I64` holds.
        auto truncated = signedResult ? U64(I64(source)) : U64(source);
        return Folded::value(truncated & maskOf(bits));
    }

    if(!isInt(arg->type)) return Folded::nothing();

    U64 value;
    if(!lowerConstantOf(base, arg, value)) return Folded::nothing();

    auto extended = signedSource ? U64(signedValue(value, widthOf(arg->type))) : value;
    return Folded::value(extended & maskOf(widthOf(type)));
}

/*
 * The value under whatever bitcasts stand over it.
 *
 * A `Bitcast` is the lower IR's "these are the same bits at another type", which is what pointer
 * arithmetic written in the language costs: `difference(from, to)` casts both ends to `%U8` and
 * subtracts, so the two values a rule wants to compare arrive wearing one cast each. Following them
 * is identity-preserving by the instruction's own definition.
 */
LowerValue* underBitcasts(LowerBase base, LowerValue* value) {
    while(true) {
        auto inst = value->inst();
        if(inst->kind != LowerInst::Bitcast) return value;

        auto from = base[((LowerInstUnary*)inst)->from];
        if(widthOf(from->type) != widthOf(value->type)) return value;

        value = from;
    }
}

/*
 * `(p + n) - p` is `n`, at any width and over any base - the distance between a pointer and one
 * offset from it.
 *
 * True in two's complement whatever either side does: both operations wrap at the same width, so
 * the base cancels exactly and nothing is required of `p` at all. Stated over `Add` only, because
 * `(p - n) - p` is `-n` and answering that would be a new instruction rather than one already
 * standing there - which `Folded` may not do.
 *
 * **What produces it** is `byteSpan` in `lib/Native/Run.yana`, which measures a type by moving a
 * pointer and subtracting: `difference(bitcast(from), bitcast(from + count))`. That is the language
 * spelling of `sizeOf`, so it is at every container that asks how many bytes `n` elements occupy -
 * 647 sites in `test/base` on one stage and 29 over the thirteen bench programs on both. Each one
 * came out as a `shl`, an `lea` and a `sub` where the `shl` alone is the answer.
 *
 * Here rather than in either resolver for the reason `reassociateOffset` is here: both native
 * backends read this IR, and the rule is about the arithmetic rather than about what wrote it.
 */
Folded pointerDistance(LowerBase base, LowerValue* lhs, LowerValue* rhs, LowerType type) {
    auto from = underBitcasts(base, rhs);
    auto sum = underBitcasts(base, lhs)->inst();
    if(sum->kind != LowerInst::Add) return Folded::nothing();

    auto binary = (LowerInstBinary*)sum;
    auto left = base[binary->lhs];
    auto right = base[binary->rhs];

    auto offset = underBitcasts(base, left) == from ? right
                : underBitcasts(base, right) == from ? left
                : nullptr;

    // The offset at the distance's own type, which is what `forward` requires everywhere in this
    // file: a value of another width standing in for this one would be a different number.
    if(!offset || offset->type != type) return Folded::nothing();

    return Folded::forward(offset);
}

/*
 * Floating arithmetic, which is the same four operations and a different set of identities.
 *
 * IEEE 754 specifies add, subtract, multiply and divide exactly and both native backends use the
 * hardware's, so the host's own `double` computes what the target computes - at `F32` once
 * `narrowFloat` has put each end and the result back through single precision. This is the tier
 * below `resolve_ssa/opt/fold.cpp`'s `foldFloatBinary` and states the same rules; it exists because
 * an *inlined* body's arithmetic only becomes two constants here, after `promoteStackSlots` has
 * turned the caller's argument and the callee's parameter slot into one value. A generic `x + x`
 * called at `10` was already folded by this file through its integer half; called at `1.5` it
 * reached the machine as `imm 1.5 ; add %v,%v` for want of this one.
 *
 * **`x * 1` and `x / 1` are the whole list of identities**, and every rule an integer reader would
 * write next is wrong: `x + 0` is not `x` because `-0 + 0` is `+0`, `x * 0` is not `0` because
 * `NaN * 0` is `NaN`, and `x - x` is not `0` because an infinity minus itself is a NaN. Which is
 * also why the zero-identity block above is written over `lowerConstantOf`, that being the integer
 * reader: a float immediate is not a constant to it and none of those rules can reach a float.
 */
Folded foldFloatBinary(LowerBase base, LowerInst::Kind kind, LowerValue* lhs, LowerValue* rhs,
                       LowerType type) {
    F64 a = 0, b = 0;
    auto knownLhs = lowerFloatConstantOf(base, lhs, a);
    auto knownRhs = lowerFloatConstantOf(base, rhs, b);

    // The identity, and at the result's own type for the reason `forward` states above: an operand
    // of another precision standing in for this one would be a different number.
    auto forward = [&](LowerValue* value) {
        return value->type == type ? Folded::forward(value) : Folded::nothing();
    };

    if(!knownLhs || !knownRhs) {
        if(knownRhs && b == 1.0 && (kind == LowerInst::Mul || kind == LowerInst::Div)) {
            return forward(lhs);
        }

        if(knownLhs && a == 1.0 && kind == LowerInst::Mul) return forward(rhs);
        return Folded::nothing();
    }

    if(!isFoldableFloat(a) || !isFoldableFloat(b)) return Folded::nothing();

    switch(kind) {
        case LowerInst::Add: return Folded::number(a + b, type);
        case LowerInst::Sub: return Folded::number(a - b, type);
        case LowerInst::Mul: return Folded::number(a * b, type);

        // A zero divisor answers an infinity or a NaN, which `Folded::number` would decline anyway;
        // written out because dividing by it in the compiler is not a thing to leave to that.
        case LowerInst::Div: return b == 0.0 ? Folded::nothing() : Folded::number(a / b, type);

        default: return Folded::nothing();
    }
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
    // Before the constant readings below, because it is the one rule here that needs neither side to
    // be one - see `pointerDistance`.
    if(kind == LowerInst::Sub) {
        auto distance = pointerDistance(base, lhs, rhs, type);
        if(distance.kind != Folded::None) return distance;
    }

    // Ahead of the integer readings rather than beside them, since those begin by declining a pair
    // that has no integer constant in it and a float pair is exactly that.
    if(isFloat(type)) return foldFloatBinary(base, kind, lhs, rhs, type);

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
        if(!evaluateBinary(kind, a, b, bits, result)) return Folded::nothing();

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

        /*
         * A divisor of one, which all four answer without a machine division: the quotient is the
         * dividend and the remainder is zero, on both signednesses and at every value the type
         * holds, the lowest included.
         *
         * `lower_strength.cpp` declines `1` in each of its four cases by name and says the fold
         * behind it takes them - this is that fold, and until now it was not here, so a `div %v, 1`
         * reached the machine as a `cqto` and an `idiv`: 647 of them in `test/base` and 29 over the
         * bench corpus, most from `byteSpan`, where a distance in bytes is divided by a stride the
         * specializer has already made one.
         *
         * Nothing else about a division is an identity worth writing here. A zero divisor is the
         * case `makeDivisionTotal` guards, and a zero *dividend* cannot be folded to zero without
         * proving the divisor non-zero first - which is a fact about the other operand, and this
         * pass only knows the one it can read.
         */
        case LowerInst::Div:
        case LowerInst::IDiv:
            if(knownRhs && b == 1) return forward(lhs);
            break;

        case LowerInst::Rem:
        case LowerInst::IRem:
            if(knownRhs && b == 1) return Folded::value(0);
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
        // names none is nothing. Neither is a special case of the loop in `evaluateBinary` above: this is
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
 * A value read as a truth value, and whether reading it that way gives the value or its complement.
 *
 * Three spellings, one decomposition. Lowering an `if` over a materialized `Bool` produces a
 * comparison against a literal, and the language's `!` over one produces an `xor` against 1:
 *
 *   `cmp_eq %b, 1`   and  `cmp_neq %b, 0`    are  `%b`
 *   `cmp_eq %b, 0`   and  `cmp_neq %b, 1`    are  `!%b`
 *   `xor %b, 1`                              is   `!%b`
 *
 * All five want the same three things established - which side is the literal, whether the literal
 * is one of the two a truth value can be compared against, and whether `%b` is a truth value at all
 * - and the two readers below used to establish them separately, in three copies of the same twenty
 * lines. The copies are what this is instead.
 *
 * `%b` has to *already* be one of 0 and 1, because that is what makes the reading correct: with an
 * arbitrary `%x`, `cmp_neq %x, 0` is a narrowing rather than an identity and `xor %x, 1` flips a bit
 * rather than a truth value.
 *
 * Both sides constant is refused rather than answered. That is the ordinary folder's shape, and
 * answering it here would make this pass's loop something other than a fixpoint.
 */
struct BooleanTest {
    LowerValue* value = nullptr;
    bool inverted = false;
};

bool booleanTest(LowerBase base, LowerInst* inst, BooleanTest& into) {
    if(inst->kind != LowerInst::Cmp && inst->kind != LowerInst::Xor) return false;

    auto relation = LowerCmp::eq;
    if(inst->kind == LowerInst::Cmp) {
        relation = ((LowerInstCmp*)inst)->getCmp();
        if(relation != LowerCmp::eq && relation != LowerCmp::neq) return false;
    }

    // The literal and the value it is set against, either way round.
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

    bool inverted;
    if(inst->kind == LowerInst::Xor) {
        if(constant != 1) return false;
        inverted = true;
    } else if(constant == 1) {
        inverted = relation == LowerCmp::eq ? false : true;
    } else if(constant == 0) {
        inverted = relation == LowerCmp::eq ? true : false;
    } else {
        return false;
    }

    // At the instruction's own width, since a truth value read at a wider one is a different value:
    // the upper half of the register is what the two types disagree about.
    if(boolean->type != binary->result.type) return false;
    if(!isBooleanValued(base, boolean)) return false;

    into = BooleanTest { boolean, inverted };
    return true;
}

/*
 * Two literals compared, which is a number.
 *
 * Written down deliberately here because it used to happen by accident. `foldInstruction` sends
 * every binary but a `Cmp` to `foldBinaryValue` - a comparison answers a different type than it
 * reads, so the identities that pass is built on do not apply to one - and the only thing that ever
 * folded a comparison of two constants was `foldBooleanValue` failing to check that just one of its
 * operands was a literal. It got the right answer for four of the ten relations and left the rest,
 * which is the signature of a rule nobody wrote.
 *
 * The relations divide the way the machine divides them: six read their operands as unsigned and
 * four as signed, and `lowerConstantOf` hands back the low bits of the operand's own type - so the
 * unsigned six compare those bits directly and the signed four have to sign-extend from that width
 * first. `uno` and `ord` are float-only and answer nothing here.
 */
Folded foldConstantCompare(LowerBase base, LowerInst* inst) {
    if(inst->kind != LowerInst::Cmp) return Folded::nothing();

    auto compare = (LowerInstCmp*)inst;
    auto lhs = base[compare->lhs];
    auto rhs = base[compare->rhs];

    // One type either way, and `isInt`/`isFloat` both answer false for a vector - whose comparison
    // produces a mask rather than a number, so "the answer is 1" is not a statement about one.
    if(lhs->type != rhs->type) return Folded::nothing();

    /*
     * The float six, which are the *plain* six rather than the signed four: a float's bits do not
     * order the way the unsigned relations read them, so the resolvers spell every float comparison
     * with `eq`, `neq`, `gt`, `ge`, `lt` and `le` and the machine picks an ordered condition code.
     *
     * `uno` and `ord` are decided by the operands rather than by the relation, and are decided here
     * only because `lowerFloatConstantOf` has already declined a NaN - so both ends are numbers and
     * the pair answers false and true. A comparison whose operand is *not* foldable stays, which is
     * what leaves the one question this cannot answer to the machine.
     */
    if(isFloat(lhs->type)) {
        F64 x, y;
        if(!lowerFloatConstantOf(base, lhs, x) || !lowerFloatConstantOf(base, rhs, y)) {
            return Folded::nothing();
        }

        if(!isFoldableFloat(x) || !isFoldableFloat(y)) return Folded::nothing();

        bool ordered;
        switch(compare->getCmp()) {
            case LowerCmp::eq:  ordered = x == y; break;
            case LowerCmp::neq: ordered = x != y; break;
            case LowerCmp::gt:  ordered = x > y; break;
            case LowerCmp::ge:  ordered = x >= y; break;
            case LowerCmp::lt:  ordered = x < y; break;
            case LowerCmp::le:  ordered = x <= y; break;
            case LowerCmp::uno: ordered = false; break;
            case LowerCmp::ord: ordered = true; break;
            default: return Folded::nothing();
        }

        return Folded::value(ordered ? 1 : 0);
    }

    if(!isInt(lhs->type)) return Folded::nothing();

    U64 a, b;
    if(!lowerConstantOf(base, lhs, a)) return Folded::nothing();
    if(!lowerConstantOf(base, rhs, b)) return Folded::nothing();

    auto bits = widthOf(lhs->type);
    auto signedOf = [&](U64 value) -> I64 {
        return bits >= 64 ? I64(value) : (I64(value << (64 - bits)) >> (64 - bits));
    };

    bool answer;
    switch(compare->getCmp()) {
        case LowerCmp::eq:  answer = a == b; break;
        case LowerCmp::neq: answer = a != b; break;
        case LowerCmp::gt:  answer = a > b; break;
        case LowerCmp::ge:  answer = a >= b; break;
        case LowerCmp::lt:  answer = a < b; break;
        case LowerCmp::le:  answer = a <= b; break;
        case LowerCmp::igt: answer = signedOf(a) > signedOf(b); break;
        case LowerCmp::ige: answer = signedOf(a) >= signedOf(b); break;
        case LowerCmp::ilt: answer = signedOf(a) < signedOf(b); break;
        case LowerCmp::ile: answer = signedOf(a) <= signedOf(b); break;
        default: return Folded::nothing();
    }

    return Folded::value(answer ? 1 : 0);
}

/*
 * A select that decides nothing, which is the other half of the pair above.
 *
 * The same gap and the same reason: `foldInstruction` sends a `Select` to `foldBooleanValue` and
 * nowhere else, so the one thing it could be asked about was whether it materializes a truth value.
 * A *decided* one - a constant condition, or two arms that are the same answer - had no rule at all,
 * and the fold above is what makes those arrive: `cmp_eq 1048575, 1048575` was three constants in
 * `WidePack.yana` and each of them left a `select 1, k, 0` standing behind it.
 *
 * `foldSelect` in resolve_ssa/opt/fold.cpp is the same three rules one tier up, and both are
 * wanted: this tier is where a constant condition is *produced*, by the strength reduction, the
 * division expansion and the fold above, all of which run below the SSA optimizer entirely.
 *
 * The lane-wise select is declined by asking `isInt` of the condition. A vector select's condition
 * is a mask, one lane of which decides one lane of the result, and "the condition is the constant 1"
 * is not a statement about it.
 */
Folded foldConstantSelect(LowerBase base, LowerInst* inst) {
    if(inst->kind != LowerInst::Select) return Folded::nothing();

    auto select = (LowerInstSelect*)inst;
    if(select->getEmbeddedCmp()) return Folded::nothing();

    auto whenTrue = base[select->lhs];
    auto whenFalse = base[select->rhs];
    auto type = select->result.type;

    // Answering with an arm is only right where that arm is already the result's own type - the same
    // rule `foldBinaryValue`'s `forward` states, and for the same reason.
    auto forward = [&](LowerValue* value) {
        return value->type == type ? Folded::forward(value) : Folded::nothing();
    };

    if(whenTrue == whenFalse) return forward(whenTrue);

    U64 a, b;
    auto knownTrue = lowerConstantOf(base, whenTrue, a);
    auto knownFalse = lowerConstantOf(base, whenFalse, b);

    // Two arms that are one number, which two arms producing the same literal leave: an immediate
    // belongs to no block and is materialized per use, so the two were never the same value.
    if(knownTrue && knownFalse && a == b) return Folded::value(a);

    auto condition = base[select->cmp];
    if(!isInt(condition->type)) return Folded::nothing();

    U64 decided;
    if(!lowerConstantOf(base, condition, decided)) return Folded::nothing();

    return forward(decided ? whenTrue : whenFalse);
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
 * The inverses are deliberately not folded, which is the whole of what `inverted` decides here. They
 * are the *negation* of `%b`, which is a new instruction rather than one of the operands, and
 * `Folded` says either a constant or an operand already there - inverting the comparison inside `%c`
 * instead would rewrite an instruction this pass does not own, and `%c` may have other readers that
 * wanted it the first way. `negatedBranchCondition` below is where they are answered, because at a
 * *branch* the negation costs nothing at all.
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

    // The `xor` spelling is one `booleanTest` recognises and this one may not answer: it is always
    // the inverse, and an inverse has no operand to forward to.
    if(inst->kind != LowerInst::Cmp) return Folded::nothing();

    BooleanTest test;
    if(!booleanTest(base, inst, test) || test.inverted) return Folded::nothing();

    return Folded::forward(test.value);
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
        cmp = mirroredCmp(cmp);
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

// What a pair of bounds is rewritten into: one unsigned comparison of `value - low` against `span`,
// `le` where the two bounds were joined by an `and` and `gt` where they were joined by an `or`.
struct RangeTest {
    LowerValue* value;
    U64 low;
    U64 span;
    bool outside;
};

/*
 * One comparison of a value against a constant, read as a bound: the value it reads, and the first
 * or last number the comparison admits (`lower`) or rejects (`!lower`) at the value's width.
 * `lower` asks for a bound below which the answer is false - `ge`, `gt` - and `!lower` for one above
 * which it is false - `le`, `lt`. `isSigned` is which ordering the relation reads its operands in.
 */
static bool boundOf(LowerBase base, LowerInst* inst, bool lower, LowerValue*& value, U64& bound,
                    bool& isSigned) {
    if(inst->kind != LowerInst::Cmp) return false;
    if(((LowerInstSingle*)inst)->result.uses.size() != 1) return false;

    auto compare = (LowerInstCmp*)inst;
    auto lhs = base[compare->lhs];
    auto rhs = base[compare->rhs];
    if(lhs->type != rhs->type || !isInt(lhs->type)) return false;

    auto cmp = compare->getCmp();
    U64 constant, other;

    if(lowerConstantOf(base, rhs, constant)) {
        if(lowerConstantOf(base, lhs, other)) return false;
        value = lhs;
    } else if(lowerConstantOf(base, lhs, constant)) {
        value = rhs;
        cmp = mirroredCmp(cmp);
    } else {
        return false;
    }

    auto bits = widthOf(value->type);
    auto all = maskOf(bits);
    auto top = all >> 1;

    isSigned = cmp == LowerCmp::igt || cmp == LowerCmp::ige || cmp == LowerCmp::ilt || cmp == LowerCmp::ile;
    auto highest = isSigned ? top : all;
    auto lowest = isSigned ? top + 1 : U64(0);

    // `gt k` admits from `k + 1` and `lt k` up to `k - 1`, which is a bound only where that number
    // exists - `gt` of the highest value admits nothing and is the folder's, not this.
    switch(cmp) {
        case LowerCmp::ge: case LowerCmp::ige:
            if(!lower) return false;
            bound = constant;
            return true;
        case LowerCmp::gt: case LowerCmp::igt:
            if(!lower || constant == highest) return false;
            bound = (constant + 1) & all;
            return true;
        case LowerCmp::le: case LowerCmp::ile:
            if(lower) return false;
            bound = constant;
            return true;
        case LowerCmp::lt: case LowerCmp::ilt:
            if(lower || constant == lowest) return false;
            bound = (constant - 1) & all;
            return true;
        default:
            return false;
    }
}

/*
 * A value tested against both ends of a range, as the one unsigned comparison the range is.
 *
 *   `and (cmp_ge %x, A), (cmp_le %x, B)`   is  `cmp_le (sub %x, A), B - A`
 *   `or  (cmp_lt %x, A), (cmp_gt %x, B)`   is  `cmp_gt (sub %x, A), B - A`
 *
 * with the signed relations reading the same way, since subtracting the low end maps `[A, B]` onto
 * `[0, B - A]` in either ordering and sends everything outside it above `B - A` once read unsigned.
 * `gt` and `lt` are the same bounds one step in.
 *
 * It is what a range pattern and `c >= "0" && c <= "9"` both come to, once `fuseBranches` has made the
 * first a single condition: two comparisons, two flag reads and a merge become a `lea` and one `cmp`,
 * and a branch on the result is a `cmp`/`jcc` pair the machine fuses into one operation.
 *
 * Both comparisons must be read by this merge alone, so that they are what goes away. The bounds must
 * be in order in the relations' own signedness: a reversed pair is an empty range or a full one, which
 * is a constant and the folder's.
 */
static bool foldRangeTest(LowerBase base, LowerInst* inst, RangeTest& into) {
    if(inst->kind != LowerInst::And && inst->kind != LowerInst::Or) return false;

    auto binary = (LowerInstBinary*)inst;
    auto left = base[binary->lhs];
    auto right = base[binary->rhs];
    if(!left->inst() || !right->inst()) return false;
    if(!isBooleanValued(base, left) || !isBooleanValued(base, right)) return false;

    // An `and` admits between its bounds, so it wants its lower bound from a `ge` and its upper one
    // from a `le`. An `or` of the two *rejections* admits outside them - `lt` below the low end and
    // `gt` above the high one - which is the same pair of bounds read from the other side.
    auto outside = inst->kind == LowerInst::Or;

    for(int order = 0; order < 2; order++) {
        auto first = order == 0 ? left->inst() : right->inst();
        auto second = order == 0 ? right->inst() : left->inst();

        LowerValue* lowValue;
        LowerValue* highValue;
        U64 low, high;
        bool lowSigned, highSigned;

        if(!outside) {
            if(!boundOf(base, first, true, lowValue, low, lowSigned)) continue;
            if(!boundOf(base, second, false, highValue, high, highSigned)) continue;
        } else {
            // `lt A` rejects from `A` up, which is a lower bound of the admitted range read as
            // `!(x >= A)`; `boundOf` answers `A - 1` for it, so the admitted bound is one above.
            if(!boundOf(base, first, false, lowValue, low, lowSigned)) continue;
            if(!boundOf(base, second, true, highValue, high, highSigned)) continue;

            // A rejection bound at the far end of the type rejects nothing on that side, so the
            // `or` is always true - the folder's constant, and not a range.
            auto bits = widthOf(lowValue->type);
            auto all = maskOf(bits);
            auto signedBounds = lowSigned || highSigned;
            if(low == (signedBounds ? all >> 1 : all)) continue;
            if(high == (signedBounds ? (all >> 1) + 1 : U64(0))) continue;

            low = (low + 1) & all;
            high = (high - 1) & all;
        }

        if(lowValue != highValue || lowSigned != highSigned) continue;

        auto bits = widthOf(lowValue->type);
        auto inOrder = lowSigned ? signedValue(low, bits) <= signedValue(high, bits) : low <= high;
        if(!inOrder) continue;

        into = { lowValue, low, (high - low) & maskOf(bits), outside };
        return true;
    }

    return false;
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
 * `(p + c1) + c2` over a pointer, restated as `p + (c1 + c2)`.
 *
 * Pointer arithmetic only, and that is what makes this a separate rule rather than a line in
 * `reassociate` in resolve_ssa/opt/fold.cpp. That one is over integers and declines a pointer
 * type outright, so an offset chain reaches the backends exactly as the front end wrote it - and the
 * front end writes it left-associatively, which is a *chain*: `p + a + b + c` is three adds each
 * reading the one above.
 *
 * What that costs is visible only in the x64 addressing mode. `foldAddresses` folds `[base + disp]`
 * into the access that reads it, but only where the value is used as an address and nowhere else -
 * and every link of a chain is also read by the next link, so none of them qualifies. An unrolled
 * block move came out as `lea 0x20(%r9),%r9` between every pair of transfers, six instructions of
 * pure address arithmetic in a loop whose whole body is eight. Restated against one base, each
 * offset has exactly one reader again, and each becomes a displacement that costs nothing.
 *
 * Nothing is required of `p` and nothing about how many readers the inner add has: it stays where it
 * is, and `removeDeadValues` takes it if this was the last one. That matters here - the first link
 * of the chain usually *is* still read, by the transfer at offset zero.
 *
 * The kind is not rewritten, only the two operands, so a subtraction stays a subtraction and carries
 * the combined constant with its own sign: `(p ± c1) - c2` is `p - (c2 ∓ c1)`. Both sides wrap at
 * the same width, and an address wraps with them.
 */
struct Offset {
    LowerValue* pointer;
    U64 constant;
};

bool reassociateOffset(LowerBase base, LowerInst* inst, Offset& into) {
    if(inst->kind != LowerInst::Add && inst->kind != LowerInst::Sub) return false;

    auto binary = (LowerInstBinary*)inst;
    auto type = binary->result.type;
    if(!isPtr(type)) return false;

    U64 outer;
    if(!lowerConstantOf(base, base[binary->rhs], outer)) return false;

    auto lhs = base[binary->lhs];
    if(lhs->type != type) return false;

    auto source = lhs->inst();
    if(source->kind != LowerInst::Add && source->kind != LowerInst::Sub) return false;

    auto inner = (LowerInstBinary*)source;
    U64 middle;
    if(!lowerConstantOf(base, base[inner->rhs], middle)) return false;

    auto pointer = base[inner->lhs];
    if(pointer->type != type) return false;

    // The inner constant as this instruction would have to read it: added when the inner operation
    // added it, negated when it subtracted.
    auto signed_ = source->kind == LowerInst::Sub ? U64(0) - middle : middle;
    auto combined = inst->kind == LowerInst::Sub ? outer - signed_ : outer + signed_;

    into = { pointer, combined };
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
 * So this is the other half of `booleanTest` and nothing more: the three spellings, the literal side
 * and the requirement that `%b` is already a truth value are all established there, and what is left
 * here is which of the two answers this reader wants.
 */
bool negatedBranchCondition(LowerBase base, LowerInst* terminator, LowerValue*& into) {
    if(terminator->kind != LowerInst::Je) return false;

    auto je = (LowerInstJe*)terminator;
    if(je->getEmbeddedCmp()) return false; // already carried in the flags: not this pass's shape

    BooleanTest test;
    if(!booleanTest(base, base[je->cond]->inst(), test) || !test.inverted) return false;

    into = test.value;
    return true;
}

/*
 * A truncation undone by the extension that reads it - `cast (cast %x : I32) : I64` over an `%x` of
 * the wider type whose dropped bits are known to be zero - which is `%x`. J145, J94 §6.2 F.
 *
 * It is the packed capacity word's unpack: `ArrayHeader`'s capacity is the low 30 bits of an `I64`,
 * the library narrows the masked word to the `Int` it declares and every reader widens it back to
 * the `I64` a count is, so `and; cast; cast` stood at every capacity read in the corpus. The bits
 * the truncation drops are compared together with the narrow sign bit, which is what makes either
 * extension - zero or sign - hand back the bits the truncation kept and nothing else.
 */
static Folded foldCastRoundTrip(LowerBase base, LowerInstCast* cast) {
    auto from = base[cast->from];
    auto type = cast->result.type;
    if(!isInt(type) || !isInt(from->type)) return Folded::nothing();

    auto narrow = widthOf(from->type);
    auto wide = widthOf(type);
    if(narrow >= wide || narrow == 0) return Folded::nothing();

    auto inner = from->inst();
    if(inner->kind != LowerInst::Cast) return Folded::nothing();

    auto source = base[((LowerInstCast*)inner)->from];
    if(source->type != type) return Folded::nothing();

    auto dropped = maskOf(wide) & ~maskOf(narrow - 1);
    if((knownZeroBits(base, source) & dropped) != dropped) return Folded::nothing();

    return Folded::forward(source);
}

/*
 * `cast (shr (cast %x : W) k : W) : N` over an `%x` of the narrow type - a shift done at the wide
 * width only because the operand was widened for it - restated as `shr %x, k` at `N`, or `sar` where
 * the widening copied a sign bit that is not known to be zero. J145, J94 §6.2 F: the packed
 * capacity word's flag is `word >> 30` read as an `Int`, and where the word arrives narrow (a
 * flattened `String` argument) the unpack widened it, shifted, and narrowed again.
 *
 * Exact for every `k < N`: the bits the narrow result keeps are `%x`'s from `k` up and then what the
 * extension brought in, which is zero for a zero extension and the narrow sign bit for a sign one.
 * Only where the wide shift has no other reader, so the rewrite never adds an instruction.
 */
static bool narrowShift(LowerBase base, LowerInst* inst, LowerValue*& value, U64& amount,
                        bool& arithmetic) {
    if(inst->kind != LowerInst::Cast) return false;

    auto cast = (LowerInstCast*)inst;
    auto type = cast->result.type;
    auto from = base[cast->from];
    if(!isInt(type) || !isInt(from->type)) return false;

    auto narrow = widthOf(type);
    if(narrow == 0 || narrow >= widthOf(from->type)) return false;

    auto shift = from->inst();
    if(shift->kind != LowerInst::Shr || from->uses.size() != 1) return false;

    auto binary = (LowerInstBinary*)shift;
    if(!lowerConstantOf(base, base[binary->rhs], amount) || amount == 0 || amount >= narrow) {
        return false;
    }

    auto widened = base[binary->lhs]->inst();
    if(widened->kind != LowerInst::Cast) return false;

    auto extension = (LowerInstCast*)widened;
    auto source = base[extension->from];
    if(source->type != type) return false;

    arithmetic = extension->isSignedSource()
              && !(knownZeroBits(base, source) & (U64(1) << (narrow - 1)));
    value = source;
    return true;
}

// What an instruction that is already in a block comes to, for the pass below. One switch over the
// three shapes the folds above cover; everything else answers nothing.
Folded foldInstruction(LowerBase base, LowerInst* inst) {
    if(auto boolean = foldBooleanValue(base, inst); boolean.kind != Folded::None) return boolean;
    if(auto compared = foldConstantCompare(base, inst); compared.kind != Folded::None) return compared;
    if(auto selected = foldConstantSelect(base, inst); selected.kind != Folded::None) return selected;

    if(isCast(inst)) {
        // A `Bitcast` is a unary rather than a `LowerInstCast`, and the only thing asked of one here
        // is whether it reinterprets anything - which a promotion can be what decides, since the
        // operand it reads may not have had its final type when the instruction was built.
        if(inst->kind == LowerInst::Bitcast) {
            auto unary = (LowerInstUnary*)inst;
            return foldUnaryValue(base, inst->kind, base[unary->from], unary->result.type);
        }

        auto cast = (LowerInstCast*)inst;
        if(auto trip = foldCastRoundTrip(base, cast); trip.kind != Folded::None) return trip;

        return foldCastValue(base, base[cast->from], cast->result.type, cast->isSignedSource(),
                             cast->isSignedResult());
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

    // The float overload of the same constructor - `LowerImm` holds one union and which member was
    // written is the whole difference, so the two are named apart here rather than being one call
    // with a converted argument.
    if(folded.kind == Folded::Real) {
        return block.addInst(base, new (module.arena) LowerImm(name, type, folded.real));
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

/*
 * And two of its own for a phi, because a phi is the only case that *branches* and the only one that
 * can reach itself.
 *
 * The depth bound keeps a whole merge cluster from being expanded out of the middle of some other
 * query; the cluster bound is what the walk below spends instead of depth. Two levels answer the
 * shapes that motivated the case and bound the fan-out at the square of the widest phi.
 */
static constexpr U32 kMaxKnownBitsPhiDepth = 2;
static constexpr Size kMaxKnownBitsPhiCluster = 16;

/*
 * What a cluster of phis carries, which is a question about the values that *enter* it.
 *
 * A phi cycle is the one shape a recursive query cannot answer by recursing. `promoteStackSlots`
 * turns a flag assigned in a loop into two phis that name each other -
 *
 *     %a = phi [entry, 0], [latch, %b]
 *     %b = phi [head, %a], [set, 1]
 *
 * - and a walk that follows operands runs out of depth on the way round and answers "nothing known",
 * which is what left the last `xor $1` in `scanDecimal` standing.
 *
 * ## Why this is not the optimistic fixpoint it looks like
 *
 * The textbook answer is to assume every bit zero for a value already on the stack and iterate to a
 * fixed point. Assuming it and *not* iterating is unsound, and the counter-example is two lines:
 *
 *     %a = phi [entry, 1], [latch, %b]
 *     %b = shl %a, 1
 *
 * One pass with `%a` assumed clear answers "every bit above the lowest is zero", and `%a` is every
 * power of two in turn. So the assumption has to be paid for with iteration - unless the cycle
 * cannot transform what goes round it, which is exactly the restriction here.
 *
 * **Every step of the cycle is a phi**, so nothing on it computes: a phi selects between values that
 * already exist, so the set of values a cluster of them can carry is precisely the set entering it
 * from outside. The answer is therefore the meet over the cluster's *non-phi* inputs, computed once,
 * with no assumption to discharge and no iteration to converge. The `shl` above is not a phi and so
 * is not on any cycle this walks; it is an ordinary input, asked the ordinary way.
 *
 * Bounded by the cluster size rather than by depth, since the walk visits each phi once. A cluster
 * with no input from outside is unreachable code, and answers nothing rather than everything.
 */
static U64 phiClusterZeros(LowerBase base, LowerValue* root, U32 depth) {
    SmallArray<LowerValue*, kMaxKnownBitsPhiCluster> cluster;
    cluster.push(root);

    auto known = maxLimit<U64>;
    auto entered = false;

    for(Size i = 0; i < cluster.size(); i++) {
        auto used = ((LowerInstPhi*)cluster[i]->inst())->used();
        if(!used.length) return 0;

        for(Size j = 0; j < used.length; j++) {
            // An alternative that is not there yet, which is a phi under construction: promotion
            // names its phis before it knows what they merge, and the values it builds in the
            // meantime read one. See `makePhi` in lower_promote.cpp.
            if(used.ptr[j] == nullptr) return 0;

            auto from = base[used.ptr[j]];

            // One type across the whole cluster, on `zerosOf`'s terms: a narrower operand of a wider
            // merge is a value whose upper half is whatever the register held.
            if(from->type != root->type) return 0;

            if(from->inst()->kind == LowerInst::Phi) {
                auto seen = false;
                for(auto other: cluster) if(other == from) { seen = true; break; }
                if(seen) continue;

                if(cluster.size() >= kMaxKnownBitsPhiCluster) return 0;
                cluster.push(from);
                continue;
            }

            entered = true;
            known &= knownZeroBits(base, from, depth + 1);
        }
    }

    return entered ? known : 0;
}

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
            if(load->isVolatile()) return outside;
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

        /*
         * A value merged from several, which is the meet: a bit is zero here only where it is zero
         * on every edge that arrives. `zerosOf` is what states that per operand, and the fold over
         * it starts from "every bit" so that the first edge decides and the rest can only weaken.
         *
         * This is the one case a value can have without any instruction *computing* it, and the
         * reason it is worth having is that the tier below builds them: `promoteStackSlots` turns a
         * local written on two paths into a phi, so a flag assigned `True` on one arm and `False` on
         * the other has no definition to read the fact off after promotion. `Real.yana`'s
         * `select %p, 1, 0` over such a phi was the site that named this.
         */
        case LowerInst::Phi: {
            if(depth >= kMaxKnownBitsPhiDepth) return outside;

            return outside | phiClusterZeros(base, value, depth);
        }

        /*
         * A mask read as an integer, which is the one reduction that bounds its own answer:
         * `LowerReduce::Bits` puts lane `i` in bit `i` and nothing above the lane count, so a
         * four-lane mask answers a number below 16 whatever the lanes held.
         *
         * The other reductions are arithmetic over the lanes and say nothing here. The two that do
         * answer a truth value - `And` and `Or` over a mask, which are `all` and `any` - need no
         * case: their resolve type is `Bool`, so `normalFormBits` has already stated the one bit on
         * the value itself, which is both cheaper and reaches further than reading it back off the
         * instruction would.
         */
        case LowerInst::VecReduce: {
            auto reduce = (LowerInstVecReduce*)inst;
            if(reduce->getReduce() != LowerReduce::Bits) return outside;

            auto from = base[reduce->from];
            if(!from->type.isMask()) return outside;

            return outside | ~maskOf(from->type.lanes());
        }

        /*
         * The three counting operations, whose answer is bounded by the width of what they counted:
         * a population count, a trailing-zero count and a leading-zero count are all in [0, w], so
         * everything above the bits it takes to write `w` is zero. A 64-bit operand answers at most
         * 64, which is seven bits, so bits 7 and above are clear - and that is what makes a count
         * co-pack, compare and mask like the small number it is.
         *
         * `Cttz` and `Bsr` are deliberately absent. Both are **undefined at a zero operand**, and
         * what a machine leaves in the register there is not a number this may make claims about -
         * see the note on the pair in lower_inst.h. The three here are the total ones.
         */
        case LowerInst::Intrinsic: {
            auto intrinsic = (LowerInstIntrinsic*)inst;
            auto kind = intrinsic->getIntrinsic();

            if(kind != LowerIntrinsic::Popcnt && kind != LowerIntrinsic::CttzWidth &&
               kind != LowerIntrinsic::ClzWidth)
            {
                return outside;
            }

            auto used = intrinsic->used();
            if(used.length != 1) return outside;

            auto from = base[used.ptr[0]];
            if(!isInt(from->type)) return outside;

            // The bits it takes to write the operand's width, which is the count's own ceiling.
            auto counted = widthOf(from->type);
            U32 answerBits = 1;
            while(answerBits < 64 && (U64(1) << answerBits) <= U64(counted)) answerBits++;

            return answerBits >= bits ? outside : outside | ~maskOf(answerBits);
        }

        default:
            return outside;
    }
}

/*
 * Whether this value is already one of the two integers a `Bool` is.
 *
 * Which is one question about known bits and not a second analysis: a truth value is a value every
 * bit of which above the lowest is zero, and `knownZeroBits` is the function that answers that for
 * any value at all. This used to be its own walk - a `Cmp`, the two literals, the three bitwise
 * operations over those, and a `Select` between 1 and 0 - and every one of those five is a case
 * that function already had, stated there in terms of which bits survive rather than in terms of
 * which instructions are truth values.
 *
 * **What the restatement buys is everything the walk could not reach**, and the two halves of that
 * are worth naming because they are why the duplication was costing something:
 *
 *  - The instructions `knownZeroBits` knows about and the walk did not. A cast, a narrow unsigned
 *    load, a shift by a literal, a `Set`, and an `and` against a mask of one bit - which is how a
 *    `@bits(1)` field arrives. `Packed.yana`, `Record.yana` and `ScalarSum.yana` between them held
 *    18 sites of `and %f, 1; cast; select 1, 0` where the walk stopped at the cast and the
 *    materialization survived; all 18 fold now.
 *  - The hint, which is not an instruction at all. `normalFormBits` in resolve_ssa/lower/gen.cpp
 *    states a `Bool`'s one bit on the value itself, so a truth value that came out of a call, a
 *    parameter, a phi or a mask reduction is one this answers for - and none of those has a
 *    definition any walk over the instruction graph could read the fact off.
 *
 * The bound and the recursion go with the walk. `knownZeroBits` has its own six-level bound and its
 * own argument for it, which is the one that governs now.
 */
bool isBooleanValued(LowerBase base, LowerValue* value) {
    return (knownZeroBits(base, value) | 1) == maxLimit<U64>;
}

LowerInst* foldUnaryArith(LowerBase base, LowerModule& module, LowerBlock& block,
                          LowerInst::Kind kind, LowerValue* arg, LowerType type, StringId name) {
    return materialize(base, module, block, foldUnaryValue(base, kind, arg, type), type, name);
}

LowerInst* foldCast(LowerBase base, LowerModule& module, LowerBlock& block, LowerValue* arg,
                    LowerType type, bool signedSource, bool signedResult, StringId name) {
    return materialize(base, module, block, foldCastValue(base, arg, type, signedSource, signedResult),
                       type, name);
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
                 * stopped reading is left to `removeDeadValues` at the end of the pipeline, which
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

                /*
                 * A range tested at both ends, as one comparison - see `foldRangeTest`. A new
                 * subtraction and comparison in place of the merge, since the answer is a different
                 * kind of instruction; the two comparisons it read are left to `removeDeadValues`.
                 */
                RangeTest range;
                if(foldRangeTest(base, inst, range)) {
                    auto merged = (LowerInstBinary*)inst;
                    auto type = range.value->type;
                    auto subject = range.value;

                    if(range.low != 0) {
                        auto low = new (module.arena) LowerImm(StringId(), type, range.low);
                        low->block = blockPtr;
                        low->source = inst->source;
                        kept.push(low - base);

                        auto shifted = new (module.arena) LowerInstBinary(StringId(), type,
                                                                          range.value - base,
                                                                          &low->result - base,
                                                                          LowerInst::Sub);
                        shifted->block = blockPtr;
                        shifted->source = inst->source;
                        range.value->uses.push(module.arena, (LowerInst*)shifted - base);
                        low->result.uses.push(module.arena, (LowerInst*)shifted - base);
                        kept.push((LowerInst*)shifted - base);
                        subject = &shifted->result;
                    }

                    auto span = new (module.arena) LowerImm(StringId(), type, range.span);
                    span->block = blockPtr;
                    span->source = inst->source;
                    kept.push(span - base);

                    auto compare = new (module.arena) LowerInstCmp(merged->result.name, subject - base,
                                                                   &span->result - base,
                                                                   range.outside ? LowerCmp::gt
                                                                                 : LowerCmp::le,
                                                                   merged->result.type);
                    compare->block = blockPtr;
                    compare->source = inst->source;
                    subject->uses.push(module.arena, (LowerInst*)compare - base);
                    span->result.uses.push(module.arena, (LowerInst*)compare - base);
                    kept.push((LowerInst*)compare - base);

                    detach(base, inst);
                    replaceUses(base, module.arena, &merged->result - base, &compare->result - base);

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
                // And an offset chain restated against the pointer it started from, which is the
                // same shape of rewrite: two operands replaced in place, the answer being another
                // add rather than anything `Folded` may say.
                Offset offset;
                if(reassociateOffset(base, inst, offset)) {
                    auto binary = (LowerInstBinary*)inst;
                    auto imm = new (module.arena) LowerImm(StringId(), base[binary->rhs]->type,
                                                          offset.constant);
                    imm->block = blockPtr;
                    imm->source = inst->source;
                    kept.push(imm - base);

                    setOperand(base, module.arena, inst, binary->lhs, offset.pointer);
                    setOperand(base, module.arena, inst, binary->rhs, &imm->result);
                    rewrote = true;
                }

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

                // And a shift done at the wide width only because its operand was widened for it,
                // which is a new instruction rather than a `Folded` answer - see `narrowShift`.
                LowerValue* shifted;
                U64 amount;
                bool arithmetic;
                if(narrowShift(base, inst, shifted, amount, arithmetic)) {
                    auto cast = (LowerInstCast*)inst;
                    auto type = cast->result.type;

                    auto imm = new (module.arena) LowerImm(StringId(), type, amount);
                    imm->block = blockPtr;
                    imm->source = inst->source;
                    kept.push(imm - base);

                    auto shift = new (module.arena) LowerInstBinary(
                        cast->result.name, type, shifted - base, &imm->result - base,
                        arithmetic ? LowerInst::Sar : LowerInst::Shr);
                    shift->block = blockPtr;
                    shift->source = inst->source;
                    shifted->uses.push(module.arena, (LowerInst*)shift - base);
                    imm->result.uses.push(module.arena, (LowerInst*)shift - base);
                    kept.push((LowerInst*)shift - base);

                    detach(base, inst);
                    replaceUses(base, module.arena, &cast->result - base, &shift->result - base);

                    rewrote = true;
                    changed = true;
                    continue;
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
                if(folded.kind == Folded::Constant || folded.kind == Folded::Real) {
                    auto imm = folded.kind == Folded::Real
                        ? new (module.arena) LowerImm(result->name, result->type, folded.real)
                        : new (module.arena) LowerImm(result->name, result->type, folded.constant);

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
                block->setSuccessors(base, je->then, je->otherwise);
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

/*
 * Whether every reader of `address` only writes *into* it - a store, a copy or a pattern fill naming
 * it as the destination and nothing else - gathering those readers as it goes.
 *
 * A constant offset from the address is the same question one step along, so it is followed, twice
 * at most: that is a field of a field, and nothing this tier writes goes deeper into a temporary.
 * Anything else - a load, a copy *out*, the address stored as a value, handed to a call, compared - is
 * a way for what was written to be seen, and ends the question with no.
 */
static bool onlyWrittenThrough(LowerBase base, LowerPtr<LowerValue> address,
                               SmallArray<LowerPtr<LowerInst>, 16>& gathered, U32 depth) {
    auto& uses = base[address]->uses;

    for(Size i = 0; i < uses.size(); i++) {
        auto userPtr = uses.get(base, i);
        auto user = base[userPtr];

        switch(user->kind) {
            case LowerInst::Store: {
                auto store = (LowerInstStore*)user;
                if(store->to != address || store->value == address) return false;
                break;
            }
            case LowerInst::Copy: {
                auto copy = (LowerInstCopy*)user;
                if(copy->to != address || copy->from == address || copy->count == address) return false;
                break;
            }
            case LowerInst::SetPattern: {
                auto fill = (LowerInstSetPattern*)user;
                if(fill->to != address || fill->count == address || fill->pattern == address) return false;
                break;
            }
            case LowerInst::Add: {
                auto add = (LowerInstBinary*)user;
                if(depth >= 2 || add->lhs != address || add->rhs == address) return false;
                if(base[add->rhs]->inst()->kind != LowerInst::Imm) return false;
                if(!onlyWrittenThrough(base, add->created().ptr - base, gathered, depth + 1)) return false;
                break;
            }
            default:
                return false;
        }

        gathered.push(userPtr);
    }

    return true;
}

/*
 * Storage nothing reads - an `alloca` whose every use writes into it - removed with those writes.
 *
 * The writes are what keep such a slot alive, and nothing else in the sweep will take them: a store or
 * a copy has no result, so it is never unused, and the allocation it names is never unused while it
 * stands. So a temporary that is filled and then never read survived every pass to the backend, as a
 * frame slot and a memory write. `splitAggregateSlots` makes these: a value read out of a heap block
 * into a temporary and then handed to a cell nothing reads - the element an array's `remove` took out
 * and nobody wanted - leaves the temporary with its copy in and no copy out once the dead cell's move
 * is dropped.
 *
 * Sound for the reason a dead load is: the address never leaves (every use was accounted for above),
 * so no reader anywhere can observe what the writes put there.
 */
static bool removeWriteOnlySlots(LowerBase base, Region<LowerRegion>& arena, LowerFunction& fun) {
    HashMap<U32, U8> doomed;
    auto found = false;

    for(auto blockPtr: fun.blocks.contents(base)) {
        for(auto instPtr: base[blockPtr]->instructions.contents(base)) {
            auto inst = base[instPtr];
            if(inst->kind != LowerInst::Alloca) continue;

            auto address = ((LowerInstSingle*)inst)->created().ptr - base;

            // And one with no use at all, which is the same slot after its writes went some other
            // way: `promoteLoopCells` sinks a contained cell's stores to the loop's exits and needs
            // none where every exit ends the frame, which leaves the slot read by nothing but the
            // preheader's own read - and that is dead once the loop's reads stop needing it. An
            // `alloca` is not removable when unused by the ordinary rule, so it is taken here.
            if(base[address]->uses.isEmpty()) {
                doomed.add(U32(instPtr), 1);
                found = true;
                continue;
            }

            SmallArray<LowerPtr<LowerInst>, 16> gathered;
            if(!onlyWrittenThrough(base, address, gathered, 0)) continue;

            doomed.add(U32(instPtr), 1);
            for(auto write: gathered) doomed.add(U32(write), 1);
            found = true;
        }
    }

    if(!found) return false;

    // Across every block, since a slot is allocated in one and may be written in any. Detaching in
    // whatever order the blocks come is fine: each doomed instruction stops using what it read, and
    // what it read is either another doomed instruction or a value the next sweep round can take.
    for(auto blockPtr: fun.blocks.contents(base)) {
        auto block = base[blockPtr];

        SmallArray<LowerPtr<LowerInst>, 32> kept;
        auto rewrote = false;

        for(auto instPtr: block->instructions.contents(base)) {
            if(doomed.getValue(U32(instPtr))) {
                detach(base, base[instPtr]);
                rewrote = true;
                continue;
            }

            kept.push(instPtr);
        }

        if(!rewrote) continue;

        block->instructions.clear();
        for(auto instPtr: kept) block->instructions.push(arena, instPtr);
    }

    return true;
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
             * alive as long as the phi naming them does. the vector `indexOf` in Core is the shape - the
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

                if(isRemovableWhenUnused(inst) &&
                   ((LowerInstSingle*)inst)->created().ptr->uses.isEmpty()) {
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

        // And the storage only ever written, which a use count cannot see - see the function above.
        if(removeWriteOnlySlots(base, arena, fun)) {
            changed = true;
            dropped = true;
        }
    }

    return dropped;
}
