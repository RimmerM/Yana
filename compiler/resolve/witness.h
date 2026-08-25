#pragma once

#include "module.h"
#include "Net/Buffer.h"

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
 * The slot numberings.
 *
 * A table is a list of TableSlots - see TableCell in module.h - and these are the positions in that
 * list. No byte offsets: where a slot lands is decided by whichever backend is about to emit it,
 * because that is the only place the answer exists. repr/table.h is the native answer.
 */

/*
 * The slots of a TypeDesc, of which there are two sets - one per target.
 *
 * Read only through whatever its target's materialization made of these positions - a word by byte
 * offset, an address through InstTableSlot. There is no second description: a tuple laid out like a
 * descriptor used to sit beside this one so that the typed IR could project into it, and it went
 * away when a slot stopped being a field of anything.
 *
 * **The two layouts share nothing, deliberately.** They used to share a prefix and overlap in the
 * tail, on the reading that a size is a size everywhere; it is not. Every question a descriptor
 * answers is a question about a *representation*, and the two targets do not have one in common -
 * so what each holds is what its own backend loads, in its own order, and a slot one target does
 * not read is a per-type function that target does not generate. A resolved program belongs to one
 * target (`@platform` selects declarations during resolution, and Program::optimized says out loud
 * that a second target's request would be answered with the first's IR), so no image ever contains
 * both, and each name below is used only by the backend it belongs to. `writeTypeDesc` is the
 * single writer and branches once, which is where to look if that ever stops being true.
 */

/*
 * What a machine target's descriptor holds: three measurements and one teardown.
 */
namespace NativeTypeDesc {
    /*
     * No leading identity word.
     *
     * This used to start with the interned type as its region offset, for a debug check that
     * compares a context against its schema - and that check was never written, so every descriptor
     * in every emitted program carried a compiler arena handle nothing loaded. Identity is what
     * *interning* is for here: one descriptor per type, so the address of the descriptor is the
     * identity, and a reader holding one already knows which it holds. If a debug check does get
     * built it wants a content-derived id, which an arena offset is not - see TableSlot.
     */
    static constexpr U16 kSize = 0;

    // What indexing homogeneous storage advances by. `alignUp(size, align)` today, and explicit
    // because a packed or target-specific array stride may differ from it later - which is the one
    // reason it is not derived from the two numbers beside it.
    static constexpr U16 kStride = 1;

    /*
     * The flags, and the alignment above them.
     *
     * Alignment had a word of its own and did not need one: TypeDescFlags uses bits 0 to 7 and the
     * remaining 24 were spare, so the two share a cell and a descriptor is one word smaller. That is
     * worth having only because of what an address slot now costs - four words used to round up to
     * the same sixteen bytes as three, so removing one bought nothing until the addresses stopped
     * being eight bytes wide.
     *
     * The alignment is stored as itself rather than as a log, so reading it is one shift and no
     * table. 24 bits is every alignment any target has.
     */
    static constexpr U16 kFlags = 2;
    static constexpr U32 kAlignShift = 8;

    // Which bits of that cell are the flags, for a reader that wants them without the alignment.
    static constexpr U32 kFlagMask = (1u << kAlignShift) - 1;

    /*
     * **Both halves of the teardown, as one function.**
     *
     * A concrete drop site names the two halves separately, because it can see which of them a
     * region discharged and which of them this type has at all. Erased code can do neither: it
     * holds an address and a descriptor, the descriptor's flags are not a thing a call may branch
     * on for free, and the two halves are frequently *the same function* - a container writes one
     * walk over its live elements and supplies both from it (Implementation-Containers.md §13), so
     * two slots called in turn would release every element twice.
     *
     * Two slots and a guard would answer that, and the guard is exactly what a descriptor exists to
     * avoid: the equality is known once, per type, at the point the table is written, and paying
     * for it again at every erased drop asks the machine to rediscover a compile-time fact. So the
     * merge happens where the fact lives - see `teardownBothFor`, which answers the shared function
     * itself wherever there is one and generates a two-call wrapper only where the halves genuinely
     * differ.
     *
     * **The region case does not want a second slot; it wants a second descriptor.** A region
     * discharges the reclaim half of everything inside it in bulk and leaves the drop half to run at
     * last use, and the first reading of that was "one slot cannot express it, so put the drop half
     * beside this one". It is the wrong reading. Whether a value is region-allocated is not
     * something the *drop site* discovers - an erased body cannot discover it at all - it is
     * something the site that builds the environment knows, and that site is already choosing which
     * descriptor to name. So it names one whose teardown slot is the drop half alone, and the
     * discharged and undischarged paths are one call to two different addresses.
     *
     * That costs at most a second interned descriptor per type, keyed by provenance rather than by
     * type alone - which is exactly what `closureReleaseFor` already does for a frame environment
     * against a heap one. `genEnvFor` interns on `(callee, type arguments)` and goes on doing so;
     * only the descriptor it names differs.
     *
     * A code address and never null. See TypeDescFlags for what the empty case is - it is "nothing
     * to do", never "unavailable", so erased code calls unconditionally and never tests first.
     */
    static constexpr U16 kTeardown = 3;

    static constexpr U16 kCount = 4;
}

/*
 * What a managed target's descriptor holds: the two operations whose *shape* it cannot reconstruct,
 * and the effect at last use.
 *
 * The three measurements are absent, and that is the difference between the targets rather than an
 * economy. A byte count is not a fact about a JS value - the runtime decides what an object costs,
 * and nothing this compiler emits can observe it - so `sizeOf` and `alignOf` are `@platform(native)`
 * and there is no question left for a size cell to answer. What that leaves is `kMoveInit` and
 * `kCopyInit`, which native performs inline from `kSize` (see `relocateWith` and the `Copy` case in
 * resolve/lower_mem.cpp): a managed block copy is property by property, so what an unknown type
 * withholds *here* is the shape, and no number reconstructs one.
 *
 * Reclaim is absent for the older reason: the collector releases storage, so there is no such half
 * to run and `kDrop` is the whole of a teardown here - which is also why this target needs no merged
 * slot the way the native one does.
 */
namespace ManagedTypeDesc {
    static constexpr U16 kDrop = 0;
    static constexpr U16 kMoveInit = 1;
    static constexpr U16 kCopyInit = 2;

    static constexpr U16 kCount = 3;
}

// A descriptor has no tuple form. It used to have one, so that a place rooted in a descriptor
// address could read a word the way any other aggregate is read; nothing ever did, and a slot is no
// longer a field of anything - see InstTableSlot.

// The type of one word of a function value - see FunValueLayout. Both are bare addresses: what is
// behind the code word is not this compiler's business, and what is behind the environment word is
// known only to the closure header the code word leads to.
TypePtr funValueFieldType(Module& module, U16 field);

/*
 * What a closure's teardown runs, emitted as static data immediately in front of the lifted
 * function's entry point.
 *
 * A function value is `{code, env}`, and neither word says what is in the environment. The
 * environment's descriptor used to be a third word; it does not have to be a word of the value at
 * all, because what a closure captured is decided by the lambda it came from and the code word
 * already names the lambda. So the answer lives at a negative offset from the entry point -
 * `[code - kSize_]` - and every function value is one word narrower for it.
 *
 * Where it lives is a *target* decision, and the only one the two targets make differently about a
 * lambda: a code word that is not an address has nothing in front of it to subtract from, so a
 * target may keep this table anywhere it likes and answer FunValueLayout::kHeader with it instead.
 * What is in the table, and what reads it, is the same either way.
 *
 * The two slots hold plain code addresses rather than a descriptor, for the same reason a class
 * witness's method slots do: everything a descriptor carries beyond them - the size, the alignment,
 * the flags - is about a type nothing here has to reason about, and the extra indirection would be
 * paid at every teardown to reach the two words that matter. Holding the addresses also makes them
 * the *closure's* halves rather than the environment type's, which is what lets a heap environment's
 * reclaim be "release the captures and hand the storage back" while a frame environment's is only
 * the first half. Where the environment lives is a compile-time constant per lifted lambda - one
 * lambda expression is one allocation site - so it is spent here, in which function this slot names,
 * instead of travelling to a shared teardown as a bit for it to test.
 *
 * Both slots are always callable, never null, so nothing that reaches them has to test one first -
 * see emptyTeardown, which is the same rule a TypeDesc's lifecycle slots follow.
 *
 * "Immediately in front of the entry point" is a real constraint on the backend rather than a
 * convention: what the emitted teardown subtracts is the header's own size, so a code generator that
 * pads between the header and the first instruction breaks it. AsmModule::startFunction is where
 * that is honoured, and it is why LowerFunction carries the header rather than the module's global
 * list. How far back that is is deliberately not a constant here - the teardown asks for the size of
 * closureHeaderPlaceType() as a TypeMetric and lets the target fold it, because a target that laid
 * two addresses out differently would otherwise have this compiler subtracting somebody else's
 * number.
 *
 * Only a lambda that captured something has one. A non-capturing lambda and the thunk that makes a
 * named function a value have a null environment, so their teardown never reaches for a header and
 * none is emitted.
 */
namespace ClosureHeaderFields {
    /*
     * One slot, holding what this target's teardown of the environment is.
     *
     * Natively that is both halves - release the captures, and hand back the storage under them
     * where the environment is heap-placed. On a managed target it is the drop half alone, because
     * the collector owns the storage. Which of those the slot holds is decided once, by
     * `closureHeaderFor`, in exactly the way `teardownAtSite` decides it for an ordinary drop.
     *
     * It used to be two - a drop slot and a reclaim slot - and everything that touched them had to
     * ask whether they named the same function first, because a captured container supplies both
     * halves from one walk. `devirtualizeClosureDrop` is the site that shows what that cost: it had
     * to *carry the pairing across* a rewrite rather than recompute it, with a comment explaining
     * that rewriting the two independently would run the walk twice. See NativeTypeDesc::kTeardown,
     * which is the same collapse for the same reason.
     *
     * Always callable, never null, so nothing that reaches it has to test one first - see
     * emptyTeardown.
     */
    static constexpr U16 kTeardown = 0;

    static constexpr U16 kCount = 1;
}

TypePtr closureHeaderPlaceType(Module& module);

/*
 * The header for one lifted lambda, built once and attached to the function it belongs in front of.
 *
 * Built when the closure is, with the reclaim slot naming the environment type's own - which is the
 * answer for an environment that lives in the frame, and the one that is safe to be wrong about in
 * the direction of doing too little. selectStorage replaces it where the environment turned out to
 * need the heap, because that is the pass that decides and nothing before it knows.
 */
ModulePtr<Global> closureHeaderFor(Module& module, ModulePtr<Function> lambda, TypePtr envType,
                                   LocationId source);

// The reclaim of a closure whose environment is heap-placed: the environment type's own, and then
// the storage. Interned per environment type - what it does depends on the type and on nothing
// about the lambda - and generated only where a closure actually needs one.
ModulePtr<Function> closureReleaseFor(Module& module, TypePtr envType, LocationId source);

// Points an existing header's reclaim slot at another function. Called from selectStorage with the
// result of closureReleaseFor, which is the one thing about a header that is not decided by the
// time the closure is built.
void setClosureRelease(Module& module, ModulePtr<Global> header, ModulePtr<Function> reclaim);

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

// The flags alone. Packed with the alignment into one cell by typeDescFlagWord, which is what a
// descriptor actually holds - see TypeDescFields::kFlags.
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
 * One immutable realization of a GenSchema: one pointer per slot in the canonical numbering, and
 * nothing else. Emitted code reads slot N - no hashing, no search, no comparison.
 * Implementation-Generics.md part 1's "no runtime name lookup" is exactly this.
 *
 * There is no leading schema word. One was reserved for a debug build to compare against the
 * callee's own schema, and since that check was never written nothing ever *filled* it either: every
 * environment ever emitted led with a literal zero, and a stack-built one led with a word the
 * builder skipped. See TypeDescFields for the same story with the same ending.
 */
namespace GenEnvFields {
    static constexpr U16 kSlots = 0;

    static constexpr U16 slot(U16 index) { return U16(kSlots + index); }
    static constexpr U16 countFor(Size slotCount) { return U16(kSlots + slotCount); }

    /*
     * A GenEnv is the one table that is not always a constant: a generic function calling another
     * builds its callee's environment on the frame - see genEnvironment. It holds the same
     * anchor-relative offsets a constant table holds, which is the whole reason the anchor exists
     * rather than each slot being measured from itself. See repr/table.h.
     */
}

/*
 * The layout of a `ClassWitness` - Implementation-Generics.md part 6.
 *
 * One immutable method table per class implementation: one erased entry point per class function,
 * then one witness pointer per superclass. A generic body that deferred a dispatch loads the witness
 * out of its own environment and the method out of the witness, both at numbers the schema fixed at
 * compile time.
 *
 * Three leading words and a descriptor per class argument used to come first, and all four groups
 * were write-only. The class id was for the unwritten debug check every other table here reserved
 * one for; the argument and method counts are compile-time constants at every reader, since a
 * reader has to know which class it is reading to know what a method index means; and the argument
 * descriptors are Implementation-Generics.md part 6's provision for associated types, which do not
 * exist. Nothing loaded any of them, and they dominated a small witness: a one-method single-
 * argument class was 32 bytes of which 24 were these, and is now 8. They also held a descriptor -
 * and, through the reachability walk, its move and copy glue - alive in programs that never
 * mentioned the type.
 *
 * The method slots hold plain code addresses rather than full `FunctionWitness` records. A witness
 * carries a closure and a captured environment, and a class method has neither: it is a known
 * function reached through a known table. Constrained function *values* are what need the wider
 * shape - which is now exactly the `{code, env}` pair FunValueLayout describes, plus the
 * generic environment a constrained callable additionally has to carry.
 *
 * After the methods come the superclasses: one witness pointer per class this one declares, in the
 * order the declaration wrote them. This is what a requirement that another one *implies* is
 * satisfied through - `class (FromInt(a)) Num(a)` means every `Num` witness names a `FromInt`
 * witness for the same types - so a body holding `Num(a)` dispatches `fromInt` through one extra
 * load rather than through a second environment slot its caller would have had to fill with a
 * witness it already passed. See genWitnessPath, which is where that path is worked out.
 */
namespace ClassWitnessFields {
    // The two counted sections of addresses, in order. Nothing precedes them: a witness is its
    // methods and its superclasses, and every count a reader needs it already knows statically.
    static constexpr U16 kMethods = 0;

    static constexpr U16 method(U16 index) { return U16(kMethods + index); }
    static constexpr U16 super(U16 methodCount, U16 index) {
        return U16(kMethods + methodCount + index);
    }

    static constexpr U16 countFor(U16 methodCount, U16 superCount) {
        return U16(kMethods + methodCount + superCount);
    }

}

/*
 * Which slot of a witness for `typeClass` holds the pointer to its `index`th superclass - the
 * classes it declares, in declaration order.
 *
 * A constant per class, since the method section in front of it is decided by the class rather than
 * by the instance: every witness for one class has the same shape, which is what lets a body
 * compiled once load an implied requirement's witness from a fixed slot.
 */
U16 classSuperclassSlot(GlobalBase global, GlobalPtr<TypeClass> typeClass, U16 index);

/*
 * The witness for one class implementation, interned per class and argument list.
 *
 * Null after reporting when no instance serves these types, or when one of its methods cannot be
 * given an erased entry point. A call site that gets null specializes instead.
 */
ModulePtr<Global> classWitnessFor(Module& module, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args,
                                  LocationId source);

/*
 * The layout of a `PropertyWitness` - Implementation-Generics.md part 5.
 *
 * One constrained field of one concrete owner: the two operations that reach it, and nothing else. A
 * generic body that reads `a.name` loads this out of its environment at the slot its schema numbered
 * and calls through it, with nothing searched and no field name hashed.
 *
 * The owner's and the field's descriptors used to lead, per part 5, and were never loaded: they
 * belong to the *scoped* ABI, where a callback is handed erased field storage and needs a
 * descriptor to make sense of it. The by-value pair below hands the field to storage the caller
 * already knows the type of, so it needs neither. They cost two addresses in every property witness
 * plus - through the reachability walk - a descriptor and its move and copy glue for a field type
 * the program may otherwise never mention. They come back with the scoped form, if it lands.
 *
 * `read` and `set` take addresses and no callbacks, which is deliberately narrower than the scoped
 * `read/modify` the design document describes. A scope is what a *borrow* of a field needs, and the
 * CPS lowering that would make one is not built; by-value get and set are enough for every access a
 * body actually writes, because a generic mutable field use is the same materialize-and-commit pair
 * Design.md's tier 1 already is - read into a temporary, call, set it back. What the scoped form
 * additionally buys is avoiding a copy of a large or non-TrivialCopy field, which is a cost rather
 * than a capability.
 *
 *   read(owner, out)   - writes the field's logical value into uninitialized caller storage.
 *   set(owner, value)  - takes ownership of `value` and commits it, releasing what was there.
 *
 * Both are ordinary generated functions, so an inline field's pair is a load and a store, a packed
 * field's is a shift-and-mask and a read-modify-write, and neither the caller nor the body can tell
 * which it got. That is the whole of "Repr chooses the witness bodies, never whether the property
 * exists".
 */
namespace PropertyWitnessFields {
    static constexpr U16 kRead = 0;
    static constexpr U16 kSet = 1;

    static constexpr U16 kCount = 2;
}

/*
 * The witness for one field of one concrete owner, interned per pair.
 *
 * Null after reporting when the owner has no such field, or when its accessors cannot be generated.
 * A call site that gets null specializes instead, which is the same staging every other witness kind
 * here uses.
 */
ModulePtr<Global> propertyWitnessFor(Module& module, TypePtr owner, StringId field, TypePtr result,
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
 * Creates the label every compiler-built table's address slots are measured from.
 *
 * Must run after resolution and before the reachability walk: it is the last global added, and the
 * walk is what decides which globals exist. Nothing in the IR refers to it - the only reference is
 * made during lowering - so it is seeded as a root there rather than found.
 */
void ensureImageAnchor(Program& program);

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
 * The bytes, as a real function rather than a flag the caller interprets, because the caller is
 * generic code that does not know how many. Null where relocation is a copy of nothing, and null
 * with a diagnostic where the type may not be relocated at all.
 *
 * **Asked only for a managed target.** A native erased relocation block-copies `kSize` bytes inline
 * - see `relocateWith` in resolve/lower_mem.cpp - so the slot this fills is read on one target and
 * the glue is generated for one target. What a managed block copy needs is the *shape*, and a byte
 * count is not one, which is the whole of the difference.
 */
ModulePtr<Function> moveInitFor(Module& module, TypePtr type, LocationId source);

/*
 * `copyInit(dst, src)`: initialize uninitialized `dst` as a structural duplicate of `src`, leaving
 * `src` alive and owning what it owned.
 *
 * Three answers where `moveInit` now has one: the bytes alone for a TrivialCopy type, the authored
 * `Copy` where the type has one, and neither. Null where duplicating is a copy of
 * nothing, and null with a diagnostic where this compiler cannot state one at all - which is the
 * constraint being reported during context construction rather than at the write.
 *
 * Generated as *resolve IR* rather than as anything target-specific, which is the whole reason this
 * closes the gap rather than moving it: each backend compiles the same function its own way, so the
 * block copy inside it becomes a `memcpy` on native and a property-by-property duplicate here, and
 * neither backend needs a case for the other's.
 */
ModulePtr<Function> copyInitFor(Module& module, TypePtr type, LocationId source);

