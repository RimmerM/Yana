#pragma once

#include "role.h"

namespace intrinsic {

/*
 * Which parameterized type the compiler creates - `constructor.def`.
 *
 * `PrimitiveKind`'s sibling, and not a stage's own type kind for its reason: a stage builds its own
 * representation out of this answer and there is more than one stage. `Vector` and `Mask` are two
 * rows here although a stage may well hold them as one kind with a flag - what the table says is
 * which *name* was written, and how a stage represents the two is its own business.
 */
enum class ConstructorKind: U8 {
#define YANA_TYPE_CONSTRUCTOR(kind, name, least, most) kind,
#include "constructor.def"
#undef YANA_TYPE_CONSTRUCTOR

    Count,
};

struct ConstructorDef {
    ConstructorKind kind;
    StringView name;

    // How many type arguments the name accepts - see the note on the range in `constructor.def`.
    U8 least;
    U8 most;
};

extern const ConstructorDef constructorTable[Size(ConstructorKind::Count)];

// The constructor a written type name spells, or null where it is not one. `findPrimitive`'s
// sibling, and the one lookup for these three names, so that no stage compares a type name against
// a literal of its own.
const ConstructorDef* findConstructor(StringView name);

// The row for a kind, which is the same table read the other way - a stage that has built a vector
// and wants to print it back needs the name it was written with.
const ConstructorDef& constructorOf(ConstructorKind kind);

} // namespace intrinsic
