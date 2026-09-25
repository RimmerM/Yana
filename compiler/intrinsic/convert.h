#pragma once

#include "primitive.h"

/*
 * Which conversion class relates a pair of numeric primitives - `doc/spec/core.md`'s Conversion and
 * `doc/spec/overloading.md`'s Implicit conversion, and the whole of what the compiler decides here.
 *
 * `Widen`, `Narrow` and `Bitcast` are the library's classes; their instances at the primitive types
 * are the compiler's, because `I32`'s widening cannot be written in terms of anything more basic.
 * *Which* class a pair joins is neither a library fact nor a backend one - it is the language
 * stating what is lossless - so it belongs here beside the widths it is stated over, and not in a
 * resolver.
 *
 * It was written while two resolve stages read it, and that was the reason it existed rather than
 * the reason it was convenient: a fixture was compiled by both and the two answers compared, so two
 * ladders that disagreed by one rung would have meant one program that compiles and one that does
 * not, from one source - the failure the fixity table had before `lib/Core` took it over.
 * `compiler/resolve` went in J173; the ladder stays out of `resolve_mir` for the reason above.
 *
 * ## What is *not* here
 *
 * Nothing target-dependent. A JavaScript build declines the `Bitcast` rungs that cross between a
 * sixty-four-bit integer and a `Double`, because a `bigint` going through one is a heap value on one
 * side and a `number` on the other; that is a *policy* about a target and it stays where the target
 * is known. What is here is the language's question - are these two the same bits on every machine -
 * and a stage above the representation layer may ask it without knowing which machine it is for.
 *
 * Nothing about `@bits` refinements either. A refinement is a type of the resolver's own making,
 * and what it needs is `integerRangeFits` applied to the range it declares - `refinesInteger` in
 * `resolve_mir` is the caller that does so, and it decides the pair *before* asking for a rung,
 * since no rung was ever generated naming one.
 */
namespace intrinsic {

/*
 * One numeric primitive, as the rules below see it.
 *
 * The four columns of `primitive.def` that a conversion depends on, and no handle: a struct holding
 * a stage's own type would be a struct only one stage could build. `bits` is the row's own width and
 * is therefore the *maximum* for a target-answered one - `minBitsOf` and `maxBitsOf` are what read
 * the two ends, and which end a question wants is the question.
 */
struct Numeric {
    PrimitiveKind kind = PrimitiveKind::Int;
    U16 bits = 0;
    bool isSigned = false;
    TargetWidth target = TargetWidth::Fixed;

    bool isFloat() const { return kind == PrimitiveKind::Float; }
};

inline Numeric integerOf(U16 bits, bool isSigned, TargetWidth target = TargetWidth::Fixed) {
    return Numeric { PrimitiveKind::Int, bits, isSigned, target };
}

inline Numeric floatOf(U16 bits) {
    return Numeric { PrimitiveKind::Float, bits, false, TargetWidth::Fixed };
}

/*
 * Whether every value of one integer type is a value of another - which is the whole of what makes a
 * conversion between them lossless, and therefore the one question `Widen` is generated from.
 *
 * Asked as a range containment and not as "is the target wider". Two primitives of the same width
 * and signedness are two names for one set of values - `Long` and `I64`, `Int` and `I32` - and a
 * strict comparison on the widths says the conversion between them loses information. It does not.
 *
 * Unsigned needs its whole width inside the target's positive half; signed needs a signed target at
 * least as wide, since a signed type of any width holds -1 and no unsigned target does.
 *
 * **The two ends of the bound are not the same end**, and that is the load-bearing line. What has to
 * fit is every value the source *may* hold, so the source reads its maximum; what it has to fit into
 * is the capacity the destination is *guaranteed* to have, so the destination reads its minimum. A
 * target-answered width on either side therefore answers conservatively, and the pair is decided
 * once for every machine rather than for the one being compiled for. `let n: Size = someU32` is
 * refused for exactly this reason and the refusal is correct: `Size` promises thirty-two signed
 * bits, which is thirty-one positive ones, and a `U32` needs thirty-two.
 */
inline bool integerRangeFits(Numeric from, Numeric to) {
    auto fromBits = U32(maxBitsOf(from.target, from.bits));
    auto toBits = U32(minBitsOf(to.target, to.bits));

    if(from.isSigned) return to.isSigned && toBits >= fromBits;
    return toBits - (to.isSigned ? 1u : 0u) >= fromBits;
}

/*
 * Which of the two conversion classes relates an ordered pair, or neither.
 *
 * One function rather than a `widens` predicate and a rule about what to do when it says no, because
 * "neither" is a third answer and a predicate cannot give it: an integer that does not cross to a
 * float does not thereby *narrow* into one, and generating a `Narrow` rung for it would make
 * `truncate(x) :: F32` compile at a width where nobody decided what it means.
 *
 * The pair `(a, a)` is `Narrow`, and that is deliberate: taking the low bits of a value at its own
 * width is the identity, and the rung exists so that `truncate` is **total** over the types it
 * relates. `Size` is `I64` natively and `Int` on a host whose word is thirty-two bits, so
 * `truncate(length(xs)) :: Int` is a real truncation on one target and the identity on the other -
 * and without the identity rung there is no portable spelling of "this length is an `Int` now".
 * Neither `::` nor an implicit position ever selects one, because both answer `sameType` and return
 * before an instance is looked for.
 *
 * ## The crossing set is the 32- and 64-bit integer widths
 *
 * An integer reaches a float only at `I32`/`U32` and `I64`/`U64`, and a float reaches an integer
 * only into those widths. The set is stated rather than derived, and it is worth saying why it is
 * not simply "every integer whose magnitude fits the float's mantissa": `I32` into `F32` is in this
 * set and is *not* exact past 2^24, so the derived rule would refuse a conversion the language has
 * admitted since before the eight fixed widths existed. The unsigned counterparts use the same
 * width-based rule, which makes `U32` to `Double` direct and keeps signedness from creating a second
 * conversion ladder. The narrower widths still have no direct float rung and widen to `I32` first.
 */
enum class Rung: U8 {
    None,
    Widen,
    Narrow,
};

// The integer widths a float rung is written at - see the note above, which is where the set is argued.
inline bool crossesToFloat(Numeric integer) {
    return integer.kind == PrimitiveKind::Int && integer.target == TargetWidth::Fixed &&
           (integer.bits == 32 || integer.bits == 64);
}

// Whether two rows describe one type - the four columns, since that is the whole of what a
// `Numeric` is. Used for the identity rung and nowhere else; a stage compares its own handles.
inline bool sameNumeric(Numeric a, Numeric b) {
    return a.kind == b.kind && a.bits == b.bits && a.isSigned == b.isSigned && a.target == b.target;
}

inline Rung conversionRung(Numeric from, Numeric to) {
    /*
     * The identity, which is `Narrow` and not `Widen` - see the note above. Stated before anything
     * else because the range rule would answer `Widen` for it: every value of a type is of course a
     * value of that same type, and a `Widen` rung there would be an implicit conversion that never
     * fires - both `::` and an implicit position answer `sameType` first - standing where the rung
     * that makes `truncate` total has to be.
     */
    if(sameNumeric(from, to)) return Rung::Narrow;

    if(from.isFloat() && to.isFloat()) {
        return from.bits < to.bits ? Rung::Widen : Rung::Narrow;
    }

    // A float in exactly one position - the crossing rungs, whose set is `crossesToFloat`.
    if(from.isFloat()) return crossesToFloat(to) ? Rung::Narrow : Rung::None;
    if(to.isFloat()) return crossesToFloat(from) ? Rung::Widen : Rung::None;

    return integerRangeFits(from, to) ? Rung::Widen : Rung::Narrow;
}

/*
 * Whether two primitives are the same bits on **every** target - the one question `Bitcast` is
 * generated from, and the whole of its safety argument.
 *
 * Same width is all of it, so nothing downstream has to ask whether a `bitcast` fits: no instance
 * relating a pair that does not was ever made. What that costs is stated rather than worked around
 * in each caller:
 *
 *  - **A target-answered width pairs with the same target quantity and with nothing else.** `Size`
 *    is not a number here, so it cannot be compared against one; what it *can* be compared against
 *    is `USize`, which is the same word read the other way and is therefore the same width on every
 *    machine by construction. That pair is the one a bounds test is written with.
 *  - **`WideInt` has no partner.** Fifty-three bits in a sixty-four-bit register is a shape nothing
 *    else has, and a reinterpretation of it would be a question about the other eleven bits that
 *    nothing here can answer.
 *  - **`CodeUnit` has no partner either**, and that costs nothing: nothing reinterprets an encoding
 *    unit.
 *
 * An address is the other pair a program wants, and it is declared where the address is - the
 * pointer rungs relate `%a` to `Size`, and are sound on every target by construction rather than by
 * both sides happening to be sixty-four bits.
 */
inline bool reinterpretable(Numeric from, Numeric to) {
    if(from.target != to.target) return false;
    if(from.target != TargetWidth::Fixed) return true;

    return from.bits == to.bits;
}

} // namespace intrinsic
