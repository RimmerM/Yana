#pragma once

#include "role.h"

namespace intrinsic {

/*
 * Which family a primitive belongs to - `primitive.def`.
 *
 * Deliberately not a stage's own type kind. A stage builds its own representation out of this
 * answer, and there is more than one stage; a table that handed back `mir::Type*` would be a table
 * only one of them could read.
 */
enum class PrimitiveKind: U8 {
    Unit,
    Bool,
    Int,
    Float,
};

/*
 * Which quantity answers a type's width - the language, or the machine it is compiled for.
 *
 * `Fixed` is every name that states its own width: an `I32` is thirty-two bits wherever it runs, and
 * so is `WideInt`, whose fifty-three are a fact about what stays a machine primitive on every target
 * rather than about any one target.
 *
 * The other two are the names the language deliberately leaves open. `Size` and `USize` are the
 * target's index word - C's `size_t`, existing for the reason C's does - and `CodeUnit` is one unit
 * of its string encoding, a UTF-8 byte natively and a UTF-16 unit on a host whose strings are
 * UTF-16. Both are here rather than settled by the compiler because a *name* for a width is how a
 * thirty-two-bit machine comes to be given a sixty-four-bit index type: the ternary that chose gets
 * asked which host it is on, and never which machine it is emitting for.
 *
 * A stage above the representation layer therefore may not know the answer, and this enum is what it
 * carries instead - see `minBitsOf` and `maxBitsOf` for what it may still say.
 */
enum class TargetWidth: U8 {
    Fixed,
    Word,
    CodeUnit,
};

/*
 * `WideInt`'s width, named because two places state it: the row in `primitive.def` that declares the
 * type, and the accessor that has to recognise the one fixed width with no byte count to index by.
 */
constexpr U16 kWideIntBits = 53;

/*
 * The bound the language states for each width it leaves to the target, and both ends are
 * load-bearing.
 *
 * The low end is what a program may rely on: `let n: Size = 3000000000` has to be a diagnostic
 * rather than a program that works on one machine, so a `Size` promises thirty-two bits and no more.
 * The high end is what nothing may exceed: a type whose maximum is known cannot be packed into a
 * field narrower than it might turn out to be.
 *
 * A word is at least thirty-two bits because that is the narrowest machine this compiler describes,
 * and at most sixty-four because that is the widest integer every target holds. A code unit is eight
 * or sixteen, which is UTF-8 against UTF-16 and not a width class - both arrive in the same register.
 */
constexpr U16 kWordMinBits = 32;
constexpr U16 kWordMaxBits = 64;
constexpr U16 kCodeUnitMinBits = 8;
constexpr U16 kCodeUnitMaxBits = 16;

/*
 * The two readings of a width that hold without knowing the target - the narrowest a value of the
 * type is guaranteed to have, and the widest it may turn out to have.
 *
 * `fixed` is the row's own width and is what both answer for a `Fixed` type, so a caller that has a
 * type in hand asks these rather than testing which kind it is. Which of the two a decision wants is
 * the decision: what a value is promised to hold reads the minimum, and what has to be reserved for
 * one reads the maximum.
 */
constexpr U16 minBitsOf(TargetWidth target, U16 fixed) {
    return target == TargetWidth::Word ? kWordMinBits
         : target == TargetWidth::CodeUnit ? kCodeUnitMinBits : fixed;
}

constexpr U16 maxBitsOf(TargetWidth target, U16 fixed) {
    return target == TargetWidth::Word ? kWordMaxBits
         : target == TargetWidth::CodeUnit ? kCodeUnitMaxBits : fixed;
}

struct PrimitiveDef {
    StringView name;
    PrimitiveKind kind;

    /*
     * The width in bits, and zero both for a kind that has none and for a width the target chooses.
     *
     * Zero rather than the bound in the `target` case, because a row that wrote one would be stating
     * the same fact `minBitsOf`/`maxBitsOf` state and could come to state it differently. What a row
     * says about a target width is which quantity answers it; what the bounds are is one statement,
     * above.
     */
    U16 bits;

    // Meaningful for `Int`, and false everywhere else.
    bool isSigned;

    // Which quantity answers the width - see `TargetWidth`. `Fixed` for everything that states one.
    TargetWidth target;
};

constexpr Size kPrimitiveCount = 0
#define YANA_PRIMITIVE_TYPE(name, kind, bits, isSigned, target) + 1
#include "primitive.def"
#undef YANA_PRIMITIVE_TYPE
;

extern const PrimitiveDef primitiveTable[kPrimitiveCount];

// The primitive a written type name spells, or null where it is not one. The one lookup, so that no
// stage compares a type name against a literal of its own.
const PrimitiveDef* findPrimitive(StringView name);

/*
 * The name an integer of this shape is written with, or empty where nothing spells one.
 *
 * `findPrimitive` read backwards, and here for its reason: a dump and a diagnostic have to call a
 * type what the source calls it, and a stage that spelled `I` plus a width would print `I64` for
 * `Size` - a name for a different type, on a target that may not even hold it. The first matching
 * row wins, which is what makes `Int` and `Long` print as the widths they alias.
 */
StringView primitiveIntName(U16 bits, bool isSigned, TargetWidth target);

} // namespace intrinsic
