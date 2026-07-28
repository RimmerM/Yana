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

    static constexpr U32 offsetOf(U16 field) { return 8u * field; }
}

enum class ProjectionKind: U8 {
    Discriminant,
    Field,
    Deref,
    Index,
    Downcast,
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
        Copy,
        Drop,
        Address,
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
 */
struct Arg: Value {
    Arg(ModulePtr<Block> block, TypePtr type, U16 index):
        Value(Value::Arg, block, type), index(index) {}

    bool isMutableBorrow() const { return convention == ast::BindType::Ref; }

    U16 index;
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

    // Set when the type has an authored `Sink`: relocating it is that call rather than a memcpy.
    ModulePtr<Function> sink = nullptr;
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

    // For a class dispatch, which slot of the *caller's* environment holds the witness. Filled in
    // after every requirement has been collected, since adding one renumbers the context.
    U16 classSlot = maxLimit<U16>;
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

// How a binding convention is named in a diagnostic. The sigil for the two that have one, and a
// description for the default, since "declared ``" reads as a compiler bug rather than as a rule.
StringView conventionName(ast::BindType convention);

// The printed name of one word of a function value - see FunValueLayout, and funValueFieldType in
// witness.h for the type each one has.
StringView funValueFieldName(U16 field);
