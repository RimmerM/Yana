#pragma once

#include "type.h"

struct Block;
struct Function;
struct Module;
struct ModuleRegion {};

using ModuleBase = RegionBase<ModuleRegion>;

template<class T>
using ModulePtr = RegionPtr<ModuleRegion, T>;

template<class T, bool allowEmbed = true>
using ModuleList = SmallList<ModuleRegion, T, allowEmbed>;

struct Inst;
struct Value;
struct Global;

/*
 * A list of operand values: the arguments of one call, the fields of one construction, the
 * alternatives of one phi.
 *
 * Eight inline. Argument lists are two or three in almost everything, and a function of more than
 * eight arguments is one whose single allocation here is not what makes it expensive. The resolver
 * builds one of these per call site it resolves, and a call site's arguments are themselves calls,
 * so these nest as deeply as the expression does.
 */
using ValueList = SmallArray<ModulePtr<Value>, 8>;

/*
 * Where an owned value's storage comes from (Implementation-IR.md part 3).
 *
 * `Inline` is storage inside its parent, `Stack` is the frame, `Region` is an arena, and `Heap` is
 * the Native allocator. Escape analysis chooses between the frame and the heap, cheapest first;
 * `Region` is reserved and never selected, since regions are deliberately not part of this
 * milestone and adding the rung later should be a third case in an existing decision rather than a
 * new pass. `Inline` is what a field of an aggregate already is, and is never chosen either.
 */
enum class StorageClass: U8 {
    Inline,
    Stack,
    Region,
    Heap,
};

/*
 * What one ownership root's storage has to be able to do (Implementation-IR.md part 5).
 *
 * Design.md's "Binding mutability and owner mutation demand" is careful that this is a property of
 * the owned object rather than of whichever binding currently names it: a move preserves the same
 * root and its demand, a borrow contributes to it, and only a copy starts a new one. So it is
 * inferred per root and never read off a convention.
 *
 * `Unknown` is the analysis's top state rather than a source-level category. It arises where a
 * callee's summary is not available - a dynamic call, an unspecialized generic - and it selects the
 * conservative writable representation wherever one has to be chosen.
 */
enum class MutationDemand: U8 {
    ReadOnly,
    Writable,
    Unknown,
};

struct ReprRequirements {
    MutationDemand mutation = MutationDemand::ReadOnly;

    // Set where something took the address of this root, which is what makes a value that could
    // have stayed in a register have to occupy storage.
    bool needsStableAddress = false;

    // Set where something may grow this root in place, which is what keeps an array's buffer off
    // the frame - see the array's own storage decision in analyze.cpp.
    bool mayResize = false;

    void raise(const ReprRequirements& other) {
        if(other.mutation > mutation) mutation = other.mutation;
        needsStableAddress = needsStableAddress || other.needsStableAddress;
        mayResize = mayResize || other.mayResize;
    }

    bool operator==(const ReprRequirements& other) const {
        return mutation == other.mutation && needsStableAddress == other.needsStableAddress &&
               mayResize == other.mayResize;
    }
};

/*
 * How long a value's storage has to stay valid, in the order the escape analysis tries them
 * (Design.md's "Implicit region-backed allocation": stack, then the ambient region, then heap).
 *
 * `Arguments` is the case a borrow returned from a function is in: the storage is not this frame's
 * and not the ambient region's, it is whatever the caller passed. `Region` is reserved rather than
 * produced - regions are deliberately not part of this milestone - so that adding the rung later is
 * a third case in this decision rather than a new one.
 */
enum class StorageBound: U8 {
    Frame,
    Arguments,
    Region,
    Escapes,
};

/*
 * The two words a function value is made of (Design.md's "Function types", Design-Memory §8).
 *
 * Written as field indices rather than as byte offsets because a function value is reached the way
 * every other aggregate is - a place with a Field projection - so that constructing one, reading one
 * and tearing one down all go through machinery that already exists. The byte offsets are here
 * because lowering is where the structure stops and arithmetic starts.
 *
 *  - `Code` is the entry point, and it always takes the environment as its first parameter, so that
 *    a capturing lambda, a non-capturing one and a plain function referenced by name are one shape.
 *  - `Env` is the storage the captures live in, or null.
 *
 * What is *not* here is the environment's descriptor. Tearing a closure down is a per-closure
 * question rather than a per-type one - two values of one function type can capture completely
 * different things - but it is not a per-*value* question either: which environment a closure has
 * is decided by which lambda it came from, and that is exactly what the code word already names. So
 * the answer is static data attached to the code rather than a third word copied into every value;
 * see ClosureHeaderLayout in witness.h.
 */
namespace FunValueLayout {
    static constexpr U16 kCode = 0;
    static constexpr U16 kEnv = 1;
    static constexpr U16 kFieldCount = 2;

    /*
     * The closure header, as a projection rather than as a third word.
     *
     * Where the header *is* is a target decision, and this is where the two targets part. Native
     * puts it in front of the entry point and reaches it by arithmetic on the code word, which is
     * what keeps a function value two words wide - see ClosureHeaderLayout, and teardownFunValue for
     * the arithmetic. A target whose code word is not an address has nothing in front of it to
     * subtract from, so it attaches the header to the code word instead and this projection is how
     * the teardown asks for it.
     *
     * So it is a *projection* the value may or may not have rather than a field it always has:
     * `kFieldCount` is still two, `offsetOf` is still only asked about those two, and the extra
     * index exists so that one shared teardown can be written against both answers. Only a build
     * whose target provides it ever emits one.
     */
    static constexpr U16 kHeader = 2;
    static constexpr U16 kProjectionCount = 3;

    static constexpr U32 offsetOf(U16 field) { return 8u * field; }
}

enum class ProjectionKind: U8 {
    Discriminant,
    Field,
    Deref,
    Index,
    Downcast,

    /*
     * A field of a type this body cannot see - Design.md's `a.field: b`, and the only projection
     * that names a requirement rather than a structure.
     *
     * `index` is the property slot of the owning function's schema, which is what carries the field
     * name and the result type: a generic body knows *that* `a` has a `name` of type `b` and cannot
     * know where it is, because where it is depends on `a`'s Repr and `a` is not chosen yet.
     *
     * Rewritten into the Downcast and Field it always meant when the body is specialized, which is
     * why nothing downstream of specialization has a case for it - see clonePlace. That is the whole
     * of the compile-time half: the runtime half, where an unspecialized body reads the field
     * through a PropertyWitness the caller passed, is what genericBodyLowerable declines so that a
     * call site specializes instead.
     */
    Property,

    /*
     * The storage unit a packed field lives in, read and written as a whole.
     *
     * The one projection no front end produces. `compiler/opt` appends it when it expands a packed
     * field access into arithmetic - the prefix names the field, and this says "the word containing
     * it" rather than "the bits of it" - and `index` is how wide that word is, in bits.
     *
     * It exists because the expansion happens above the fork while the word is a target decision. A
     * `Place` cannot name a bit range and does not have to: every backend's place walk already
     * produces the *containing word* for a packed field, because a packed field's `offset` is the
     * word's and everything inside it sits at offset zero - so the address (or the property) each
     * one arrives at is exactly what this asks for, and the shift and the mask that used to follow
     * are ordinary instructions by the time the backend sees them.
     *
     * Two invariants the expansion maintains and everything downstream may rely on:
     *
     *  - it is only ever the *last* projection of a path, and the one before it is the packed field;
     *  - two accesses to one word produce two *identical* paths, because the field index is
     *    canonicalized to the lowest one sharing the storage. Without that, `h.version` and
     *    `h.length` would expand to two paths that opt_place.cpp is entitled to say do not alias -
     *    which is true of the fields and false of the word they became.
     */
    Unit,
};

struct Projection {
    ProjectionKind kind;
    U16 index;
    ModulePtr<Value> value = nullptr;
};

/*
 * What a place is rooted in.
 *
 * Implementation-IR.md part 2 states a place as a local plus a path into it, which is all the
 * ownership model ever needs: a local is storage whose lifetime the function knows. Raw memory
 * adds the two roots whose lifetime it does not - a module-level global, and an address the
 * program computed for itself.
 *
 * The distinction is the one the borrow checker will care about. A local root can be proved
 * things about; a pointer root cannot be proved anything about at all, which is exactly what
 * Design.md means by the Native module being unsafe.
 */
enum class PlaceRoot: U8 {
    Local,
    Global,
    Pointer,
    Borrow,
};

struct Place {
    static Place inLocal(U32 local) { return Place { PlaceRoot::Local, local }; }
    static Place inGlobal(ModulePtr<Global> global) { return Place { PlaceRoot::Global, 0, global }; }

    // The memory a raw pointer names. The address is the root itself rather than something loaded
    // from it, so `*p` is one place with no projections rather than a place plus a Deref.
    static Place atPointer(ModulePtr<Value> pointer) {
        return Place { PlaceRoot::Pointer, 0, nullptr, pointer };
    }

    /*
     * The storage a borrow refers to.
     *
     * Shaped exactly like the pointer case and kept apart from it on purpose: the two are the same
     * address and different amounts of knowledge. A pointer root can be proved nothing about, so
     * the borrow checker declines to reason about it at all; a borrow root carries provenance back
     * to the place it was taken of, so writing through one is checked against that place's
     * exclusivity and against the mutability the borrow was created with.
     */
    static Place inBorrow(ModulePtr<Value> borrow) {
        return Place { PlaceRoot::Borrow, 0, nullptr, borrow };
    }

    PlaceRoot root = PlaceRoot::Local;
    U32 local = 0;
    ModulePtr<Global> global = nullptr;
    ModulePtr<Value> pointer = nullptr;
    ModuleList<Projection, false> projections;
};

struct Value {
    enum Kind: U8 {
        Arg,
        ConstInt,
        ConstFloat,
        ConstDouble,
        Alloc,
        LoadPlace,
        Init,
        Assign,
        Borrow,
        Move,
        Swap,
        Exchange,
        Copy,
        Drop,
        Address,
        TypeMetric,
        Native,
        Cast,
        Neg,
        Not,
        Add,
        Sub,
        Mul,
        Div,
        Rem,
        Shl,
        Shr,
        Sar,
        And,
        Or,
        Xor,
        Cmp,
        Symbol,
        Call,
        CallDyn,
        GenCall,
        Je,
        Jmp,
        Ret,
        Phi,
    };

    Value(Kind kind, ModulePtr<Block> block, TypePtr type):
        block(block), type(type), kind(kind) {}

    ModulePtr<Block> block;
    TypePtr type;
    ModuleList<ModulePtr<Inst>, false> uses;
    LocationId source = kNullLocation;
    StringId name = 0;
    U32 id = 0;
    Kind kind;
};

/*
 * One function parameter.
 *
 * `convention` is Design.md's three-item list, and it is a property of the parameter rather than of
 * its type: `fn f(&x: Int)` takes a mutable borrow of an Int, not a value of some `&Int` type. A
 * `&` parameter therefore has type `T` while *arriving* as an address, which is a fact about this
 * argument that only lowering and place resolution consult.
 *
 * That is why `&T` exists as a type only where a borrow has to survive being handed to someone -
 * a result and the binding that receives one - rather than everywhere a parameter can appear.
 *
 * `returnRoot` is the `return` marker: a declaration that a borrow in the result may be rooted in
 * this argument. What it means for the caller is in FunctionSummary; what it means here is that
 * the loan created for this argument lasts until the last use of the result.
 *
 * `lazyType` is the `@lazy` marker, and it is the one place where the parameter's *declared* type
 * and the type of what arrives genuinely differ. What arrives is a nullary thunk over the caller's
 * frame, so `type` is `() -> T` and every pass below resolve treats the parameter as the ordinary
 * function value it is; `lazyType` is the `T` the signature declared, which is what selection,
 * conversion and diagnostics read. Reading the parameter in the body calls the thunk - see
 * ExprResolver::force.
 */
struct Arg: Value {
    Arg(ModulePtr<Block> block, TypePtr type, U16 index):
        Value(Value::Arg, block, type), index(index) {}

    bool isMutableBorrow() const { return convention == ast::BindType::Ref; }
    bool isLazy() const { return lazyType != nullptr; }

    // The type this parameter has in the signature, which is `type` for all but a `@lazy` one.
    TypePtr declaredType() const { return lazyType ? lazyType : type; }

    U16 index;
    TypePtr lazyType = nullptr;
    ast::BindType convention = ast::BindType::Borrow;
    bool returnRoot = false;
};

struct ConstInt: Value {
    ConstInt(ModulePtr<Block> block, TypePtr type, U64 value):
        Value(Value::ConstInt, block, type), value(value) {}

    U64 value;
};

struct ConstFloat: Value {
    ConstFloat(ModulePtr<Block> block, TypePtr type, F32 value):
        Value(Value::ConstFloat, block, type), value(value) {}

    F32 value;
};

struct ConstDouble: Value {
    ConstDouble(ModulePtr<Block> block, TypePtr type, F64 value):
        Value(Value::ConstDouble, block, type), value(value) {}

    F64 value;
};

// Every instruction takes its block and its result type as its first two constructor arguments,
// followed by whatever it is made of. That order is what lets builder.h construct any of them
// through one function instead of one overload per kind.
struct Inst: Value {
    using Value::Value;
};

struct InstAlloc: Inst {
    InstAlloc(ModulePtr<Block> block, TypePtr type, U32 local):
        Inst(Value::Alloc, block, type), local(local) {}

    U32 local;

    // Which of the four storage classes this allocation uses, filled in by escape analysis.
    StorageClass storage = StorageClass::Stack;

    /*
     * Whether the frame that made this allocation is also the one that releases it.
     *
     * True for everything frame-placed, and for a `@heap` binding - the storage went to the heap
     * because it was asked to, and the value still lives and dies here. False for storage that
     * escaped: something else holds it now, and that something's own `Drop` is what releases it.
     * Freeing it here as well would be a double free of the array buffer the array now owns.
     */
    bool releasedHere = true;

    /*
     * A Bool constant recording where this allocation landed, for code that has to know at run
     * time. The array's buffer is the case it exists for: its `Drop` frees the buffer only when it
     * went to the heap, and whether it did is a decision made after the body was resolved.
     *
     * Null when nothing asked. selectStorage patches the constant, which is the one place a
     * decision these passes make becomes a value the program itself can read.
     */
    ModulePtr<Value> storageFlag = nullptr;

    // Set when this allocates a closure's environment: the lifted function whose header the storage
    // decision is spent in. The frame that makes the decision is not the one that acts on it, so it
    // is written where the *closure's* teardown will find it - see ClosureHeaderLayout.
    ModulePtr<Function> closure = nullptr;
};

struct InstLoadPlace: Inst {
    InstLoadPlace(ModulePtr<Block> block, TypePtr type, Place place):
        Inst(Value::LoadPlace, block, type), place(place) {}

    Place place;
};

/*
 * Writing a value into a place. Two kinds share one shape, because they differ in exactly one
 * thing and everything that reads them reads them the same way:
 *
 *  - `Init` fills storage that held nothing - a fresh local, a field of a value being constructed.
 *  - `Assign` overwrites storage that held a live value, which means the old value's `Drop` runs
 *    first.
 *
 * Keeping them apart is what makes "has this been initialized" a property the compiler can check
 * rather than a convention, which is the prerequisite for `@uninit &` and for knowing which places
 * a drop pass owes a drop to.
 */
struct InstInit: Inst {
    InstInit(ModulePtr<Block> block, TypePtr unit, Place place, ModulePtr<Value> value,
             Kind kind = Value::Init):
        Inst(kind, block, unit), place(place), value(value) {}

    Place place;
    ModulePtr<Value> value;
};

/*
 * A borrow of a place - Implementation-IR.md part 3's InstBorrow.
 *
 * The root is kept rather than the address alone, because everything the borrow checker asks is
 * about the root: whether a second borrow of an overlapping path is live at the same time, and
 * whether the owner is still initialized. An address would answer none of it.
 *
 * The result type is `&T`, and its mutability is the one this instruction was created with. A
 * borrow value is an address once the checking is done, so lowering maps it to the place's address
 * the way InstAddress does; what the type buys is everything before that - a place rooted in a
 * borrow knows whether it may be written, and a call knows whether it was handed a loan or a copy.
 */
struct InstBorrow: Inst {
    InstBorrow(ModulePtr<Block> block, TypePtr type, Place place, bool mut):
        Inst(Value::Borrow, block, type), place(place), mut(mut) {}

    Place place;

    // Exclusive while live. An immutable borrow coexists with any number of others.
    bool mut;
};

/*
 * Taking ownership out of a place - what `->` compiles to.
 *
 * The place is dead afterwards: reading it again is a use-after-move, and the drop pass owes it no
 * drop unless something initializes it again. Both of those are why this is an instruction rather
 * than a plain read - the value produced is indistinguishable from a load, and the statement being
 * made is entirely about the place it came out of.
 *
 * Only emitted for a type that is not TrivialCopy. Design.md's copy-on-read rule says a `->`
 * binding of a TrivialCopy source produces an independent copy and leaves the source untouched, so
 * for those this is an InstCopy or - for a scalar already in a register - nothing at all.
 */
struct InstMove: Inst {
    InstMove(ModulePtr<Block> block, TypePtr type, Place place):
        Inst(Value::Move, block, type), place(place) {}

    Place place;

    // Set when relocating the type is a call rather than a memcpy: the authored `Sink` where the
    // type has one, and the generated member-wise glue where a member has one. Null for a
    // TrivialSink type, whose relocation is its bytes - see sinkFor.
    ModulePtr<Function> sink = nullptr;
};

/*
 * Exchanging the contents of two places - `swap(a, b)`.
 *
 * The one ownership operation that is *total*. Every other one moves a place across the
 * initialization lattice: Init fills, Move and Drop empty, and Assign owes a drop that depends on
 * which of those the place last went through. A swap's precondition and postcondition are the same
 * statement - both places hold a live value before and after - so it changes no state at all.
 *
 * Which is why it is an instruction rather than the three moves it looks like. Written out, the
 * middle of it has one place emptied and the other not yet refilled, and a move out of a borrow is
 * exactly what checkMoves rejects - for the good reason that it would leave someone else's storage
 * empty with nothing obliged to refill it. Here the refill is the same operation, so the property
 * holds at every instruction boundary; making the sequence one instruction is what makes that a
 * fact about the IR rather than a claim about the order of three of them.
 *
 * The upshot is that this is the operation that works on the roots the lattice cannot describe. A
 * global, a borrow, an element the collection handed back - none has a state to consult, and a swap
 * never needs one. Taking a value out of any of those is spelled with this rather than with `->`.
 */
struct InstSwap: Inst {
    InstSwap(ModulePtr<Block> block, TypePtr unit, Place a, Place b, TypePtr content):
        Inst(Value::Swap, block, unit), a(a), b(b), content(content) {}

    Place a;
    Place b;

    // What is being exchanged. Carried rather than re-derived because `type` is unit - a swap
    // produces nothing - and lowering has no Module to ask placeType with.
    TypePtr content;

    // The relocation, on the same terms as InstMove::sink: null when the type's bytes are its whole
    // relocation, and the authored or generated `Sink` otherwise. One field for what lowering
    // performs three times.
    ModulePtr<Function> sink = nullptr;
};

/*
 * `exchange(slot, ->value)` - write `value` into `slot` and produce what was there.
 *
 * The same totality argument as InstSwap, and a separate instruction for the reason the two were
 * asked for together: they cost different amounts. A swap is three relocations and a temporary,
 * because neither place can be written until both have been read. An exchange is two and no
 * temporary, because the incoming value is already a value rather than a place - the caller moved
 * it in - so there is nothing to save it from.
 *
 * `swap(a, b)` is therefore not `exchange` twice, and `exchange(s, v)` is not a swap with a
 * temporary the optimizer might remove. They are two operations, and a caller that has a
 * replacement in hand should not pay for the one that does not.
 */
struct InstExchange: Inst {
    InstExchange(ModulePtr<Block> block, TypePtr type, Place place, ModulePtr<Value> value):
        Inst(Value::Exchange, block, type), place(place), value(value) {}

    Place place;
    ModulePtr<Value> value;

    ModulePtr<Function> sink = nullptr;

    // Storage for the result, as InstCopy has: what came out of the place is a value with a root of
    // its own, and for a memory type that root has to be somewhere. maxLimit for a scalar, which
    // comes out in a register.
    U32 local = maxLimit<U32>;
};

/*
 * An independent, freshly-rooted duplicate of a place.
 *
 * Deliberately a different operation from a borrow rather than a special case of one. Emitting a
 * copy is exactly what lets a TrivialCopy read out through a live `&` not count as handing out a
 * second borrow: the result has its own root, so the source's exclusivity is untouched.
 */
struct InstCopy: Inst {
    InstCopy(ModulePtr<Block> block, TypePtr type, Place place):
        Inst(Value::Copy, block, type), place(place) {}

    Place place;

    // Set when the type has an authored `Copy`; null for the bitwise duplicate a TrivialCopy type
    // gets. The local is the storage the duplicate lands in, as a call result's is.
    ModulePtr<Function> copy = nullptr;
    U32 local = maxLimit<U32>;
};

/*
 * The end of a value's lifetime, inserted by the drop pass and never by the AST resolver.
 *
 * Design-Memory §4 splits teardown in two, and this instruction carries both because they are
 * elidable under different conditions and a later pass must not have to re-derive which is which:
 *
 *  - `drop` is the effect - closing a socket, unlocking a mutex - and is never elided, on any
 *    target, ever;
 *  - `reclaim` releases the value's own storage, and specializes away entirely wherever something
 *    else reclaims it in bulk (a region reset, the JS host's collector).
 *
 * They run in that order, which is the only order that works: whatever the drop does, it does while
 * the storage it does it in is still there. `releaseStorage` is the last step and is the reclaim
 * half of *this* allocation rather than of its members - see selectStorage.
 *
 * `flag` is set only where control flow made the teardown conditional - a value moved out of on one
 * arm of a branch and not the other. It names an I8 local holding 1 while the place still owns
 * something, which is the standard drop-elaboration answer to a question no amount of static
 * analysis can settle.
 */
struct InstDrop: Inst {
    InstDrop(ModulePtr<Block> block, TypePtr unit, Place place, TeardownKind dropKind,
             TeardownKind reclaimKind):
        Inst(Value::Drop, block, unit), place(place), dropKind(dropKind), reclaimKind(reclaimKind) {}

    Place place;

    // What to run for each half: an authored instance, or the glue synthesized for a derived one.
    // Null where that half is empty, which the pass elides rather than emits.
    ModulePtr<Function> drop = nullptr;
    ModulePtr<Function> reclaim = nullptr;

    // The drop flag's local, or maxLimit when the teardown is unconditional.
    U32 flag = maxLimit<U32>;

    TeardownKind dropKind;
    TeardownKind reclaimKind;

    // Set when this place's own storage has to be handed back as well - a heap-placed allocation
    // whose frame owns it. Separate from `reclaim` because most values release nothing of their own
    // (the storage is the frame's, and the frame returning is the release) and because a type with
    // no teardown at all can still be heap-placed.
    bool releaseStorage = false;

    bool isEmpty() const { return !drop && !reclaim && !releaseStorage; }
};

// The address of a place, as a raw pointer. This is what `addressOf` compiles to, and it is the
// one operation that forces storage to exist: a value it is applied to cannot stay in a register,
// which is the "writable, stable-address representation requirement" Design.md's Pointers section
// gives to anything a raw pointer is taken of.
struct InstAddress: Inst {
    InstAddress(ModulePtr<Block> block, TypePtr type, Place place):
        Inst(Value::Address, block, type), place(place) {}

    Place place;
};

/*
 * How wide a type is, as a value rather than as a number the resolver already knew.
 *
 * `sizeOf(x)`, `alignOf(x)` and the scale factor in `p + n` all used to be folded to a ConstInt
 * while the call was being resolved, which quietly made the resolver the authority on layout. Layout
 * belongs to a target (see compiler/repr/repr.h), so the question travels in the IR and is answered
 * by whoever is emitting: the native path folds it against its own Repr table, and the JS path
 * against its own.
 *
 * The unexpected dividend is generic code. `sizeOf` on a type variable had no answer at all before,
 * because there was no number to fold; here it is the same instruction, and lowering reads the width
 * out of the caller's TypeDesc instead of out of a table. One instruction, and the concrete and
 * erased cases stop being different features.
 */
enum class TypeMetricKind: U8 {
    Size,
    Align,

    // What indexing homogeneous storage advances by, which is not always the size - see Repr.
    Stride,
};

struct InstTypeMetric: Inst {
    InstTypeMetric(ModulePtr<Block> block, TypePtr type, TypePtr of, TypeMetricKind metric):
        Inst(Value::TypeMetric, block, type), of(of), metric(metric) {}

    // The type being measured, which is not `type` - the result is an integer.
    TypePtr of;
    TypeMetricKind metric;
};

/*
 * An operation of the Native module that is not expressible as anything more basic.
 *
 * These are one instruction rather than one kind each because they have nothing in common with
 * the rest of the IR and everything in common with each other: a fixed operation, a flat argument
 * list, and a meaning that is the machine's rather than the language's. Everything the resolver
 * does with them is the same for all of them, so a kind per operation would be a case per
 * operation in five switches to say the same thing five times.
 */
enum class NativeOp: U8 {
    // copyMemory(to, from, count) and setMemory(to, value, count) - the two block operations the
    // lower IR already has, reached by name instead of derived from an aggregate assignment.
    CopyMemory,
    SetMemory,

    // syscall(number, args...). The lower IR and the x64 backend already model this as a call with
    // its own convention, so the number is operand zero here exactly as it is there.
    Syscall,
};

struct InstNative: Inst {
    InstNative(ModulePtr<Block> block, TypePtr type, NativeOp op):
        Inst(Value::Native, block, type), op(op) {}

    ModuleList<ModulePtr<Value>, false> args;
    NativeOp op;
};

struct InstUnary: Inst {
    InstUnary(ModulePtr<Block> block, TypePtr type, Kind kind, ModulePtr<Value> from):
        Inst(kind, block, type), from(from) {}

    ModulePtr<Value> from;
};

struct InstBinary: Inst {
    InstBinary(ModulePtr<Block> block, TypePtr type, Kind kind, ModulePtr<Value> lhs, ModulePtr<Value> rhs):
        Inst(kind, block, type), lhs(lhs), rhs(rhs) {}

    ModulePtr<Value> lhs;
    ModulePtr<Value> rhs;
};

enum class CompareOp: U8 {
    Eq,
    Ne,
    Gt,
    Ge,
    Lt,
    Le,
};

struct InstCmp: InstBinary {
    InstCmp(ModulePtr<Block> block, TypePtr type, ModulePtr<Value> lhs, ModulePtr<Value> rhs, CompareOp cmp):
        InstBinary(block, type, Value::Cmp, lhs, rhs), cmp(cmp) {}

    CompareOp cmp;
};

/*
 * The address of something the linker names - a function's entry point, or a compiler-built
 * constant table - as a raw pointer.
 *
 * One instruction for both because they are one operation: an address that is not known until the
 * module is placed and that nothing in the frame computes. It exists because a function value has to
 * hold both (`{code, env}`), and neither was expressible before - a place rooted in a
 * global names the storage rather than its address, and a TypeDesc has no source type for a place
 * to be typed by at all.
 */
struct InstSymbol: Inst {
    InstSymbol(ModulePtr<Block> block, TypePtr type, ModulePtr<Function> callee, ModulePtr<Global> global):
        Inst(Value::Symbol, block, type), callee(callee), global(global) {}

    // Exactly one of the two is set.
    ModulePtr<Function> callee;
    ModulePtr<Global> global;
};

struct InstCall: Inst {
    InstCall(ModulePtr<Block> block, TypePtr type, ModulePtr<Function> callee):
        Inst(Value::Call, block, type), callee(callee) {}

    ModulePtr<Function> callee;
    ModuleList<ModulePtr<Value>, false> args;
    U32 local = maxLimit<U32>;
};

/*
 * A call through an address rather than to a name - what calling a function value compiles to.
 *
 * `callable` is the function value itself rather than the two words unpacked out of it, and that is
 * load-bearing rather than a convenience: the value is what the *ownership* passes have to see. A
 * call that named only the loaded code and environment would leave the closure's own last use at
 * those loads, and the drop pass would then release the environment immediately before the call
 * that is about to read it.
 *
 * `address` is the other shape: a bare code address, for the calls the compiler makes on its own
 * account - a teardown reached through a descriptor - where there is no function value and no
 * environment convention to honour. Exactly one of the two is set.
 *
 * `signature` is the FunType the call was written through, which is where the argument conventions
 * and the return-root group come from. That is the whole reason FunArg carries them: a caller
 * reaching a function through a value has the type and nothing else, and a contract that evaporated
 * here would be worse than no contract. Null for a compiler-internal call.
 *
 * It is read by the resolver and by the ownership passes alike, and both read the same two things:
 * each argument's convention decides what is passed and what that does to the caller's storage, and
 * the declared `return` group decides what a borrow in the result may be rooted in and how long the
 * loans the arguments created have to live. What the type cannot state is retention, so this call is
 * assumed to keep a reference to everything it is handed - see the note at the end of analyze.cpp.
 */
struct InstCallDyn: Inst {
    InstCallDyn(ModulePtr<Block> block, TypePtr type, ModulePtr<Value> callable,
                ModulePtr<Value> address, TypePtr signature):
        Inst(Value::CallDyn, block, type), callable(callable), address(address), signature(signature) {}

    ModulePtr<Value> callable;
    ModulePtr<Value> address;
    TypePtr signature;
    ModuleList<ModulePtr<Value>, false> args;
    U32 local = maxLimit<U32>;
};

/*
 * A call whose callee is not decided yet, because deciding it needs types this function does not
 * have. It appears only inside a generic body and never survives specialization: cloning
 * substitutes `typeArgs`, and a concrete argument list turns the instruction into an ordinary
 * InstCall to the class implementation or to the callee's specialization.
 *
 * Keeping the resolved decision here rather than re-deriving it later is what Implementation-
 * Generics.md's first invariant asks for. The body has already decided *which class function*
 * and *with which type arguments*; specialization only supplies the instance.
 */
/*
 * How one slot of a callee's environment is filled at a call site.
 *
 * Exactly one of the two applies: either the caller knows the slot concretely and stores the address
 * of an interned constant, or the slot is one of the caller's own type variables and the caller
 * copies its own slot across.
 */
struct GenSlotFill {
    ModulePtr<Global> constant = nullptr;
    U16 forwarded = maxLimit<U16>;

    // The superclasses to step through from the forwarded slot, when what the caller holds is a
    // witness that *implies* the one the callee's slot wants rather than that one itself. One byte
    // offset into a witness per step. Empty for a slot copied across as it stands, and unused by a
    // constant. See genWitnessPath and ClassWitnessLayout.
    ModuleList<U32, false> forwardedSupers;

    bool isForwarded() const { return forwarded != maxLimit<U16>; }
};

struct InstGenCall: Inst {
    InstGenCall(ModulePtr<Block> block, TypePtr type, ModulePtr<Function> callee,
                GlobalPtr<TypeClass> typeClass, U16 index):
        Inst(Value::GenCall, block, type), callee(callee), typeClass(typeClass), index(index) {}

    // The class signature this dispatches to, or the generic function being called.
    ModulePtr<Function> callee;

    // Set for a class dispatch, with `index` naming the function within the class. Null when the
    // callee is a generic function, whose `typeArgs` are its own context's instead.
    GlobalPtr<TypeClass> typeClass;

    ModuleList<TypePtr, false> typeArgs;
    ModuleList<ModulePtr<Value>, false> args;
    U16 index;
    U32 local = maxLimit<U32>;

    // Set when this call took the erased path rather than being specialized: the constant
    // environment the callee reads its slots out of, built for exactly these type arguments. Null
    // for a call still waiting to be made concrete by an instantiation, and null for one whose
    // environment cannot be a constant - see `fill`.
    ModulePtr<Global> env = nullptr;

    /*
     * How to build the callee's environment when it cannot be one interned constant.
     *
     * Implementation-Generics.md part 9 lists four ways to supply an environment, and the middle two
     * are what this is for: a generic body calling another generic function knows some of the
     * callee's slots concretely and has to project the rest out of its *own* environment. The result
     * is a small table assembled on the frame from forwarded and static pointers.
     *
     * One entry per slot of the callee's schema, in that numbering. Empty when `env` covers it.
     */
    ModuleList<GenSlotFill, false> fill;

    // For a class dispatch, which slot of the *caller's* environment holds the witness, and the
    // superclasses to step through from it - the requirement this call dispatches on may be one the
    // body never declared because another requirement implies it. Filled in after every requirement
    // has been collected, since adding one renumbers the context.
    U16 classSlot = maxLimit<U16>;
    ModuleList<U32, false> classPath;
};

struct InstJe: Inst {
    InstJe(ModulePtr<Block> block, TypePtr unit, ModulePtr<Value> cond,
           ModulePtr<Block> thenBlock, ModulePtr<Block> elseBlock):
        Inst(Value::Je, block, unit), cond(cond), thenBlock(thenBlock), elseBlock(elseBlock) {}

    ModulePtr<Value> cond;
    ModulePtr<Block> thenBlock;
    ModulePtr<Block> elseBlock;
};

struct InstJmp: Inst {
    InstJmp(ModulePtr<Block> block, TypePtr unit, ModulePtr<Block> target):
        Inst(Value::Jmp, block, unit), target(target) {}

    ModulePtr<Block> target;
};

struct InstRet: Inst {
    InstRet(ModulePtr<Block> block, TypePtr unit, ModulePtr<Value> value):
        Inst(Value::Ret, block, unit), value(value) {}

    ModulePtr<Value> value;
};

struct PhiInput {
    ModulePtr<Block> block;
    ModulePtr<Value> value;
};

struct InstPhi: Inst {
    InstPhi(ModulePtr<Block> block, TypePtr type): Inst(Value::Phi, block, type) {}

    ModuleList<PhiInput, false> inputs;
};

bool isTerminator(const Value& value);
bool isConstant(const Value& value);

/*
 * The places one instruction names.
 *
 * Every pass that walks storage asks this same question - which slots does this instruction touch -
 * and each of them used to answer it with a switch of its own: recording uses when a block is built,
 * deciding which parameters a specialization has to give storage back to, deciding whether a body
 * can be lowered at all, keeping a table reachable, and the ownership analyses. Five copies of one
 * list, and an instruction added to the IR has to reach all five or the ones it does not reach are
 * silently wrong about it.
 *
 * Writes them into `target` and returns how many. Every instruction here names one place except the
 * swap, which is the only one in the IR that names two - so `target` needs room for kMaxPlaces.
 */
static constexpr Size kMaxPlaces = 2;
Size instructionPlaces(const Value& instruction, Place* target);

/*
 * The same list, as storage a transform may write back into.
 *
 * `instructionPlaces` answers with copies, which is what every *reader* wants and what a rewrite
 * cannot use - a pass replacing the value a projection indexes by has to reach the projection the
 * instruction actually holds. Kept beside the reader rather than in the pass that needed it first,
 * so that an instruction added to the IR gains both answers in one place or neither.
 */
Size instructionPlaceSlots(Value& instruction, Place** target);

// The same, for a caller that would only have written the loop.
template<class F>
inline void eachPlace(const Value& instruction, F&& f) {
    Place places[kMaxPlaces];
    auto count = instructionPlaces(instruction, places);
    for(Size i = 0; i < count; i++) f(places[i]);
}

// The first place an instruction names, for callers that only ever ask about the single-place ones.
// A swap answers with `a`, which is the same order instructionPlaces writes them in.
inline bool firstPlace(const Value& instruction, Place& target) {
    Place places[kMaxPlaces];
    if(!instructionPlaces(instruction, places)) return false;

    target = places[0];
    return true;
}

// How a binding convention is named in a diagnostic. The sigil for the two that have one, and a
// description for the default, since "declared ``" reads as a compiler bug rather than as a rule.
StringView conventionName(ast::BindType convention);

// The printed name of one word of a function value - see FunValueLayout, and funValueFieldType in
// witness.h for the type each one has.
StringView funValueFieldName(U16 field);
