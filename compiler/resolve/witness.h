#pragma once

#include "module.h"

/*
 * The runtime half of the generic model - Implementation-Generics.md parts 3 to 6.
 *
 * A generic body that has not been specialized does not know what its type variables are, so
 * everything it needs to know about them travels with the call as data. This file is where that
 * data is built: one immutable constant per type, per class implementation and per constrained
 * field, interned per program and emitted as ordinary module-level constants with relocations for
 * the addresses they hold.
 *
 * Two rules shape all of it, both from Implementation-Generics.md part 1:
 *
 *  - **Every operation is authorized by the schema.** Knowing a type's size does not grant `Copy`,
 *    `Eq`, construction, a field, or a class method. A descriptor carries what the *language* needs
 *    to move and release a value at all; anything a program could have written differently is a
 *    separate witness with its own constraint entry.
 *  - **The resolver decides whether a body may use a fact.** A TypeDesc says whether its type is
 *    TrivialCopy because lowering has to know how to relocate it; that flag never lets a body emit
 *    a bitwise copy its own signature did not ask for.
 */

/*
 * The layout of a TypeDesc, as the emitted bytes have it.
 *
 * Written out as offsets rather than as a C++ struct because there are two readers with different
 * ideas of the word: this compiler, which writes the bytes, and the generic code it emits, which
 * reads them through ordinary loads at these offsets. A struct would describe the first and leave
 * the second to a second, unchecked copy of the same numbers.
 */
namespace TypeDescLayout {
    // The interned type this describes, as its region offset. Stable within one program, which is
    // all that identity is needed for: validating a context against its schema, interning, and
    // specialization keys.
    static constexpr U32 kLogicalType = 0;

    static constexpr U32 kSize = 4;
    static constexpr U32 kAlign = 8;

    // What indexing homogeneous storage advances by. `alignUp(size, align)` today, and explicit
    // because a packed or target-specific array stride may differ from it later.
    static constexpr U32 kStride = 12;

    static constexpr U32 kFlags = 16;

    // The three lifecycle operations, each a code address or null. See TypeDescFlags for what null
    // means in each case - it is "nothing to do", never "unavailable".
    static constexpr U32 kMoveInit = 24;
    static constexpr U32 kReclaim = 32;
    static constexpr U32 kDrop = 40;

    static constexpr U32 kSize_ = 48;
    static constexpr U32 kAlign_ = 8;
}

/*
 * The structural facts a generic body may need about a type it cannot see.
 *
 * These are the *already-resolved* answers, not permissions. Design-Memory §2.1 is what makes that
 * distinction load-bearing: a body compiled against an unconstrained `a` treats it as
 * non-TrivialCopy however these bits come out at one call site, and only a declared constraint
 * changes what the body does.
 */
enum class TypeDescFlags: U32 {
    None = 0,
    TrivialCopy = 1 << 0,
    TrivialSink = 1 << 1,

    // Two bits each, holding a TeardownKind. Read by the generic release loop that has to know
    // whether a per-element teardown exists at all before it walks a buffer.
    ReclaimShift = 2,
    DropShift = 4,

    // The selected Repr requires the value to keep its address, so relocating it is not a move.
    NeedsStableAddress = 1 << 6,

    // This is the canonical representation used at an unspecialized boundary, rather than a variant
    // selected for one owner. Everything is today; the bit exists so that a Repr variant crossing a
    // generic boundary is a decision rather than an accident.
    CanonicalRepr = 1 << 7,
};

inline U32 typeDescFlags(const Ownership& ownership, bool needsStableAddress) {
    U32 flags = U32(TypeDescFlags::CanonicalRepr);
    if(ownership.trivialCopy) flags |= U32(TypeDescFlags::TrivialCopy);
    if(ownership.trivialSink) flags |= U32(TypeDescFlags::TrivialSink);
    if(needsStableAddress) flags |= U32(TypeDescFlags::NeedsStableAddress);

    flags |= U32(ownership.reclaim) << U32(TypeDescFlags::ReclaimShift);
    flags |= U32(ownership.drop) << U32(TypeDescFlags::DropShift);
    return flags;
}

/*
 * The descriptor for one fully concrete type, interned per program.
 *
 * Built in the module that asked for it, for the same reason teardown glue is: the lifecycle
 * operations it points at are found by instance lookup, and instance lookup is relative to the
 * module doing the looking. Interning is still program-wide, which instance coherence is what makes
 * sound.
 *
 * Null for a type that is not concrete. A generic body never builds a descriptor for its own type
 * variables - it is handed theirs.
 */
ModulePtr<Global> typeDescFor(Module& module, TypePtr type, LocationId source);

/*
 * `moveInit(dst, src)`: initialize uninitialized `dst` from an owned `src`, leaving `src` dead.
 *
 * A block copy for a TrivialSink type, and a call to the authored `Sink` for anything else.
 * Generated as a real function rather than as a flag the caller interprets, because the caller is
 * generic code that does not know the size - which is exactly the thing the descriptor exists to
 * carry. Null where relocation is a copy of nothing.
 */
ModulePtr<Function> moveInitFor(Module& module, TypePtr type, LocationId source);
