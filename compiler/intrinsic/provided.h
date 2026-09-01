#pragma once

#include "known.h"

namespace intrinsic {

/*
 * The instances the compiler generates - `provided.def`, which is empty until the numeric tower
 * arrives with the rest of `lib/Core`.
 */
enum class Provided: U16 {
#define YANA_PROVIDED_ROLE(role, klass) role,
#include "provided.def"
#undef YANA_PROVIDED_ROLE

    Count,
};

struct ProvidedDef {
    // The attribute's spelling - the enumerator's own name. See `YANA_ROLE_TEXT`.
    StringView role;

    // Which class this generates instances of, as a `Known` role rather than a name.
    Known klass;
};

// Deliberately not `extern const ProvidedDef providedTable[Size(Provided::Count)]`: a zero-length
// array is not a type C++ has, and the table is reached through `providedDef` which asserts anyway.
const ProvidedDef& providedDef(Provided role);

} // namespace intrinsic
