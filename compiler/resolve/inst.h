#pragma once

#include "type.h"

struct Block;
struct Function;
struct Module;

// `ModuleRegion` and its three aliases are in type.h - see the comment there for why.

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

    /*
     * The path, which is read directly only for what a path *is* - how long it is, which kind each
     * step is, whether two of them agree.
     *
     * **Anything that carries a type along the path goes through `walkPlace` in resolve/place.h.**
     * That walk is the only one that decides what a step arrives at, and the reason it exists is
     * that a dozen consumers used to decide it separately and had to agree: `boxedStep` is a rule
     * about `Field::boxed` that every one of them has to apply, and one that missed it did not fail
     * to build - `pointeeType` answered null, the next Downcast read constructor zero of nothing,
     * and the assertion fired somewhere in container.h.
     *
     * A new consumer that needs the owner type of a step, or the type a step produces, is asking for
     * `PlaceStep` rather than for this.
     */
    ModuleList<Projection, false> projections;
};

/*
 * What a pass may ask about a *kind* rather than about an instruction - see inst.def, where the
 * answer for each kind is a column.
 *
 * Each of these used to be a switch of its own somewhere, and the cost of that is not the lines: a
 * kind added to the IR is silently absent from every one it does not reach, and "absent" reads as
 * `false` in all four - not a terminator, not a constant, not pure, no result. Three of those are
 * the safe answer and one is not.
 */
static constexpr U8 kInstResult = 1 << 0;
static constexpr U8 kInstConstant = 1 << 1;
static constexpr U8 kInstPure = 1 << 2;
static constexpr U8 kInstTerminator = 1 << 3;

struct InstructionTraits {
    StringView mnemonic;
    U8 flags;
};

// Indexed by `Value::Kind`, in the order inst.def lists them - which is the order the enum is
// generated in, so the two cannot come apart.
extern const InstructionTraits kInstructionTraits[];

struct Value {
    enum Kind: U8 {
#define YANA_INST(kind, Struct, mnemonic, flags) kind,
#include "inst.def"
#undef YANA_INST
    };

    // The number of kinds, which is what the traits table is checked against - see inst.cpp.
    static constexpr Size kKindCount =
#define YANA_INST(kind, Struct, mnemonic, flags) + 1
#include "inst.def"
#undef YANA_INST
    ;

    Value(Kind kind, ModulePtr<Block> block, TypePtr type):
        block(block), type(type), kind(kind) {}

    /*
     * ## The instruction traits, as the answers an instruction that has none inherits
     *
     * Four questions every walk over the IR asks, and the four things a kind added to the IR has to
     * say about itself. They are declared here so that "this instruction names no places" and "this
     * instruction reads no operands" are what a struct means by saying nothing, and overridden - by
     * plain hiding, since none of this is virtual - beside the fields each one is about.
     *
     * The point is that a *consumer* never writes a switch over kinds to ask any of them.
     * `visitInstruction` turns a kind into its concrete type once, from inst.def, and everything
     * below is one loop over whatever that type declares. Adding an instruction that names a place
     * therefore reaches every pass that walks places, rather than the ones whose switch was updated.
     *
     *  - `kPlaceCount` / `placeAt` are the storage this instruction names. The slot is handed out
     *    rather than a copy, so that the reader and the rewriter are one declaration - see
     *    `instructionPlaces`, which is `instructionPlaceSlots` dereferenced.
     *  - `mapOperandFields` is the values it reads, in the order `IrEditor::append` records uses in,
     *    each replaced by what `f` answers. The places are walked for the caller, so this is only
     *    what is left.
     *  - `eachTransferField` is the operands it hands ownership *out* through, which is a strict
     *    subset of the above and is spelled separately for that reason - see `eachTransferOperand`.
     *  - `kSuccessorCount` / `successorAt` are the edges a terminator makes, as slots for the same
     *    reason the places are.
     */
    static constexpr Size kPlaceCount = 0;
    static constexpr Size kSuccessorCount = 0;

    Place* placeAt(Size) { return nullptr; }
    ModulePtr<Block>* successorAt(Size) { return nullptr; }

    template<class F> void mapOperandFields(ModuleBase, F&&) {}
    template<class F> void eachTransferField(ModuleBase, F&&) {}

    /*
     * Everything that reads this value, one entry per naming - an instruction that reads it twice is
     * in the list twice, which is what makes "does anything still read this" a count rather than a
     * search.
     *
     * Read-only, on the same terms as `Block`'s lists: this is the second half of a statement whose
     * first half is the reading instruction's own operand, and `IrEditor` (edit.h) is the only thing
     * that may write one without writing the other. See eachOperand for the list the two agree on.
     */
    auto uses(ModuleBase base) { return useList.contents(base); }
    Size useCount() { return useList.size(); }
    ModulePtr<Inst> useAt(ModuleBase base, Size index) { return useList.get(base, index); }

    ModulePtr<Block> block;
    TypePtr type;
    LocationId source = kNullLocation;
    StringId name {};
    U32 id = 0;

    /*
     * The slot this value is the whole contents of, or maxLimit for one that occupies no storage.
     *
     * The other half of `Local::value`, and the reason it is a field rather than a search: an
     * aggregate travels through the IR as the value that produced it, so every pass that asks "which
     * storage is this" - liveness, the borrow check, the resolver placing a projection - was
     * answering it by scanning the local table for the slot whose `value` matched. That is O(locals)
     * per operand per instruction per fixpoint round, and it is a *derivation* of a decision the
     * frontend already made: `call->local` names the destination at the point the call is built.
     *
     * Written by Function::addLocal, which is the one place a slot is paired with the value that
     * fills it. A value bound to two slots would answer with the later one, which is why nothing
     * binds one twice - see the assertion in backingLocal.
     */
    U32 slot = maxLimit<U32>;

    Kind kind;

private:
    friend struct IrEditor;

    ModuleList<ModulePtr<Inst>, false> useList;
};

// A flat operand list, mapped in place - the argument list of the three calls and of a native
// operation. Written once here because a list is the one operand shape more than one instruction has.
template<class F>
inline void mapValueList(ModuleBase base, ModuleList<ModulePtr<Value>, false>& values, F&& f) {
    for(Size i = 0; i < values.size(); i++) values.set(base, i, f(values.get(base, i)));
}

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
 *
 * `defaultValue` is the `= expr` marker, held in exactly the form a field default is - the constant
 * the parameter starts at, since a default is a *constant* and not an expression evaluated per call
 * (doc/spec/functions.md's Default arguments). What it means at a call site is that the position may
 * be left out, and what is passed there is that constant materialized - see
 * ExprResolver::materializeDefaults.
 */
struct Arg: Value {
    Arg(ModulePtr<Block> block, TypePtr type, U16 index):
        Value(Value::Arg, block, type), index(index) {}

    bool isMutableBorrow() const { return convention == ast::BindType::Ref; }
    bool isLazy() const { return lazyType != nullptr; }
    bool hasDefault() const { return defaultValue != nullptr; }

    // The type this parameter has in the signature, which is `type` for all but a `@lazy` one.
    TypePtr declaredType() const { return lazyType ? lazyType : type; }

    U16 index;
    TypePtr lazyType = nullptr;
    ModulePtr<ConstValue> defaultValue = nullptr;
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

/*
 * A string constant - Implementation-String.md part 9, on the target where a string is a host value.
 *
 * **JS only, and that asymmetry is the point.** Natively a literal is bytes in the module's data
 * plus the two words describing them, which is an ordinary global and an ordinary call to
 * `stringLiteral` - nothing about it needs a value kind of its own. On JS there is no data section
 * and no descriptor: a literal *is* a host string, and the only thing that can produce one is a
 * constant in the emitted source. So this exists for the same reason `ConstDouble` does, and the
 * native lowering reports reaching one as an internal error rather than growing a case for it.
 *
 * `text` is the interned decoded content, in UTF-8 as the lexer left it. The JS emitter re-encodes
 * it as a source-level string literal, which is where the host takes over the UTF-16 half of
 * part 2's table.
 */
struct ConstString: Value {
    ConstString(ModulePtr<Block> block, TypePtr type, StringId text):
        Value(Value::ConstString, block, type), text(text) {}

    StringId text;
};

// Every instruction takes its block and its result type as its first two constructor arguments,
// followed by whatever it is made of. That order is what lets builder.h construct any of them
// through one function instead of one overload per kind.
struct Inst: Value {
    using Value::Value;
};

struct InstAlloc: Inst {
    InstAlloc(ModulePtr<Block> block, TypePtr type, U32 local, ModulePtr<Value> extent = nullptr):
        Inst(Value::Alloc, block, type), extent(extent), local(local) {}

    /*
     * How many of `type` this allocates - Implementation-Containers.md §2's `Run(a)`.
     *
     * Null means one, which is every allocation a `let`, a construction or a closure environment
     * makes. A run of slots is the same instruction with a count beside it, and that is deliberately
     * all it is: a run is an *ordinary* allocation, so `selectStorage` places it with no new rule,
     * the drop pass releases it with no new rule, and the only thing that changes downstream is the
     * byte count lowering asks for - in strides rather than in sizes, since a run indexes.
     *
     * A count that is not a compile-time constant forces the heap. The frame answer for one would be
     * a dynamic alloca, which is not released until the frame ends - so a run allocated inside a loop
     * would grow the frame per iteration. That is Implementation-Containers.md §12's third strategy
     * and it is deferred with its own placement rule; until then the conservative answer is the heap.
     *
     * Taken by the constructor rather than set afterwards, because IrEditor::append is what records an
     * operand's uses and it runs once, when the instruction reaches its block.
     */
    ModulePtr<Value> extent;

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
     * The target of a boxed edge - `Field::boxed` or `Constructor::boxed`, whether written as `@box`
     * or inserted as a recursive type's automatic indirection.
     *
     * Two consequences, and they are the two halves of "the aggregate owns this now". It is *not*
     * released by this frame, because the aggregate's derived `Reclaim` hands it back; and it is
     * placed out of line unconditionally rather than by what the escape analysis proved, because the
     * teardown that frees it is interned per *type* and cannot ask where one particular value's box
     * came from. A stack box under a heap-placed owner would be a frame address handed to `freeHeap`.
     *
     * This is the same handover an array literal's buffer makes, and it is a flag rather than a
     * `storageFlag` because a box has no run-time choice to record.
     */
    bool ownedElsewhere = false;

    /*
     * An integer constant recording whether this allocation's storage is the allocator's, for code
     * that has to know at run time. A `Run(a)`'s flag is the case it exists for: `Reclaim(Run(a))` is
     * the placement test of Implementation-Storage.md §5, and which way it goes is a decision made
     * after the body was resolved.
     *
     * One bit and not the `StorageClass` itself, which it briefly held. Four arms only matter to
     * whoever can act differently on them, and nobody can: `Inline`, `Stack` and `Region` release
     * nothing, for three reasons that belong to the owner, the frame and the region rather than to
     * the value. What the second bit cost was a bit of every container's count word - see `HeapFlag`
     * in native.cpp, and Implementation-Containers.md §10.2.
     *
     * Null when nothing asked. selectStorage patches the constant, which is the one place a
     * decision these passes make becomes a value the program itself can read.
     */
    ModulePtr<Value> storageFlag = nullptr;

    // Set when this allocates a closure's environment: the lifted function whose header the storage
    // decision is spent in. The frame that makes the decision is not the one that acts on it, so it
    // is written where the *closure's* teardown will find it - see ClosureHeaderLayout.
    ModulePtr<Function> closure = nullptr;

    /*
     * How many slots a run holds - `extent`, which every walk here had been blind to.
     *
     * It is an operand in every sense that matters: `IrEditor::append` records it as a use, and a
     * rewrite that renumbers values has to renumber it. Leaving it out meant the dead-value pass saw
     * the instruction computing it with no users and deleted it, and the allocation was then left
     * naming a value no block defined - which lowering reports as "resolve value was used before it
     * was lowered".
     *
     * The reason nothing caught it is that every run until now got its extent from an array literal,
     * where the count is a `ConstInt`. A constant belongs to no block and is materialized per
     * function on demand, so it cannot be deleted and needs no remapping - the hole was real from the
     * day `extent` was added and unreachable until something passed a *computed* count.
     * `newStringOfCapacity` is the first thing that does.
     *
     * `storageFlag` is deliberately not here for exactly that reason: it is always the constant the
     * escape analysis patched, so it is never in a block and never at risk. Adding it would be
     * describing a use that does not exist.
     */
    template<class F> void mapOperandFields(ModuleBase, F&& f) {
        if(extent) extent = f(extent);
    }
};

struct InstLoadPlace: Inst {
    InstLoadPlace(ModulePtr<Block> block, TypePtr type, Place place):
        Inst(Value::LoadPlace, block, type), place(place) {}

    Place place;

    /*
     * That this load reads up to a vector's width past the extent of the place it names, and that
     * the tail-read guarantee says doing so is safe - Design-Vector §5.4, Implementation-Vector.md
     * §3.3.
     *
     * The flag is load-bearing in three places and each of them has to be told, because the default
     * reading of "reads past the end" is a bug in every one:
     *
     *  - `opt_range` must not conclude the access is out of bounds and must not keep a bounds check
     *    for it. An overreading load *states* that it is deliberately outside;
     *  - `analyze_escape` and provenance must treat it as reading the place it names and nothing
     *    else. It does not reach into a neighbouring object in any sense the ownership model can
     *    see, because the bytes past the end are unspecified and nothing may follow them;
     *  - the verifier checks that the place is rooted in storage the guarantee covers - owned heap,
     *    stack, static, or a slice of one - and not in an `Unpadded` container or a raw pointer.
     *    That is what makes the invariant an invariant rather than a convention, and it is what
     *    catches a `vectors` implementation that took the fast tail where it owed the safe one.
     *
     * **No store ever carries it**, and the *safe* tail needs no flag at all: an overlapping load at
     * `end - N` and an align-down load at `alignDown(start, N)` are ordinary loads of places wholly
     * inside the object, which `opt_range` proves in bounds by the reasoning it uses everywhere else.
     * That asymmetry is the clearest statement of what the guarantee is worth.
     */
    bool overread = false;

    static constexpr Size kPlaceCount = 1;
    Place* placeAt(Size) { return &place; }
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

    static constexpr Size kPlaceCount = 1;
    Place* placeAt(Size) { return &place; }

    template<class F> void mapOperandFields(ModuleBase, F&& f) { value = f(value); }
    template<class F> void eachTransferField(ModuleBase, F&& f) { f(value, source); }
};

/*
 * Filling `n` components of one place from `n` values - one instruction for what an aggregate
 * literal initializes, rather than one `Init` per component.
 *
 * A component is one step off the place, so this covers both shapes a literal has: the elements of a
 * run or a `[T *n]`, reached by an `Index`, and the fields of a record or a tuple, reached by a
 * `Field`. They are the same instruction because the reason for it is the same one in both cases -
 * see below - and because a target that has to build the value whole gets one rule instead of two.
 *
 * ## Why the components are not `n` separate stores
 *
 * Because they are not `n` separate decisions. `[a, b, c]` names one construction with a known
 * arity, and taking it apart before any pass sees it means every pass that wants it whole has to
 * put it back together - which is what a target that has no uninitialized storage must do. JS has
 * to build the elements *into* the literal, so the emitter was reconstructing the arity by
 * pattern-matching a run of stores, and the generic optimizer had to be taught not to break the
 * shape it was matching on (`leavesArrayHole` in opt/opt_place.cpp).
 *
 * The second cost is worse and is not about any one target. Storage that is allocated and then
 * filled has to hold *something* in between, and on a target with no raw memory that something is
 * a manufactured value of the element type - `zeroValue` walking the type to build an instance that
 * may not satisfy the type's own invariants, safe only because nothing reads it. That is a value
 * the compiler cannot always make: an abstract type across a library boundary and an existential
 * both have shapes this side of the boundary cannot see. Initializing from the values themselves
 * needs no zero at all.
 *
 * ## The components are a hand-over, which an operand normally is not
 *
 * Implementation-Containers.md §14.1 records the trap that made an earlier attempt at this wrong:
 * the ownership passes read a hand-over out of an `InstMove` or an assignment into a place, so an
 * owned value passed as an *operand* left the frame still owning it. This instruction is the
 * exception, and it is one line in `deriveEffects` - `transferFrom` per component, which is exactly
 * what the per-component `Init`s it replaces already did. Ownership-equivalent by construction: for
 * a pointer-rooted place, `transferFrom` was the *only* effect those stores had.
 *
 * ## What it names
 *
 * `place` is the value being built, and each `AggregateComponent` is one value plus the step that
 * says where it goes - so component `i` is at `place + components[i].step`, which is what
 * `aggregateElement` expands. The place is stored as a place rather than as a base pointer so that
 * the root travels through `instructionPlaceSlots` and `mapOperands` like every other one - a
 * rewrite that renumbers values reaches it without a case of its own. Reported with *no* projection,
 * which is the prefix of every component's place and therefore says "this writes the whole value" to
 * alias analysis.
 *
 * The step is stored rather than derived from the component's position, and that is not redundancy
 * in either form. An `Index` carries the constant as a *value*, and which integer type an index is
 * written with is a target decision the resolver is the only one placed to make -
 * `module.scalar.size` on a managed target, where a `Long` is a bigint and `arr[3n]` is a property
 * named "3", against `scalar.long_` natively. A `Field` carries the field number, and a construction
 * routinely skips one: a unit field has no storage, so it is left out and the fields after it keep
 * their own numbers.
 *
 * ## A sum is one construction too
 *
 * `Just(4)` writes a discriminant *and* a payload, and those are two steps off different points: the
 * tag hangs off the value itself and a payload field hangs off the constructor. `constructor` is what
 * closes that gap - it is the `Downcast` every `Field` and `Index` step is taken through, so one
 * instruction still says "these values, this one value" without the steps having to be paths.
 *
 * A `Discriminant` step is not taken through it, and neither is a `Downcast` - the first is the tag
 * and the second is a payload carried whole, and both hang off the value directly. Which is the
 * second reason the step is stored rather than implied by the position.
 *
 * Lowering expands it per target: an address per component on native, exactly what the stores it
 * replaced computed, and one literal on a managed target.
 */
// One value and where it goes. Kept as one item rather than two parallel lists, because every
// consumer wants them together and two lists can only ever be right by everyone remembering to push
// to both - see `eachWrittenComponent`, which is how they are meant to be read.
struct AggregateComponent {
    Projection step;
    ModulePtr<Value> value = nullptr;
};

struct InstAggregate: Inst {
    InstAggregate(ModulePtr<Block> block, TypePtr unit, Place place):
        Inst(Value::Aggregate, block, unit), place(place) {}

    Place place;

    // The constructor a `Field` or `Index` step is taken through, or `maxLimit` where the value has
    // no constructor to step into - which is a tuple, a run, and a record with one constructor whose
    // payload the resolver already stepped into itself.
    U16 constructor = maxLimit<U16>;

    ModuleList<AggregateComponent, false> components;

    static constexpr Size kPlaceCount = 1;
    Place* placeAt(Size) { return &place; }

    /*
     * The components going into the aggregate. The root travels through the place walk above; these
     * are the values, and each is a hand-over rather than a read - see deriveEffects. A step carries
     * a value only in the indexed form, and `f` is asked nothing about a null one for the same reason
     * the place walk skips those.
     */
    template<class F> void mapOperandFields(ModuleBase base, F&& f) {
        for(Size i = 0; i < components.size(); i++) {
            auto component = components.get(base, i);

            component.value = f(component.value);
            if(component.step.value) component.step.value = f(component.step.value);

            components.set(base, i, component);
        }
    }

    /*
     * Every component, which is this instruction's whole exception to the ownership model - see the
     * header above.
     *
     * Missing this let a construction alias droppable storage. `Just(v)` for a `v` this frame only
     * borrows, and `Pair {a: p.x, b: p.y}` for owned fields of a value that still owns them, both
     * stopped being reported - which is a double drop rather than a lost diagnostic. The instruction
     * says it is a hand-over in three other walks and this is the one that checks it.
     */
    template<class F> void eachTransferField(ModuleBase base, F&& f) {
        for(Size i = 0; i < components.size(); i++) f(components.get(base, i).value, source);
    }
};

/*
 * The place of one component - the aggregate's place, projected by that component's step.
 *
 * Rebuilt rather than appended to, because a `Place` holds its path as a list: pushing onto a copy
 * of one would be two places sharing a path, and the second push would be visible through the first.
 * `boxOf` in expr_construct.cpp rebuilds by prefix for the same reason.
 *
 * Both expansions go through this, which is the point - the component an ownership pass reasoned
 * about and the component a backend stores into have to be the same place, and there is one function
 * that says what it is.
 */
inline Place aggregateElement(ModuleBase base, Region<ModuleRegion>& arena,
                              const InstAggregate& aggregate, Size at) {
    Place result = aggregate.place;
    result.projections = {};

    for(auto projection: const_cast<InstAggregate&>(aggregate).place.projections.contents(base)) {
        result.projections.push(arena, projection);
    }

    auto step = const_cast<InstAggregate&>(aggregate).components.get(base, at).step;

    // The constructor a payload field is inside, where the value has one - see InstAggregate. A tag
    // and a payload carried whole hang off the value itself, so neither takes this step.
    if(aggregate.constructor != maxLimit<U16> &&
       (step.kind == ProjectionKind::Field || step.kind == ProjectionKind::Index)) {
        result.projections.push(arena, Projection { ProjectionKind::Downcast, aggregate.constructor });
    }

    result.projections.push(arena, step);
    return result;
}

/*
 * Every component, as it is stored: a step and a value.
 *
 * The cheap walk, for a consumer that wants the values or the raw steps and not the places they
 * expand to - the ownership transfer, the effect summary, escape analysis, cloning. Building a place
 * for each of those would be a projection path allocated per component per pass and thrown away.
 */
template<class F>
inline void eachAggregateComponent(ModuleBase base, const InstAggregate& aggregate, F&& f) {
    auto& components = const_cast<InstAggregate&>(aggregate).components;
    for(Size at = 0; at < components.size(); at++) f(components.get(base, at), at);
}

/*
 * Every component, as the place it writes and the value it puts there.
 *
 * **This is how an aggregate is meant to be interpreted as writes** - not the only way to enumerate
 * its components, which is the walk above. A dozen passes want the same two things about each
 * component, and each of them writing its own expansion is how one of them comes to disagree: the
 * ownership check was written without a walk at all and did not visit the components, which let
 * `Just(v)` alias a borrowed `v` and eventually drop it twice.
 *
 * Neither walk protects against forgetting the opcode entirely, and neither can - that is what the
 * traits below the struct are for, and inst.def behind them. What they remove is the second mistake:
 * visiting the components and expanding them to the wrong places.
 *
 * `at` is passed as well, for the one consumer that needs to name a component again afterwards -
 * `foldIntoAggregate` in opt_place.cpp rewrites the value in place.
 */
template<class F>
inline void eachWrittenComponent(ModuleBase base, Region<ModuleRegion>& arena,
                                 const InstAggregate& aggregate, F&& f) {
    eachAggregateComponent(base, aggregate, [&](const AggregateComponent& component, Size at) {
        f(aggregateElement(base, arena, aggregate, at), component.value, at);
    });
}

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

    static constexpr Size kPlaceCount = 1;
    Place* placeAt(Size) { return &place; }
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

    static constexpr Size kPlaceCount = 1;
    Place* placeAt(Size) { return &place; }
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

    // The only instruction in the IR that names two places, which is what fixes `kMaxPlaces` at two.
    static constexpr Size kPlaceCount = 2;
    Place* placeAt(Size index) { return index == 0 ? &a : &b; }
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

    static constexpr Size kPlaceCount = 1;
    Place* placeAt(Size) { return &place; }

    template<class F> void mapOperandFields(ModuleBase, F&& f) { value = f(value); }
    template<class F> void eachTransferField(ModuleBase, F&& f) { f(value, source); }
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

    static constexpr Size kPlaceCount = 1;
    Place* placeAt(Size) { return &place; }
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
 * A *conditional* teardown - a value moved out of on one arm of a branch and not the other - has no
 * spelling here, deliberately. This instruction used to carry the flag's local and leave each
 * backend to build the test around it; what the drop pass emits instead is an ordinary branch on an
 * ordinary `Bool` local with an ordinary unconditional drop inside it, which every pass after that
 * one already understands and the optimizer can fold. See analyze_drop.cpp's header.
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

    TeardownKind dropKind;
    TeardownKind reclaimKind;

    // Set when this place's own storage has to be handed back as well - a heap-placed allocation
    // whose frame owns it. Separate from `reclaim` because most values release nothing of their own
    // (the storage is the frame's, and the frame returning is the release) and because a type with
    // no teardown at all can still be heap-placed.
    bool releaseStorage = false;

    bool isEmpty() const { return !drop && !reclaim && !releaseStorage; }

    static constexpr Size kPlaceCount = 1;
    Place* placeAt(Size) { return &place; }
};

// The address of a place, as a raw pointer. This is what `addressOf` compiles to, and it is the
// one operation that forces storage to exist: a value it is applied to cannot stay in a register,
// which is the "writable, stable-address representation requirement" Design.md's Pointers section
// gives to anything a raw pointer is taken of.
struct InstAddress: Inst {
    InstAddress(ModulePtr<Block> block, TypePtr type, Place place):
        Inst(Value::Address, block, type), place(place) {}

    Place place;

    static constexpr Size kPlaceCount = 1;
    Place* placeAt(Size) { return &place; }
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

    /*
     * The number in a count position - Implementation-Const-Generics.md §3.2.
     *
     * `of` is the *count* rather than the type that carries it: the `4` of `[Int *4]` interned as a
     * ConstType, or the `n` of `[a *n]` as the context's const variable. Both spellings answer the
     * same question, which is the whole reason this joins the metrics instead of being a form of its
     * own - it already folds, it already prints, and `kInstPure` already lets CSE and LICM hoist it
     * out of the loop that reads it once per element.
     *
     * The one asymmetry with the other three: a generic one is a *slot* read and not a descriptor
     * field, so it is one step shorter rather than one longer. A number has nothing to describe.
     */
    Count,
};

struct InstTypeMetric: Inst {
    InstTypeMetric(ModulePtr<Block> block, TypePtr type, TypePtr of, TypeMetricKind metric):
        Inst(Value::TypeMetric, block, type), of(of), metric(metric) {}

    // The type being measured, which is not `type` - the result is an integer.
    TypePtr of;
    TypeMetricKind metric;
};

/*
 * The address held in one slot of a compiler-built table - the address counterpart of
 * InstTypeMetric, and here for the same reason.
 *
 * A witness table's slots are not a layout this stage may state. Native holds an address as four
 * bytes relative to the slot itself, so reading one is a load, a sign-extension and an add; JS holds
 * a real function reference in an array, so reading one is `table[N]` and there is no width in it at
 * all. Both are answers to the same question - "the address in slot N" - and the question is all
 * that is stable between them.
 *
 * This used to be an ordinary field projection into a tuple laid out like the table (the deleted
 * `typeDescPlaceType`), which worked only because an address slot happened to be exactly a pointer
 * wide. Narrowing the slot broke that identity, and the fix is not a second tuple: it is to stop
 * describing the bytes here and describe the *access* instead. `witness.h`'s numberings are the
 * whole of what a reader and a builder share.
 *
 * The slot index is the whole of what identifies the cell: every cell is four bytes, so slot N is
 * at 4N on every target that has bytes at all, and an array index on the one that does not.
 */
struct InstTableSlot: Inst {
    InstTableSlot(ModulePtr<Block> block, TypePtr type, ModulePtr<Value> table, U16 slot):
        Inst(Value::TableSlot, block, type), table(table), slot(slot) {}

    // The address of the table itself. An operand rather than a place, because what a reader has in
    // hand is an address it computed - a closure header is the bytes in front of an entry point, and
    // no place names those.
    ModulePtr<Value> table;
    U16 slot;

    template<class F> void mapOperandFields(ModuleBase, F&& f) { table = f(table); }
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
    // The two block operations the lower IR already has, reached by name instead of derived from
    // an aggregate assignment. `CopyMemory` is `Native.blockCopy(to, from, count)` on a native build
    // and `Native.copyMemory` on JS - two names for one instruction, because the native library's
    // `copyMemory` is a *body* over this and the JS one has no ladder to be (see attachIntrinsic).
    // `SetMemory` is `setMemory(to, value, count)` on both.
    CopyMemory,
    SetMemory,

    // syscall(number, args...). The lower IR and the x64 backend already model this as a call with
    // its own convention, so the number is operand zero here exactly as it is there.
    Syscall,

    /*
     * The host - Implementation-Containers.md §14.1.
     *
     * The back half of an FFI without its front half: an operation whose meaning belongs to the
     * JavaScript runtime rather than to the machine or to the language. They are here rather than in
     * a `Value::Kind` of their own for the reason the three above them are - a fixed operation, a
     * flat argument list, and a meaning nothing in the IR shares.
     *
     * A *general* node rather than one per container operation, so that `push`, `splice` and every
     * host method after them cost one declaration each and nothing here. `method` is the member
     * name; `args[0]` is the receiver for the two that have one.
     *
     * Native-only by construction on the other side: every declaration that produces one is
     * `@platform(js)`, and `platformEnabled` runs during resolution, so a native build contains no
     * name, no type and no instance that could reach one. The native lowering's arm is therefore an
     * internal error rather than a translation.
     *
     * There is deliberately no `a[i]` node, which §14.1 expected there to be. A host element is a
     * *place* - `ProjectionKind::Index` over a `HostArray(a)`, the same projection `[T *n]`
     * introduced - and a place is an lvalue, so one form gives the read, the write and the borrow at
     * once where an operation would have given only the read.
     */
    HostCall,   // args[0].method(args[1..])
    HostField,  // args[0].method
    HostArray,  // [args...]

    /*
     * `args[0] <operator> args[1]` - the host's own binary operators, for `String`.
     *
     * The three container operations above are all members, and a member is all a container needed.
     * A string is not: concatenation and comparison are *operators* on this target, and the reason
     * that matters is that the alternatives are each wrong in a way worth avoiding. `s.concat(t)` is
     * a real method and would have fitted `HostCall`, but there is no method for `<` at all -
     * `localeCompare` is locale-sensitive and therefore exactly the cross-engine disagreement
     * Implementation-String.md part 6 refuses to accept from `Intl.Segmenter`. Writing the
     * comparison as a `charCodeAt` loop instead is correct and portable and gives up the one
     * property part 3 asks for, which is that a comparison cost what the host's own costs.
     *
     * So: `+` and the relational operators, whose meaning on two host strings is *already* the raw
     * unit-wise one part 3 specifies. `method` carries the operator's spelling, exactly as it
     * carries a member's name for the two above.
     */
    HostBinary,

    /*
     * `Global.method(args...)` - a call on something in the host's global scope rather than on a
     * value the program is holding.
     *
     * `String.fromCharCode` is what needs it, and it is a genuinely different shape from `HostCall`:
     * there is no receiver among the arguments, because the receiver is a name the emitted source
     * writes literally. `method` is the whole dotted path, which keeps the emitter's arm to one line
     * and keeps the *knowledge* - that the constructor is spelled `String` - in `Host`'s declarations
     * where Analysis-JS.md §2.4 asks for it, rather than in the backend.
     */
    HostGlobalCall,

    /*
     * `throw args[0]` - how a program stops on this target.
     *
     * A statement rather than an expression, which is what makes it its own operation instead of a
     * `HostGlobalCall` on some function that happens to throw. JavaScript has no `abort`: the host
     * decides what stopping means, and the one thing every host agrees on is that an exception
     * nobody catches ends the program with the value it carried reported.
     *
     * The operand is a `String`, and it is thrown as it stands rather than wrapped in an `Error`.
     * What the message needs to do is say which check failed, and a string says that with nothing
     * around it that this backend would have to be able to construct.
     */
    HostThrow,

    /*
     * `yana$grow(args[0], args[1])` - a wider typed array with this one's contents copied into it.
     *
     * Its own operation rather than a `HostGlobalCall`, because what it names is not the host's: it
     * is a helper this backend emits, and it is a helper rather than an expression because building
     * the new array and filling it are two statements and JavaScript has no expression form of a
     * sequence that this emitter would rather write.
     *
     * Reached only where `typedArrayFor` said the element has a typed array - the host-array row
     * grows by being written past its end and never asks for this.
     */
    HostGrow,
};

// Whether an operation's meaning belongs to the host rather than to the machine. The JS emitter
// asks it to pick its arm; `expressibleInJs` asks it to let one through at all.
inline bool isHostOp(NativeOp op) {
    return op == NativeOp::HostCall || op == NativeOp::HostField || op == NativeOp::HostArray ||
           op == NativeOp::HostBinary || op == NativeOp::HostGlobalCall || op == NativeOp::HostThrow ||
           op == NativeOp::HostGrow;
}

struct InstNative: Inst {
    InstNative(ModulePtr<Block> block, TypePtr type, NativeOp op, StringId method = StringId()):
        Inst(Value::Native, block, type), method(method), op(op) {}

    ModuleList<ModulePtr<Value>, false> args;

    // The host member or constructor this names, for the four host operations and for nothing else.
    // Unqualified, and the text of it is what the emitter prints - see hostMethodName.
    StringId method;

    NativeOp op;

    /*
     * That this block copy *relocates* rather than duplicates - the source is dead the moment it
     * returns and nothing may read it again. Set on the opening copy of `moveInit$` glue and on
     * nothing else; `CopyMemory` reached any other way is `Native.blockCopy`, whose source the
     * caller goes on owning.
     *
     * A fact about this copy rather than about the function it sits in, which is why it is here and
     * not a flag on `Function`: the same glue could one day open with more than one, and a copy
     * that relocates is what each of them would separately be.
     *
     * Only one backend can tell the difference. Native writes the same `memcpy` either way - the
     * bytes are the bytes, and the source being dead changes nothing about writing them. On JS the
     * copy is structural and has to be *built*, so relocating is the assignment alone where
     * duplicating is a `cloneValue` per property. See genBlockCopy.
     */
    bool relocates = false;

    template<class F> void mapOperandFields(ModuleBase base, F&& f) { mapValueList(base, args, f); }
};

struct InstUnary: Inst {
    InstUnary(ModulePtr<Block> block, TypePtr type, Kind kind, ModulePtr<Value> from):
        Inst(kind, block, type), from(from) {}

    ModulePtr<Value> from;

    template<class F> void mapOperandFields(ModuleBase, F&& f) { from = f(from); }
};

struct InstBinary: Inst {
    InstBinary(ModulePtr<Block> block, TypePtr type, Kind kind, ModulePtr<Value> lhs, ModulePtr<Value> rhs):
        Inst(kind, block, type), lhs(lhs), rhs(rhs) {}

    ModulePtr<Value> lhs;
    ModulePtr<Value> rhs;

    template<class F> void mapOperandFields(ModuleBase, F&& f) {
        lhs = f(lhs);
        rhs = f(rhs);
    }
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
 * One of two values, chosen by a condition, without a branch to choose it in.
 *
 * The one instruction in the resolve IR **no front end produces**. A conditional in the source is a
 * `je` and a phi, which is the honest shape of it: the arms are statements, either of them may leave
 * the block, and deciding that a diamond is cheap enough to compute both sides of is a *cost*
 * question rather than a meaning one. `convertSelects` in compiler/opt is the only thing that builds
 * one, and it builds it from exactly that shape - see opt_select.cpp for the rules.
 *
 * It is in the shared IR rather than in either backend for the reason the whole of compiler/opt is:
 * both targets have the instruction already and neither can see the diamond by the time it could use
 * one. Natively it is a `cmov` - `LowerInstSelect`, which the x64 selector will fold a comparison
 * straight into the flags of. On JS it is `c ? a : b`, which is what lets an `if` and the assignments
 * in its two arms collapse into one expression, and then into whatever reads it.
 *
 * `type` is the value's, and the two arms share it. The condition is the `Bool` the branch tested,
 * which lowers to the same one-bit-in-a-register both targets already test with.
 *
 * **Both arms are evaluated.** That is what the instruction means, so every value it names has to be
 * one that may be computed unconditionally - which is a constraint on the pass that creates it rather
 * than on this, and is why nothing here records which arm was the original one.
 */
struct InstSelect: Inst {
    InstSelect(ModulePtr<Block> block, TypePtr type, ModulePtr<Value> cond,
               ModulePtr<Value> whenTrue, ModulePtr<Value> whenFalse):
        Inst(Value::Select, block, type), cond(cond), whenTrue(whenTrue), whenFalse(whenFalse) {}

    ModulePtr<Value> cond;
    ModulePtr<Value> whenTrue;
    ModulePtr<Value> whenFalse;

    template<class F> void mapOperandFields(ModuleBase, F&& f) {
        cond = f(cond);
        whenTrue = f(whenTrue);
        whenFalse = f(whenFalse);
    }
};

/*
 * `a * b + c`, at most once rounded - see inst.def for why that makes it an instruction rather than
 * two. Three operands and one type, since all three and the result are the same float or the same
 * vector of floats.
 */
struct InstFma: Inst {
    InstFma(ModulePtr<Block> block, TypePtr type, ModulePtr<Value> a, ModulePtr<Value> b, ModulePtr<Value> c):
        Inst(Value::Fma, block, type), a(a), b(b), c(c) {}

    ModulePtr<Value> a, b, c;

    template<class F> void mapOperandFields(ModuleBase, F&& f) {
        a = f(a);
        b = f(b);
        c = f(c);
    }
};

/*
 * Vectors - Implementation-Vector.md §3.2. See inst.def for why there are five of these and not
 * twenty, and for what a vector reuses instead.
 */

/*
 * Which reduction a `VecReduce` performs.
 *
 * One kind with a field rather than six kinds, so that `any`, `all`, `horizontalSum` and the rest
 * are one instruction to fold, to cost and to expand. Unlike the lower IR's `LowerReduce` there is
 * no signed/unsigned split: signedness is in the *type* here and is read off the lane, and the
 * translation picks the signed opcode from it - which is the same place the binary operations get
 * their `Div`/`IDiv` split from.
 *
 * The *order* a floating-point reduction combines in is a stated language property rather than an
 * implementation detail (Design-Vector §4.5), so this says what is combined and the expansion owes
 * the pairwise tree that says in which order.
 */
enum class ReduceOp: U8 {
    Add,
    Mul,
    Min,
    Max,
    And,
    Or,

    /*
     * The lowest set lane of a *mask*, or the lane count where nothing is set.
     *
     * Not a combination of lanes at all, which is why it sits after the six rather than among them:
     * what it answers is a lane *index*, so its result is an `Int` whatever the lane type is, and
     * the pairwise tree the others owe an expansion has nothing to say about it.
     *
     * It is a reduction rather than an arrangement of the other kinds because every target does it
     * in one step and none of them does it the same way: `pmovmskb` and a bit scan on x86, a
     * bitcast to an integer and `cttz` in LLVM, and a chain of conditionals where a lane is a
     * variable. Written portably - `min(select(mask, iota(), splat(lanes)))`, which is what this
     * replaced - it is a reduction tree over a vector, and measured about forty instructions on x64
     * where the movemask is two. See §34 item 2 of test/bench/findings.md.
     */
    FirstSet,

    /*
     * The lanes of a mask as the bits of an integer - lane `i` in bit `i`, and nothing above the
     * lane count. `pmovmskb` and the one operation in this enum that is not portable.
     *
     * The lower IR has had it since §37 - one movemask serves `any`, `all`, `count` and `firstSet`
     * at once - with the rule that a *backend* writes it for itself and nothing above one does,
     * because a target without the instruction should never see the kind rather than owe it an
     * expansion. This is that rule held one layer higher instead of broken: `Native.bits` is
     * `@platform(native)`, so the only code that can name it is code that has already said it is
     * writing for a machine, and the JS backend never resolves a declaration that could produce one.
     * The LLVM backend answers it with the same bitcast `FirstSet` uses, minus the bit scan.
     *
     * What it buys is the loop over *several* matching lanes, which is Implementation-Map.md §4.1's
     * probe: `hits & (hits - 1)` clears the lane just tested in two integer instructions, where the
     * same loop written over a `Mask` is a `firstSet` and a lane-clearing mask operation per
     * iteration. Its result is an `Int` for the reason `FirstSet`'s is - sixteen or thirty-two bits
     * of answer are not a value the lane type holds.
     */
    Bits,
};

// Every lane of the result is the same scalar. `type` is the vector; the operand is one lane of it.
struct InstVecSplat: Inst {
    InstVecSplat(ModulePtr<Block> block, TypePtr type, ModulePtr<Value> from):
        Inst(Value::VecSplat, block, type), from(from) {}

    ModulePtr<Value> from;

    template<class F> void mapOperandFields(ModuleBase, F&& f) { from = f(from); }
};

/*
 * One lane read out of a vector, and a vector with one lane written into it.
 *
 * Two kinds sharing a struct, the way `Init` and `Assign` do: the second is the first plus the value
 * to write, and every pass that reasons about the lane index reasons about both. `value` is null for
 * a `VecLane`, which is what tells the two apart beside the kind.
 *
 * `lane` is a field and not an operand, and that is Design-Vector §3.3 rather than an optimization:
 * a runtime lane index is `pshufb` on x86 and does not exist at all on some targets, so the resolver
 * refuses one and nothing below here has a dynamic case to handle.
 */
struct InstVecLane: Inst {
    // `VecLane`: the result is the lane's type.
    InstVecLane(ModulePtr<Block> block, TypePtr type, ModulePtr<Value> from, U16 lane):
        Inst(Value::VecLane, block, type), from(from), lane(lane) {}

    // `VecWithLane`: the result is the vector.
    InstVecLane(ModulePtr<Block> block, TypePtr type, ModulePtr<Value> from, U16 lane,
                ModulePtr<Value> value):
        Inst(Value::VecWithLane, block, type), from(from), value(value), lane(lane) {}

    ModulePtr<Value> from;
    ModulePtr<Value> value = nullptr;
    U16 lane;

    template<class F> void mapOperandFields(ModuleBase, F&& f) {
        from = f(from);
        if(value) value = f(value);
    }
};

/*
 * Lanes selected from two vectors by a constant pattern.
 *
 * The pattern has one entry per lane of the *result*, each naming a lane of the concatenation of the
 * two sources: `i < lanes` is a lane of the first and `i >= lanes` a lane of the second. A shuffle
 * within one vector names the same value twice, which is what keeps the pattern's meaning
 * independent of how many sources were meant - and is what both backends expect to see.
 */
struct InstVecShuffle: Inst {
    InstVecShuffle(ModulePtr<Block> block, TypePtr type, ModulePtr<Value> left, ModulePtr<Value> right):
        Inst(Value::VecShuffle, block, type), left(left), right(right) {}

    ModulePtr<Value> left;
    ModulePtr<Value> right;

    // One entry per lane of the result. A `SmallArray` rather than the trailing allocation the lower
    // IR's shuffle uses: this IR's instructions are not packed end to end, so there is nothing here
    // for a variable-length tail to be measured against.
    SmallArray<U8, 16> pattern;

    template<class F> void mapOperandFields(ModuleBase, F&& f) {
        left = f(left);
        right = f(right);
    }
};

/*
 * Which of the SHA extension's two-operand instructions this is - see `Value::ShaBinary`.
 *
 * `sha1rnds4` is four members rather than one with an immediate, and that is worth the four: its
 * immediate selects the *round function*, so the four values are four different operations that
 * happen to share an opcode, and nothing that reads this enum then has to carry a number it must
 * check the range of. There are exactly four and there will never be a fifth.
 */
enum class ShaOp: U8 {
    Sha1Msg1,
    Sha1Msg2,
    Sha1NextE,
    Sha1Rounds0,
    Sha1Rounds1,
    Sha1Rounds2,
    Sha1Rounds3,
    Sha256Msg1,
    Sha256Msg2,
};

// The nine, as text - what the IR printer writes after `sha` and what a `.lower` fixture reads back.
inline StringView nameOfShaOp(ShaOp op) {
    switch(op) {
        case ShaOp::Sha1Msg1:    return "sha1msg1"_v;
        case ShaOp::Sha1Msg2:    return "sha1msg2"_v;
        case ShaOp::Sha1NextE:   return "sha1nexte"_v;
        case ShaOp::Sha1Rounds0: return "sha1rnds4.0"_v;
        case ShaOp::Sha1Rounds1: return "sha1rnds4.1"_v;
        case ShaOp::Sha1Rounds2: return "sha1rnds4.2"_v;
        case ShaOp::Sha1Rounds3: return "sha1rnds4.3"_v;
        case ShaOp::Sha256Msg1:  return "sha256msg1"_v;
        default:                 return "sha256msg2"_v;
    }
}

// `vzeroupper` - see `Value::VZeroUpper` in inst.def. No operands and no result: what it changes is
// processor state that nothing in this IR names, which is why it is a statement rather than a value.
struct InstVZeroUpper: Inst {
    InstVZeroUpper(ModulePtr<Block> block, TypePtr unit): Inst(Value::VZeroUpper, block, unit) {}

    template<class F> void mapOperandFields(ModuleBase, F&&) {}
};

// One of the SHA extension's two-operand instructions - see `Value::ShaBinary` in inst.def, which is
// where what each of them computes is written down.
struct InstShaBinary: Inst {
    InstShaBinary(ModulePtr<Block> block, TypePtr type, ModulePtr<Value> lhs, ModulePtr<Value> rhs, ShaOp op):
        Inst(Value::ShaBinary, block, type), lhs(lhs), rhs(rhs), op(op) {}

    ModulePtr<Value> lhs;
    ModulePtr<Value> rhs;
    ShaOp op;

    template<class F> void mapOperandFields(ModuleBase, F&& f) {
        lhs = f(lhs);
        rhs = f(rhs);
    }
};

// `sha256rnds2` - the one SHA instruction with three operands, and the only reason this is a
// separate kind rather than a tenth member of `ShaOp`.
struct InstSha256Rounds: Inst {
    InstSha256Rounds(ModulePtr<Block> block, TypePtr type, ModulePtr<Value> state,
                     ModulePtr<Value> feed, ModulePtr<Value> keys):
        Inst(Value::Sha256Rounds, block, type), state(state), feed(feed), keys(keys) {}

    ModulePtr<Value> state, feed, keys;

    template<class F> void mapOperandFields(ModuleBase, F&& f) {
        state = f(state);
        feed = f(feed);
        keys = f(keys);
    }
};

// Every lane combined into one scalar, in the pairwise order Design-Vector §4.5 states. `type` is
// the lane's; for a mask it is `Int`, so `any` is Or, `all` is And and `count` is Add.
struct InstVecReduce: Inst {
    InstVecReduce(ModulePtr<Block> block, TypePtr type, ModulePtr<Value> from, ReduceOp reduce):
        Inst(Value::VecReduce, block, type), from(from), reduce(reduce) {}

    ModulePtr<Value> from;
    ReduceOp reduce;

    template<class F> void mapOperandFields(ModuleBase, F&& f) { from = f(from); }
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

    template<class F> void mapOperandFields(ModuleBase base, F&& f) { mapValueList(base, args, f); }
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
 * assumed to keep a reference to everything it is handed - see the note at the end of analyze.cpp,
 * and `handover` below for the one call where the language says otherwise.
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

    /*
     * Set on the call a `yield` compiles to: this is a lens or iterator handing a value to its
     * continuation, and nothing else sets it.
     *
     * It is what says the arguments are *not* retained, against the blanket assumption above. The
     * warrant is the continuation parameter's declaration rather than any callee's body, which is
     * why a flag on the instruction is enough and no summary is needed: resolveLensSignature
     * synthesizes that parameter with the default convention and no `return` marker, and
     * resolveLensSignature's explicit form rejects `&`, `->` and `return` on a written one - "it is
     * called, not stored, and its extent is the call". So a value handed over is a borrow whose
     * extent is bounded by this instruction, and the continuation body's own use of it is bounded
     * by the ordinary borrow check exactly as any other borrowed parameter's is.
     *
     * Only the *arguments* are exempted, and only for retention. The callable is still a value like
     * any other, and everything the call returns is read the way it always was.
     *
     * Without it, `iter fn each(xs: [a])` handing over `xs[i]` marks the slice escaped - an
     * aggregate element travels as an address into the run, so what the argument carries is the
     * container's own provenance - and that reaches the caller through the summary as "this borrow
     * outlives the frame that owns it". A scalar element never showed it, because a register
     * argument has no slot for the provenance to be attributed to.
     */
    bool handover = false;

    template<class F> void mapOperandFields(ModuleBase base, F&& f) {
        callable = f(callable);
        address = f(address);
        mapValueList(base, args, f);
    }
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

    /*
     * A const parameter's slot - Implementation-Const-Generics.md §3.1.
     *
     * The cell holds the *number* rather than an address, which is the one thing that makes this a
     * third case rather than a third source for the same case: nothing is encoded relative to the
     * image anchor, and a forwarded one is copied across as the raw cell it is. `count` says which
     * kind of cell this is; `value` is the number when the caller knows it, and zero is a perfectly
     * ordinary count so it cannot double as "not one of these".
     */
    U64 value = 0;
    bool count = false;

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

    template<class F> void mapOperandFields(ModuleBase base, F&& f) { mapValueList(base, args, f); }
};

struct InstJe: Inst {
    InstJe(ModulePtr<Block> block, TypePtr unit, ModulePtr<Value> cond,
           ModulePtr<Block> thenBlock, ModulePtr<Block> elseBlock):
        Inst(Value::Je, block, unit), cond(cond), thenBlock(thenBlock), elseBlock(elseBlock) {}

    ModulePtr<Value> cond;
    ModulePtr<Block> thenBlock;
    ModulePtr<Block> elseBlock;

    /*
     * Two arms, in the order everything that indexes an edge by ordinal means - `Block`'s outgoing
     * slots, `splitEdge`, the drop pass's loop over the arms that lead to one successor.
     *
     * The ordinal is the whole reason these are slots rather than an answer of "the successors":
     * `je %c, X, X` is legal, so "the edge to X" names two edges, and an operation that resolved the
     * first arm and both records of it has given two answers to one question.
     */
    static constexpr Size kSuccessorCount = 2;
    ModulePtr<Block>* successorAt(Size index) { return index == 0 ? &thenBlock : &elseBlock; }

    template<class F> void mapOperandFields(ModuleBase, F&& f) { cond = f(cond); }
};

struct InstJmp: Inst {
    InstJmp(ModulePtr<Block> block, TypePtr unit, ModulePtr<Block> target):
        Inst(Value::Jmp, block, unit), target(target) {}

    ModulePtr<Block> target;

    static constexpr Size kSuccessorCount = 1;
    ModulePtr<Block>* successorAt(Size) { return &target; }
};

struct InstRet: Inst {
    InstRet(ModulePtr<Block> block, TypePtr unit, ModulePtr<Value> value):
        Inst(Value::Ret, block, unit), value(value) {}

    ModulePtr<Value> value;

    template<class F> void mapOperandFields(ModuleBase, F&& f) { value = f(value); }
    template<class F> void eachTransferField(ModuleBase, F&& f) { f(value, source); }
};

/*
 * A block control never leaves - the one terminator with neither a successor nor a result.
 *
 * Written by `endNonReturningBlocks` in opt_flow.cpp and by nothing else. What puts one there is a
 * call to a function declared not to come back (see `Function::noReturn`), which is `checkFailed` and
 * so far only `checkFailed`: every bounds check and every `@bits` narrowing branches to an arm that
 * calls it, and until this existed that arm jumped back to the block below the check. The join it
 * made is what kept two checks of one index against one length from being one check - see §10 item 2
 * of test/bench/findings.md.
 *
 * It is a terminator with no successor slots, which is what every walk over the CFG already knows how
 * to read: a `ret` is the same shape, so dominance, the loop finder and the reachability sweeps need
 * to be told nothing. What it is *not* is a return - nothing is handed back, and the ownership passes
 * have finished by the time one is created, so no path through here owes a drop.
 */
struct InstUnreachable: Inst {
    InstUnreachable(ModulePtr<Block> block, TypePtr unit): Inst(Value::Unreachable, block, unit) {}
};

struct PhiInput {
    ModulePtr<Block> block;
    ModulePtr<Value> value;
};

struct InstPhi: Inst {
    InstPhi(ModulePtr<Block> block, TypePtr type): Inst(Value::Phi, block, type) {}

    ModuleList<PhiInput, false> inputs;

    template<class F> void mapOperandFields(ModuleBase base, F&& f) {
        for(Size i = 0; i < inputs.size(); i++) {
            auto input = inputs.get(base, i);
            input.value = f(input.value);
            inputs.set(base, i, input);
        }
    }

    // Blamed on the phi, because the operand is a value with no instruction of its own to name.
    template<class F> void eachTransferField(ModuleBase base, F&& f) {
        for(Size i = 0; i < inputs.size(); i++) f(inputs.get(base, i).value, source);
    }
};

/*
 * One instruction as the concrete type its kind names - the dispatch every walk below is built on,
 * and the only place a `Value::Kind` is turned into a struct.
 *
 * `f` is called with a reference of that type, so what it does with the traits is resolved
 * statically: a loop over `kPlaceCount` is a loop over a constant, and an instruction that declares
 * nothing compiles to nothing at all. Which is what makes this a replacement for the switches rather
 * than an indirection in front of them - the generated code is the same jump table, and the arms are
 * whatever `f` is.
 *
 * Generated from inst.def, so the case list and the enum are one statement. The trailing call is
 * unreachable and exists to give the switch a value on every path; a `Value` answers every trait
 * with the default, which is the harmless answer to a question about a kind that does not exist.
 */
template<class F>
inline decltype(auto) visitInstruction(Value& instruction, F&& f) {
    switch(instruction.kind) {
#define YANA_INST(kind, Struct, mnemonic, flags) case Value::kind: return f((Struct&)instruction);
#include "inst.def"
#undef YANA_INST
    }

    return f(instruction);
}

// What this kind is, as inst.def states it.
inline const InstructionTraits& instructionTraits(Value::Kind kind) {
    return kInstructionTraits[kind];
}

inline StringView instructionMnemonic(Value::Kind kind) {
    return instructionTraits(kind).mnemonic;
}

// Ends its block, and makes whatever edges its successor slots name.
inline bool isTerminator(const Value& value) {
    return (instructionTraits(value.kind).flags & kInstTerminator) != 0;
}

// Belongs to no block: materialized per function on demand, so it is never deleted and never needs
// remapping.
inline bool isConstant(const Value& value) {
    return (instructionTraits(value.kind).flags & kInstConstant) != 0;
}

/*
 * Whether this value is one the optimizer may compute again, or not compute at all.
 *
 * The column is set on few kinds on purpose, and every kind left out is left out for a reason rather
 * than from caution: the ownership instructions are the decisions the analyses already took, the
 * calls do whatever their callee does, and `LoadPlace` reads storage that something else may be
 * writing - which is a question about aliasing rather than about the instruction, and is what the
 * place forwarding pass exists to answer.
 */
inline bool isPureValue(const Value& value) {
    return (instructionTraits(value.kind).flags & kInstPure) != 0;
}

/*
 * Whether this instruction defines a value another one may name.
 *
 * False for the stores, the drop, the swap and the three terminators, and that is a checkable claim
 * rather than a description: `verifyFunction` asks it of every operand every instruction names. The
 * `Aggregate` is the one worth stating - it fills a place and produces nothing, and what looks like
 * a use of it is a use of the local it writes into, which is attributed to whatever value fills that
 * slot rather than to this.
 */
inline bool producesValue(const Value& value) {
    return (instructionTraits(value.kind).flags & kInstResult) != 0;
}

/*
 * The places one instruction names, as storage a transform may write back into.
 *
 * Every pass that walks storage asks this same question - which slots does this instruction touch -
 * and each of them used to answer it with a switch of its own: recording uses when a block is built,
 * deciding which parameters a specialization has to give storage back to, deciding whether a body
 * can be lowered at all, keeping a table reachable, and the ownership analyses. Five copies of one
 * list, and an instruction added to the IR has to reach all five or the ones it does not reach are
 * silently wrong about it.
 *
 * There is one copy now, and it is the instruction's own `placeAt`. A slot rather than a copy
 * because a rewrite needs the projection the instruction actually holds - a pass replacing the value
 * an `Index` indexes by cannot use a copy of it - and because the reader below is this dereferenced,
 * which is what keeps a reader and a rewriter from disagreeing about which places exist.
 *
 * Writes them into `target` and returns how many. Every instruction names one place except the swap,
 * which is the only one in the IR that names two - so `target` needs room for kMaxPlaces.
 */
static constexpr Size kMaxPlaces = 2;

inline Size instructionPlaceSlots(Value& instruction, Place** target) {
    return visitInstruction(instruction, [&](auto& inst) -> Size {
        for(Size i = 0; i < inst.kPlaceCount; i++) target[i] = inst.placeAt(i);
        return inst.kPlaceCount;
    });
}

// The same list, by value, for the readers - which is most of them.
inline Size instructionPlaces(const Value& instruction, Place* target) {
    Place* slots[kMaxPlaces];
    auto count = instructionPlaceSlots(const_cast<Value&>(instruction), slots);

    for(Size i = 0; i < count; i++) target[i] = *slots[i];
    return count;
}

/*
 * The blocks one terminator jumps to, as slots.
 *
 * The three places that write an edge - recording one when a terminator is appended, redirecting
 * one, and splitting one - each named `InstJe::thenBlock`, `InstJe::elseBlock` and `InstJmp::target`
 * by hand, which is the same list stated three times and one arm of `je %c, X, X` away from
 * disagreeing. The ordinal is the contract: slot `i` here is `Block`'s outgoing slot `i`, and a
 * doubled arm is two slots that happen to hold one block rather than one edge seen twice.
 */
static constexpr Size kMaxSuccessors = 2;

inline Size instructionSuccessorSlots(Value& instruction, ModulePtr<Block>** target) {
    return visitInstruction(instruction, [&](auto& inst) -> Size {
        for(Size i = 0; i < inst.kSuccessorCount; i++) target[i] = inst.successorAt(i);
        return inst.kSuccessorCount;
    });
}

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

/*
 * The operands one instruction hands ownership *out* through.
 *
 * The four departure points - a write into another place, an exchange, a return, and a phi input -
 * and the argument for gathering them is the one `instructionPlaces` makes above: they were four
 * cases enumerated by hand in the borrow checker, and an instruction added to the IR that departs in
 * a fifth way would have been silently exempt from every rule written over them.
 *
 * An *argument* is deliberately not one. Handing a borrowed value on as an argument re-borrows it -
 * another immutable borrow, or the one mutable borrow forwarded - and the callee's convention is
 * what says whether it was taken. That is what keeps this off the case that should stay legal.
 *
 * `f` is called with the operand and the location to blame for it. A phi's inputs are blamed on the
 * phi, because the operand is a value with no instruction of its own to name.
 */
template<class F>
inline void eachTransferOperand(ModuleBase base, Value& instruction, F&& f) {
    visitInstruction(instruction, [&](auto& inst) { inst.eachTransferField(base, f); });
}

/*
 * The operands of one instruction, in the order `IrEditor::append` records uses in.
 *
 * `f` is handed each operand and answers what it should become, which is the one shape that serves
 * a field and a list element alike - a `ModuleList` element is reached through `get`/`set` and
 * there is no reference to hand out. Returning the operand unchanged is the read-only use.
 *
 * This has to name exactly what `IrEditor::append` names. An operand it misses is one a replacement walks
 * past, leaving a use of a value that is no longer defined; an operand it invents is a use count
 * that never balances.
 *
 * Which is why it is here rather than in compiler/opt, where it was written: it is the same list as
 * `instructionPlaces` above, stated the other way round, and an instruction added to the IR has to
 * reach both or the one it misses is silently wrong about it. `IrEditor::append` records the uses, this
 * walks them, and `verifyFunction` checks that the two agree - three readers of one list, and the
 * check is worth nothing if it reads a different list from the one the rewrites do.
 */
template<class F>
void mapOperands(ModuleBase base, Value& instruction, F&& f) {
    // The storage half, which is the same for every instruction that names a place: what the place
    // is rooted in, and the value an `Index` indexes by. What is left below is the operand fields,
    // which are the instruction's own.
    auto place = [&](Place& p) {
        if(p.root == PlaceRoot::Pointer || p.root == PlaceRoot::Borrow) p.pointer = f(p.pointer);

        for(Size i = 0; i < p.projections.size(); i++) {
            auto projection = p.projections.get(base, i);
            if(!projection.value) continue;

            projection.value = f(projection.value);
            p.projections.set(base, i, projection);
        }
    };

    visitInstruction(instruction, [&](auto& inst) {
        for(Size i = 0; i < inst.kPlaceCount; i++) place(*inst.placeAt(i));
        inst.mapOperandFields(base, f);
    });
}

template<class F>
inline void eachOperand(ModuleBase base, Value& instruction, F&& f) {
    mapOperands(base, instruction, [&](ModulePtr<Value> operand) {
        if(operand) f(operand);
        return operand;
    });
}

// How a binding convention is named in a diagnostic. The sigil for the two that have one, and a
// description for the default, since "declared ``" reads as a compiler bug rather than as a rule.
StringView conventionName(ast::BindType convention);

// The printed name of one word of a function value - see FunValueLayout, and funValueFieldType in
// witness.h for the type each one has.
StringView funValueFieldName(U16 field);
