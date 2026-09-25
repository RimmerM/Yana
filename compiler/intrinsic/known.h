#pragma once

#include "role.h"

namespace intrinsic {

/*
 * The declarations the compiler references - `known.def`.
 *
 * One enumerator per row, in the table's order, because everything that holds an answer for these is
 * an array indexed by it: `mir::Roles::known` is `Count` entries, and a role is a subscript rather
 * than a search.
 */
enum class Known: U16 {
#define YANA_KNOWN_ROLE(role, entity, arity, members, payload, teardown, presence) role,
#include "known.def"
#undef YANA_KNOWN_ROLE

    Count,
};

struct KnownDef {
    // The attribute's spelling - the enumerator's own name. See `YANA_ROLE_TEXT`.
    StringView role;

    Entity entity;

    // How many parameters the head has, or how many arguments a call writes. Zero where the role
    // does not constrain it.
    U16 arity;

    // How many members a class must declare, or how many constructors a record must, and zero
    // where the count is the library's business.
    U16 members;

    // How many fields each constructor of a record carries, where `members` states a count. Zero
    // for a class.
    U16 payload;

    /*
     * Whether the role's member runs because a value died.
     *
     * A column rather than a comparison against a list of enumerators, which is what it replaced -
     * `drop || reclaim` written out in four places, each of which would have had to be found again
     * the day a third ownership class existed. Three things follow from it: a member of the role is a
     * teardown, its subject is consumed rather than borrowed, and source may not call it by name -
     * a teardown runs where the ownership analysis placed it, and finding `drop` by name would let a
     * program trigger a disposal the compiler is answerable for.
     */
    bool teardown;

    Presence presence;
};

extern const KnownDef knownTable[Size(Known::Count)];

inline const KnownDef& knownDef(Known role) { return knownTable[Size(role)]; }

// Whether a role - possibly `Count`, which is "no role" - is one whose member runs at a value's
// death. `Count` answers false, so a caller holding a class's role field needs no test of its own.
inline bool isTeardownRole(Known role) {
    return role != Known::Count && knownDef(role).teardown;
}

} // namespace intrinsic
