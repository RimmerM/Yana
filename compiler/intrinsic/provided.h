#pragma once

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
#define YANA_PROVIDED_ROLE(role, klass, family) role,
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
};

struct ProvidedDef {
    // The row's name - the enumerator's own, for dumps and diagnostics. See `YANA_ROLE_TEXT`.
    StringView role;

    // Which class this generates instances of, as a `Known` role rather than a name.
    Known klass;

    Family family;
};

extern const ProvidedDef providedTable[Size(Provided::Count)];
inline const ProvidedDef& providedDef(Provided role) { return providedTable[Size(role)]; }

} // namespace intrinsic
