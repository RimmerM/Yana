#include "lower_inst.h"

/*
 * The instruction table - inst.def, read as data.
 *
 * The `size` column is `sizeof` the row's own struct rather than a number written by hand, which is
 * the whole reason the `Struct` column exists: a pass that copies an instruction wholesale reads the
 * size from the same row that says the kind may be copied at all, so the two cannot disagree. A
 * variadic kind answers 0, since its allocation continues past the struct.
 *
 * The static assertions below are what keeps a row honest. Two of them are about the roster - one
 * kind, one row, and every struct at least as large as the header it extends - one is about the
 * flags: `kLowerRepeatable` is a stronger claim than `kLowerPure` and every row that makes it has to
 * make the weaker one as well, which is what lets a caller ask either without asking both - and two
 * hold the arity columns to what the fixed operand and result buffers can be, so a row that meant
 * `kAnyArity` and wrote a number is caught here rather than by a verifier failure on the one
 * program that has more.
 */
constexpr LowerInstTraits kLowerInstTraits[] = {
#define YANA_LOWER_INST(kind, Struct, mnemonic, used, created, flags) \
    LowerInstTraits { mnemonic, (flags) & kLowerVariadic ? U16(0) : U16(sizeof(Struct)), flags, U8(used), U8(created) },
#include "inst.def"
#undef YANA_LOWER_INST
};

static_assert(sizeof(kLowerInstTraits) / sizeof(LowerInstTraits) == LowerInst::kKindCount,
              "every LowerInst::Kind has exactly one row in inst.def");

#define YANA_LOWER_INST(kind, Struct, mnemonic, used, created, flags) \
    static_assert(sizeof(Struct) >= sizeof(LowerInst), "a row's struct is a LowerInst"); \
    static_assert(!((flags) & kLowerRepeatable) || ((flags) & kLowerPure), \
                  "a repeatable kind is a pure kind"); \
    static_assert(U8(used) == kAnyArity || U8(used) <= 3, "a fixed operand count is small"); \
    static_assert(U8(created) == kAnyArity || U8(created) <= 2, "a fixed result count is small");
#include "inst.def"
#undef YANA_LOWER_INST

/*
 * The families, held to what they claim - see the range markers below the generated enum.
 *
 * A marker is an offset into the row order, and inst.def's rule that a row may be appended to a
 * family and never moved between two is the whole of what keeps `isUnary` and `isBinary` honest.
 * That rule was prose. Here it is a compile error: a row put in the wrong family is a row whose
 * arity or flags do not match the family it landed in, which is what these ask.
 *
 * They do not catch every reordering - a `Sub` and a `Mul` exchanged are two binary rows still - and
 * nothing here needs them to. What is being caught is a row of a different *shape* landing inside a
 * range, which is the reordering that turns `isBinary` into a lie and casts a one-operand
 * instruction to `LowerInstBinary` somewhere far away.
 */
static constexpr bool familyArity(LowerInst::Kind first, LowerInst::Kind last, U8 used, U8 created) {
    for(Size i = Size(first); i <= Size(last); i++) {
        if(kLowerInstTraits[i].used != used || kLowerInstTraits[i].created != created) return false;
    }

    return true;
}

static constexpr bool familyCreates(LowerInst::Kind first, LowerInst::Kind last, U8 created) {
    for(Size i = Size(first); i <= Size(last); i++) {
        if(kLowerInstTraits[i].created != created) return false;
    }

    return true;
}

static constexpr bool familyFlags(LowerInst::Kind first, LowerInst::Kind last, U16 flags) {
    for(Size i = Size(first); i <= Size(last); i++) {
        if((kLowerInstTraits[i].flags & flags) != flags) return false;
    }

    return true;
}

static_assert(familyArity(LowerInst::FirstUnary, LowerInst::LastUnary, 1, 1),
              "every kind in the unary range reads one operand and defines one value");
static_assert(familyArity(LowerInst::FirstBinary, LowerInst::LastBinary, 2, 1),
              "every kind in the binary range reads two operands and defines one value");
static_assert(familyCreates(LowerInst::FirstVector, LowerInst::LastVector, 1),
              "every kind in the vector range defines one value");
static_assert(familyCreates(LowerInst::FirstTerminator, LowerInst::LastTerminator, 0),
              "a terminator defines nothing another instruction can name");
static_assert(familyFlags(LowerInst::FirstAtomic, LowerInst::LastAtomic, kLowerOrdered),
              "every kind in the atomic range orders - see isAtomicInst");

// And the ranges themselves, in the order the rows are in and not overlapping.
static_assert(LowerInst::FirstInst < LowerInst::FirstUnary &&
              LowerInst::FirstUnary <= LowerInst::FirstUnaryArith &&
              LowerInst::FirstUnaryArith <= LowerInst::LastUnaryArith &&
              LowerInst::LastUnaryArith <= LowerInst::LastUnary &&
              LowerInst::LastUnary < LowerInst::FirstBinary &&
              LowerInst::FirstBinary < LowerInst::LastBinary &&
              LowerInst::LastBinary < LowerInst::FirstVector &&
              LowerInst::FirstVector < LowerInst::LastVector &&
              LowerInst::LastVector < LowerInst::FirstAtomic &&
              LowerInst::FirstAtomic < LowerInst::LastAtomic &&
              LowerInst::LastAtomic < LowerInst::FirstTerminator &&
              LowerInst::FirstTerminator < LowerInst::LastTerminator &&
              LowerInst::LastTerminator < LowerInst::LastInst,
              "the family markers are in row order and do not overlap");

static_assert(Size(LowerInst::LastInst) + 1 == LowerInst::kKindCount,
              "LastInst is the last row in inst.def");
