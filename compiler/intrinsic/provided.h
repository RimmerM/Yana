#pragma once

#include "convert.h"
#include "known.h"

namespace intrinsic {

/*
 * The instances the compiler generates - `provided.def`, where each row is argued.
 *
 * Not a `RoleKind`: nothing claims one of these with an attribute, because the declaration a row is
 * *about* is the class, and the class claims its own `Known` role. What a row adds to that role is
 * "and the compiler supplies the instances at these types", which is a fact the class's declaration
 * cannot state and the compiler's emitters have to.
 */
enum class Provided: U16 {
#define YANA_PROVIDED_ROLE(role, klass, family, heads) role,
#include "provided.def"
#undef YANA_PROVIDED_ROLE
    Count,
};

/*
 * Which types a row generates an instance for.
 *
 * Named sets rather than a list per row, because the sets are what the language states - "every
 * number", "every integer" - and a row that listed widths would be one the target could contradict:
 * which integers exist is `primitive.def`'s, and the stage that generates the instances is what
 * turns a family into that target's handles. Two of the sets name library declarations by role,
 * `boolean` and `ordering`, since the compiler builds values of both and the classes have to be
 * answered for them.
 */
enum class Family: U8 {
    Numbers,     // every integer and every float
    Integers,    // every integer, the target-answered widths included
    Floats,      // the two floats
    Bits,        // every integer, and `Bool`
    Truthy,      // every number, and `Bool`
    Comparable,  // every number, `Bool`, and `Ordering`

    /*
     * No type at all: the heads of this row are **shapes**, supplied by a generator rather than
     * drawn from a list.
     *
     * `Lanewise(Vec(a, n), Mask(a, n))` is the one today. "Every vector" is not an enumerable set -
     * a lane count is the target's and a lane type is any machine primitive - so what stands for it
     * is a single parametric head, which is exactly the arrangement `providePointerInstances`
     * already uses for `%a` and is why those rows are not in this table at all.
     *
     * A row is here anyway when the *emitters* need a `Provided` role to be indexed by, which is
     * what `Lanewise` needs and a pointer's `Eq` does not - `Eq` reuses the row it already has.
     */
    Shapes,
};

/*
 * What the heads of a row's instances are made of - one type of the family, or a pair of them.
 *
 * A column rather than a rule keyed off which class a row names, because the three conversion rows
 * name three classes and differ *only* here: `Widen` and `Narrow` are the same set of ordered pairs
 * split by one predicate, and a generator that recognised `Known::widen` would be that predicate
 * written a second time.
 *
 * The three pairing answers are the three readings of `intrinsic/convert.h`, and nothing here
 * decides which pairs are which - `conversionRung` and `reinterpretable` do, and this column names
 * which of their answers a row collects.
 */
enum class Heads: U8 {
    // One head per type of the family - `Num(I32)`, and every row that is not a conversion.
    Each,

    // One per ordered pair the ladder relates as lossless.
    Widening,

    // One per ordered pair it relates as lossy - the identity pair included, which is what makes
    // `truncate` total over the types it relates. See `conversionRung`.
    Narrowing,

    // One per ordered pair of *distinct* types that are the same bits on every target.
    Reinterpret,
};

struct ProvidedDef {
    // The row's name - the enumerator's own, for dumps and diagnostics. See `YANA_ROLE_TEXT`.
    StringView role;

    // Which class this generates instances of, as a `Known` role rather than a name.
    Known klass;

    Family family;

    Heads heads;
};

extern const ProvidedDef providedTable[Size(Provided::Count)];
inline const ProvidedDef& providedDef(Provided role) { return providedTable[Size(role)]; }

} // namespace intrinsic
