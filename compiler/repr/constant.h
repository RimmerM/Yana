#pragma once

#include "repr.h"
#include "../resolve/const.h"

/*
 * A source-level constant, as the bytes a target holds it in.
 *
 * The other half of the ruling `ConstValue` is built on. Resolve records *what the value is* - a
 * tree of scalars, constructors, elements and addresses - and this is where one target turns that
 * into storage, using exactly the `Repr` it lays every other value of the same type out with. A
 * second layout rule here would be a second answer to the question repr.cpp owns, which is the thing
 * the tree exists to avoid.
 *
 * It sits beside table.cpp for the same reason table.cpp is here: a slot has a position only once
 * some target has said how wide an address is and what it must be aligned to. What differs between
 * the two is whose layout is being followed - a compiler-built table has one of its own, and a source
 * constant has its type's - and that is the whole difference.
 *
 * The JS target never includes this. A host value has no bytes to write; `constantValue` in
 * codegen/js/type.cpp is the same walk producing a JS expression instead, which is why it is the
 * *walk* that is duplicated and not the layout.
 */

/*
 * An address inside a materialized constant, and what it names.
 *
 * The same shape a table's relocation has and translated by `lowerProgram` the same way: what a
 * global's bytes hold is a zero until the module is placed, and this says which symbol goes there.
 */
struct ConstRelocation {
    U32 offset = 0;
    ModulePtr<Global> global = nullptr;
};

/*
 * Writes `constant` into `bytes`, which the caller allocated at the size of the constant's type and
 * zeroed, and appends one entry to `relocations` per address it contains.
 *
 * False where the constant cannot be written as bytes at all, which is one thing: a payload or a
 * field reached through an *indirection* - `Field::boxed`, `Constructor::boxed` - since what would
 * go there is the address of storage nobody has allocated. `declareGlobal` refuses one in front of
 * this, so reaching it here is a compiler bug rather than a program's; the answer exists so that the
 * bug is a missing global rather than a wrong one.
 */
bool materializeConstant(ReprTable& repr, ModuleBase local, ModulePtr<ConstValue> constant,
                         ByteBuffer bytes, Array<ConstRelocation>& relocations);
