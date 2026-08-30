#pragma once

#include "inst.h"
#include "../lower/lower.h"

namespace ssa {

/*
 * The third layer - Analysis-Borrows-Revised.md §1.3.
 *
 * ```text
 * manifest(logical type, ownership contract, target) -> representation
 * ```
 *
 * A manifestation is selected from all three and never from the logical type alone, which is the
 * whole point of the layer: an owned native `String` and a shared window into one are the same
 * logical type with different contracts and different representations, and nothing above this file
 * is allowed to know that. It must not change source legality, generic substitution or instance
 * selection, and it must not *strengthen* a contract - a shared window may not become an owner
 * because the owner representation happens to be the one that exists.
 *
 * At this milestone every type is a scalar and the table is trivial. It is a table rather than a
 * switch inside the lowering walk because the first non-scalar type is where the current compiler's
 * equivalent decision got spread across three files, and one entry point is what keeps the answer
 * in one place. The JS half of the same table belongs to JSIR (Analysis-IR-JS.md §3) and is
 * deliberately not attempted here.
 */
struct Manifestation {
    // What the value is in a register or a slot. `LowerType` is the shared IR's, so a scalar answer
    // needs no translation below this.
    LowerType type;

    U32 size = 0;
    U32 align = 1;

    // Set where the value has no register form and is reached through storage. False for every type
    // this milestone has; it is what an aggregate and an address-only owner will set.
    bool inMemory = false;
};

/*
 * The native answer.
 *
 * Takes the whole occurrence - logical type, root contract and interned interior shape - because all
 * three are inputs to the table, and a signature that took fewer would be one a caller can satisfy
 * without having them. That is not hypothetical: this used to take a type and a contract, and every
 * caller in `lower.cpp` passed `ownedContract()` because it had nothing better in hand, so a shared
 * window would have manifested as its owner. An `ssa::Value` now carries the occurrence, and this is
 * what asks it for one.
 */
Manifestation manifestNative(DeclBase base, const mir::Occurrence& occurrence);

/*
 * Where one field of a record starts, in bytes.
 *
 * The **only** place a field's offset is computed, which is what `InstFieldAddress` naming a field
 * index rather than an offset is for: the current compiler has seven place walks that each derive
 * one and have to agree, and the way that fails is silent - a walk that misses a case reads the
 * wrong bytes rather than failing to build.
 *
 * The layout is the obvious one: fields in declaration order, each on its own alignment, the record
 * on the strongest of them. It is not the packing a `@repr` attribute will ask for, and when one
 * arrives it changes this function and the size beside it, which is the point of them being one
 * walk.
 */
U32 fieldOffsetNative(DeclBase base, DeclPtr<mir::Type> record, U16 field);

/*
 * Whether the value's own bytes live at the address it carries - `Manifestation::inMemory`.
 *
 * Asked directly because the conversion has to know whether reading a place produces a value or an
 * address. True for an owned record; **false for a loan**, which is the distinction below.
 */
bool isAddressOnly(DeclBase base, const mir::Occurrence& occurrence);

/*
 * Whether the value *is* an address - a loan the target carries as a pointer to somebody else's
 * storage.
 *
 * The other half of the question `isAddressOnly` asks, and they are genuinely different: an owned
 * record's bytes are at the address it carries, so installing one copies them; a loan's "bytes" are
 * the pointer, so installing one stores eight. A single flag answered both until a record field held
 * a loan, and then it block-copied the lender's object into a pointer-sized slot.
 *
 * Not every loan is one. A shared, call-bounded loan of a trivially copyable type is a *copy* - the
 * spec copies at that position and `readMode` never produces a loan there - so `x: I32` stays a
 * register, and it is the same rule `manifestNative` reads to decide that.
 */
bool isReferenceValue(DeclBase base, const mir::Occurrence& occurrence);

} // namespace ssa
