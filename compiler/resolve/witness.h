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
 * The layout of a runtime `GenEnv`.
 *
 * One immutable realization of a GenSchema: the schema it was built for, then one pointer per slot
 * in the canonical numbering. Emitted code loads slot N from `kSlots + 8 * N` and nothing else -
 * Implementation-Generics.md part 1's "no runtime name lookup" is exactly this.
 *
 * The leading schema word is not read by emitted code. It is what a debug build compares against
 * the callee's own schema, which is the only check that can catch a caller and a callee disagreeing
 * about what slot 3 means.
 */
namespace GenEnvLayout {
    static constexpr U32 kSchema = 0;
    static constexpr U32 kSlots = 8;
    static constexpr U32 kSlotSize = 8;
    static constexpr U32 kAlign = 8;

    static constexpr U32 slotOffset(U16 slot) { return kSlots + kSlotSize * slot; }
    static constexpr U32 sizeFor(Size slotCount) { return kSlots + kSlotSize * U32(slotCount); }
}

/*
 * The layout of a `ClassWitness` - Implementation-Generics.md part 6.
 *
 * One immutable method table per class implementation: the class it is for, the descriptors of the
 * types it was selected at, and one erased entry point per class function. A generic body that
 * deferred a dispatch loads the witness out of its own environment and the method out of the
 * witness, both at numbers the schema fixed at compile time.
 *
 * The method slots hold plain code addresses rather than full `FunctionWitness` records. A witness
 * carries a closure and a captured environment, and a class method has neither: it is a known
 * function reached through a known table. Constrained function *values* are what need the wider
 * shape, and they need function values first.
 */
namespace ClassWitnessLayout {
    // The class this implements, as its region offset - the same kind of identity a TypeDesc's
    // logical type is, and used for the same debug validation.
    static constexpr U32 kClass = 0;
    static constexpr U32 kArgCount = 4;
    static constexpr U32 kMethodCount = 6;
    static constexpr U32 kArgs = 8;

    static constexpr U32 methodsOffset(U16 argCount) { return kArgs + 8 * argCount; }
    static constexpr U32 sizeFor(U16 argCount, U16 methodCount) {
        return methodsOffset(argCount) + 8 * methodCount;
    }
}

/*
 * The witness for one class implementation, interned per class and argument list.
 *
 * Null after reporting when no instance serves these types, or when one of its methods cannot be
 * given an erased entry point. A call site that gets null specializes instead.
 */
ModulePtr<Global> classWitnessFor(Module& module, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args,
                                  LocationId source);

/*
 * The environment one generic function needs when called with these type arguments.
 *
 * This is Implementation-Generics.md part 9's first case - "all entries concrete; reference an
 * interned global constant" - and it is the only one the erased path takes today. A forwarded or
 * mixed environment is what a generic body calling another generic function needs, and it wants
 * slots this caller does not have; until that lands, such a call is specialized instead.
 *
 * Null after reporting when a slot cannot be supplied. The diagnostic names the source constraint
 * rather than the slot, per part 12: a schema slot is not a thing anyone wrote.
 */
ModulePtr<Global> genEnvFor(Module& module, ModulePtr<Function> callee, Buffer<TypePtr> args,
                            LocationId source);

/*
 * Fills in every generic call site's environment, once the whole program has been resolved.
 *
 * Deliberately not done where the calls are emitted. A slot number is derived from the finished
 * context, and a body collects requirements *while* it is being resolved - so a plan computed at
 * the call site would be numbered against a context that had not stopped growing. Running once at
 * the end is what makes the caller's numbering and the callee's the same numbering.
 *
 * Reports and returns false when a call that has to be emitted cannot be supplied. A call that can
 * still be specialized is left alone.
 */
bool prepareGenericCalls(Program& program);

/*
 * Whether this generic body can be emitted as machine code at all, rather than only cloned.
 *
 * A body is lowerable when every decision left in it can be made from the environment its caller
 * passes. Two things are not, yet, and both are Implementation-Generics.md part 9's forwarded and
 * mixed environments:
 *
 *  - a call to another generic function, whose environment would have to be built from *this*
 *    function's slots rather than from concrete types;
 *  - a class dispatch the body deferred, which needs the class witness that environment would hold.
 *
 * A call site that gets `false` specializes instead, which is always available for a concrete
 * argument list. That is what keeps the erased path a staged optimization rather than a cliff.
 */
bool genericBodyLowerable(Module& module, ModulePtr<Function> function);

/*
 * `moveInit(dst, src)`: initialize uninitialized `dst` from an owned `src`, leaving `src` dead.
 *
 * A block copy for a TrivialSink type, and a call to the authored `Sink` for anything else.
 * Generated as a real function rather than as a flag the caller interprets, because the caller is
 * generic code that does not know the size - which is exactly the thing the descriptor exists to
 * carry. Null where relocation is a copy of nothing.
 */
ModulePtr<Function> moveInitFor(Module& module, TypePtr type, LocationId source);
