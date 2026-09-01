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

struct PrimitiveDef {
    StringView name;
    PrimitiveKind kind;

    // The width in bits, and zero for a kind that has none.
    U16 bits;

    // Meaningful for `Int`, and false everywhere else.
    bool isSigned;
};

constexpr Size kPrimitiveCount = 0
#define YANA_PRIMITIVE_TYPE(name, kind, bits, isSigned) + 1
#include "primitive.def"
#undef YANA_PRIMITIVE_TYPE
;

extern const PrimitiveDef primitiveTable[kPrimitiveCount];

// The primitive a written type name spells, or null where it is not one. The one lookup, so that no
// stage compares a type name against a literal of its own.
const PrimitiveDef* findPrimitive(StringView name);

} // namespace intrinsic
