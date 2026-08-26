#pragma once

#include "lower.h"

enum class LowerCmp {
    eq, neq, gt, ge, lt, le,
    igt, ige, ilt, ile,

    /*
     * "either operand is a NaN", which is not one of the six and cannot be built out of them.
     *
     * Every ordered comparison of a NaN is false, so no pair of them separates a NaN from a value
     * below the range: both answer false to `x >= lo`. That is exactly the question a saturating
     * float-to-integer conversion has to ask, since one of the two wants zero and the other wants
     * the minimum - see `saturationRange` and expandFloatToSigned.
     *
     * Float operands only. It is one condition code on x86 (parity) and `fcmp uno` on LLVM, so it is
     * cheaper on both than the equality it replaces, which needs two flags and a correction.
     *
     * `ord` is its negation and exists only to be one: nothing produces an ordered test directly,
     * and `negateCmp` has to have an answer for every code it is handed.
     */
    uno, ord
};

/*
 * The relation that holds exactly where this one does not.
 *
 * Total over the twelve, which is what makes it usable as an answer rather than as a rewrite that
 * may decline: `ord` exists so that `uno` has one, and the signed and unsigned families negate
 * within themselves - `!(a <s b)` is `a >=s b` and says nothing about the unsigned order.
 *
 * A fact about the relations rather than about any one target, so it lives here: the x64 encoder
 * negates a branch it emits the other way round, and the backend's own mask folding negates a
 * comparison it is about to complement the result of.
 */
inline LowerCmp negatedCmp(LowerCmp cmp) {
    switch(cmp) {
        case LowerCmp::eq:  return LowerCmp::neq;
        case LowerCmp::neq: return LowerCmp::eq;
        case LowerCmp::gt:  return LowerCmp::le;
        case LowerCmp::ge:  return LowerCmp::lt;
        case LowerCmp::lt:  return LowerCmp::ge;
        case LowerCmp::le:  return LowerCmp::gt;
        case LowerCmp::igt: return LowerCmp::ile;
        case LowerCmp::ige: return LowerCmp::ilt;
        case LowerCmp::ilt: return LowerCmp::ige;
        case LowerCmp::ile: return LowerCmp::igt;
        case LowerCmp::uno: return LowerCmp::ord;
        case LowerCmp::ord: return LowerCmp::uno;
    }

    return cmp;
}

/*
 * How much of the rest of memory an atomic operation orders - Analysis-Atomics.md §3.3.
 *
 * One enum here where the library has four types. `LoadOrder`, `StoreOrder`, `UpdateOrder` and
 * `FenceOrder` exist so that `store(x, LoadAcquire)` does not typecheck, which is a statement about
 * what a *caller* may write; by the time an operation reaches this IR the pairing has already been
 * decided and what is left is the one fact every consumer reads, which is how strong the edge is.
 * Four enums here would mean four switches in every backend agreeing on five cases.
 *
 * The subset each instruction admits is therefore a verifier rule rather than a type - see
 * `isLoadOrder` and the three below it, and `validateAtomicLoad` and its neighbours in
 * lower_validate.cpp, which is where a `atomic_store %p, %v, 4, acquire` is refused.
 *
 * No `consume`. C++ has never had an implementation that did not promote it to `acquire`, and its
 * dependency-tracking rule is a whole second aliasing discipline for one algorithm shape.
 */
enum class LowerOrder: U8 {
    // Only this location's own modification order. Publishes nothing else.
    Relaxed,

    // After observing a matching release, whatever that release published is visible from here on.
    Acquire,

    // What is sequenced before this becomes visible to a matching acquire.
    Release,

    // Both halves. A read-modify-write only: there is no load that releases and no store that
    // acquires, which is what `isLoadOrder` and `isStoreOrder` say.
    AcquireRelease,

    // Both halves as applicable, plus membership of the one total order every sequential operation
    // in the program shares. The only order that gives the store-buffer counterexample in §4.4 an
    // answer, and the only one that costs an instruction on x86 where the others do not.
    Sequential,
};

inline StringView nameForOrder(LowerOrder order) {
    switch(order) {
        case LowerOrder::Relaxed:        return "relaxed"_v;
        case LowerOrder::Acquire:        return "acquire"_v;
        case LowerOrder::Release:        return "release"_v;
        case LowerOrder::AcquireRelease: return "acq_rel"_v;
        case LowerOrder::Sequential:     return "seq_cst"_v;
    }

    return "relaxed"_v;
}

// Whether this order has an acquire half - true of `Acquire`, `AcquireRelease` and `Sequential`.
// What a backend asks when deciding whether a barrier is needed after the access.
inline bool isAcquireOrder(LowerOrder order) {
    return order == LowerOrder::Acquire || order == LowerOrder::AcquireRelease
        || order == LowerOrder::Sequential;
}

// Whether this order has a release half. The mirror of the above, and asked before the access.
inline bool isReleaseOrder(LowerOrder order) {
    return order == LowerOrder::Release || order == LowerOrder::AcquireRelease
        || order == LowerOrder::Sequential;
}

// The three a load may carry. A load performs no write, so `Release` and `AcquireRelease` would be
// ordering an effect that does not exist.
inline bool isLoadOrder(LowerOrder order) {
    return order == LowerOrder::Relaxed || order == LowerOrder::Acquire
        || order == LowerOrder::Sequential;
}

// The three a store may carry, for the reason above read the other way round.
inline bool isStoreOrder(LowerOrder order) {
    return order == LowerOrder::Relaxed || order == LowerOrder::Release
        || order == LowerOrder::Sequential;
}

// A read-modify-write admits all five: it both reads and writes, so either half is meaningful and
// so is neither.
inline bool isUpdateOrder(LowerOrder) {
    return true;
}

// A fence admits four. There is no relaxed fence: an operation whose entire content is an ordering
// edge, carrying the order that adds no edge, is a written statement that does nothing.
inline bool isFenceOrder(LowerOrder order) {
    return order != LowerOrder::Relaxed;
}

/*
 * The order the failed comparison of a compare-exchange performs - Analysis-Atomics.md §3.5.
 *
 * A failure performs no write, so a release half has nothing to order and is dropped; everything
 * else is carried across. This is the same projection C++ uses for its one-order overload, and it
 * is here rather than in the library because the two-order form in `Advanced` reaches the same
 * instruction and the verifier has to hold both to one rule.
 */
inline LowerOrder failureOrderFor(LowerOrder success) {
    switch(success) {
        case LowerOrder::Relaxed:        return LowerOrder::Relaxed;
        case LowerOrder::Acquire:        return LowerOrder::Acquire;
        case LowerOrder::Release:        return LowerOrder::Relaxed;
        case LowerOrder::AcquireRelease: return LowerOrder::Acquire;
        case LowerOrder::Sequential:     return LowerOrder::Sequential;
    }

    return LowerOrder::Relaxed;
}

/*
 * Whether an explicitly stated failure order is legal beside this success order - §3.5's table.
 *
 * The rule is that the failure order may not be stronger than the success order, where "stronger"
 * is the ordering `relaxed < acquire < sequential` on the three a load may carry. A failure path
 * that synchronized more than the successful one would be a compare-exchange whose *loss* published
 * more than its win, which no algorithm wants and no target implements without the strong form.
 */
inline bool isLegalFailureOrder(LowerOrder success, LowerOrder failure) {
    if(!isLoadOrder(failure)) return false;

    switch(success) {
        case LowerOrder::Relaxed:
            return failure == LowerOrder::Relaxed;
        case LowerOrder::Acquire:
        case LowerOrder::AcquireRelease:
            return failure == LowerOrder::Relaxed || failure == LowerOrder::Acquire;
        case LowerOrder::Release:
            return failure == LowerOrder::Relaxed;
        case LowerOrder::Sequential:
            return true;
    }

    return false;
}

/*
 * Which read-modify-write an `AtomicRmw` performs - Analysis-Atomics.md §3.4.
 *
 * Six rather than the five fetch operations, because an exchange is the same instruction with the
 * old value simply discarded on the way in: `xchg` and `lock xadd` differ in the opcode and in
 * nothing else the IR carries, and LLVM spells both `atomicrmw`. Keeping them one kind is what lets
 * every pass ask "does this write the location" once.
 *
 * Named for the operation and not for the instruction, in the naming `LowerMinMax` uses: `Sub` is
 * `lock xadd` of a negated operand on x86 and `atomicrmw sub` on LLVM, and neither spelling belongs
 * in the shared IR.
 */
enum class LowerAtomicOp: U8 {
    Exchange,
    Add,
    Sub,
    And,
    Or,
    Xor,
};

inline StringView nameForAtomicOp(LowerAtomicOp op) {
    switch(op) {
        case LowerAtomicOp::Exchange: return "xchg"_v;
        case LowerAtomicOp::Add:      return "add"_v;
        case LowerAtomicOp::Sub:      return "sub"_v;
        case LowerAtomicOp::And:      return "and"_v;
        case LowerAtomicOp::Or:       return "or"_v;
        case LowerAtomicOp::Xor:      return "xor"_v;
    }

    return "add"_v;
}

/*
 * What a pass may ask about a *kind* rather than about an instruction - see inst.def, where the
 * answer for each kind is a column.
 *
 * Each of these used to be a switch of its own in one pass or two, and several of them in three or
 * four that had drifted apart: `writesStorage` was written once in lower_cse.cpp, once in
 * lower_licm.cpp and once in lower_recover.cpp with a comment saying it had to be the same one, and
 * the three named different sets. The cost of a switch is never the lines - it is that a kind added
 * to the IR is silently absent from every one it does not reach, and "absent" reads as `false`.
 */

// Produces its results and nothing else, so an instance nothing reads may simply go. Not the same
// claim as `kLowerRepeatable` below and strictly weaker: an `Imm` is pure and is left where it is.
static constexpr U16 kLowerPure = 1 << 0;

// May be computed again, at another point, or not at all - which is what a hoist, a rematerialization
// and a common-subexpression unification each need. Implies `kLowerPure`, and is left off everything
// whose position is a decision an earlier pass took (see `X86Address`) and everything that reads
// storage somebody else may be writing.
static constexpr U16 kLowerRepeatable = 1 << 1;

// The operands may be exchanged without changing what is computed. Stated at the kind, so a caller
// whose target only exchanges some of the types it applies to - a float `add` propagates the NaN
// payload of a particular side - asks this and then asks about the type.
static constexpr U16 kLowerCommutative = 1 << 2;

// `(a . b) . c` is `a . (b . c)`, which with the bit above and an identity is what an accumulator
// may be carried through - see lower_tail.cpp.
static constexpr U16 kLowerAssociative = 1 << 3;

// May read memory the program can also write.
static constexpr U16 kLowerReads = 1 << 4;

// May write memory something else can read. Every kind with this also answers `writesStorage`.
static constexpr U16 kLowerWrites = 1 << 5;

/*
 * Establishes an ordering edge nothing may be moved across, whatever it does to memory itself.
 *
 * The six atomics, and it is a separate bit from the two above rather than a stronger reading of
 * them because an atomic *load* has it: what an acquire load acquires is the right to see writes
 * another thread published, so a value carried across one is a value read before the edge and used
 * after it. A fence has it with no location at all, and a spin hint has it because a load hoisted
 * out of a polling loop is the one rewrite that turns a spin into a hang.
 */
static constexpr U16 kLowerOrdered = 1 << 6;

/*
 * The allocation carries a trailing array past the struct named in the row, so `sizeof` is not the
 * whole of it and the instruction cannot be copied flat.
 *
 * A phi's source blocks, a shuffle's lane pattern, and the operand lists of a call, a return and an
 * intrinsic. What this exists to prevent is a pass that copies an instruction wholesale reading the
 * size from one list and the permission from another - see `lowerInstSize`.
 */
static constexpr U16 kLowerVariadic = 1 << 7;

/*
 * Carries nothing beside its operands and `flags`.
 *
 * Which is to say two instructions of this kind, with equal `flags` and equal operands, compute the
 * same thing - the question lower_merge.cpp asks of two copies of an exit block. A kind without it
 * has a field of its own that has to be compared by name, or a trailing array that has to be, and
 * the safe answer for a kind that says nothing is "no".
 */
static constexpr U16 kLowerPlain = 1 << 8;

// One of the four dividing operations. What `mayFault` is asked about, and what a peephole checks
// before pricing a move.
static constexpr U16 kLowerDivides = 1 << 9;

// May read the machine's flags rather than the condition operand it names - a select or a branch
// whose comparison a backend folded into it. Nothing may be lifted over a comparison past one.
static constexpr U16 kLowerUsesFlags = 1 << 10;

/*
 * Operates on the bits of its operands, with no lane, rounding or NaN semantics of its own.
 *
 * Which makes it exact at *every* type whose bits they are, a float and a float vector included -
 * `v & ~0` is `v` down to a NaN's payload, and `v & 0` is `+0.0`. That is a stronger statement than
 * `kLowerCommutative` and a different one: a float `add` is commutative in value and this backend
 * still will not exchange its operands, because `addps` takes a NaN's payload from a particular
 * side. See `isCommutativeInt` in codegen/x64/transform_address.cpp, which is what asks.
 */
static constexpr U16 kLowerBitwise = 1 << 11;

// Only valid in kernel mode. Nothing in the compiler asks - a program that writes one has already
// been type-checked against a declaration only a kernel build has - but it is a fact about the
// operation, and the place for it is beside the rest of them.
static constexpr U16 kLowerPrivileged = 1 << 12;

// A pure computation that carries nothing beside its operands and `flags`, which is what most rows
// in inst.def are - writing the three out on each of them would bury the ones that differ.
static constexpr U16 kLowerArith = kLowerPure | kLowerRepeatable | kLowerPlain;

/*
 * The arity a kind does not have - see the `used` and `created` columns in inst.def.
 *
 * A call, a return, a phi, an intrinsic and an x86 address each answer from the instruction rather
 * than from the kind, and there is no number that would be right for them. `validateLowerInst`
 * skips the column for these and their own arm checks what the arity is actually held to.
 */
static constexpr U8 kAnyArity = 0xff;

struct LowerInstTraits {
    StringView mnemonic;

    // How large an allocation of this kind is, or 0 for one that carries a trailing array - see
    // `kLowerVariadic`. Taken from the `Struct` column, so it cannot disagree with the type a
    // consumer casts to.
    U16 size;

    U16 flags;

    // How many operands a kind reads and how many values it defines, or `kAnyArity`. Checked once
    // for every instruction in `validateLowerInst`, which is what took the restatement of it out of
    // the fourteen arms that opened with one.
    U8 used, created;
};

// Indexed by `LowerInst::Kind`, in the order inst.def lists them - which is the order the enum is
// generated in, so the two cannot come apart.
extern const LowerInstTraits kLowerInstTraits[];

/*
 * One place in memory an instruction touches, as the instruction itself states it.
 *
 * Where it is and how far it reaches, which between them are what every "could these two be the same
 * bytes" question is asked of. An instruction names one of these per address it holds: four kinds
 * name one, a copy names two - its source and its destination - and everything else names none.
 *
 * The address is the *slot* rather than a copy of it, for the reason resolve's `placeAt` gives one:
 * a pass that redirects an access and a pass that reads one are then one declaration, and cannot
 * come to disagree about which operand of a copy the destination is.
 *
 * The extent is `bytes` where the instruction states a width and `count` where an operand holds it -
 * a copy and a fill of a length computed at run time. Exactly one of the two is set, and a caller
 * that cannot fold `count` to a constant has an access whose extent it does not know, which is not
 * the same thing as an access of nothing.
 */
struct LowerAccess {
    LowerPtr<LowerValue>* address = nullptr;
    U64 bytes = 0;
    LowerPtr<LowerValue> count = nullptr;
};

// A single operation that can be performed inside a function block.
struct LowerInst {
    /*
     * The instruction kinds, generated from inst.def - which is where each one is documented and
     * where its properties are stated. See that file for why the roster and the enum are one list.
     */
    enum Kind: U8 {
#define YANA_LOWER_INST(kind, Struct, mnemonic, used, created, flags) kind,
#include "inst.def"
#undef YANA_LOWER_INST
    };

    // The number of kinds, which is what the trait table is checked against - see inst.cpp.
    static constexpr Size kKindCount =
#define YANA_LOWER_INST(kind, Struct, mnemonic, used, created, flags) + 1
#include "inst.def"
#undef YANA_LOWER_INST
    ;

    /*
     * The families, as bounds on that order rather than as enumerators inside it.
     *
     * A marker is a fact about where the rows sit rather than a kind of its own, and inst.def has
     * one row per kind - so these are named here, immediately below the generated list, where the
     * order they describe is visible. Every one of them is what a predicate below reads: `isUnary`
     * is a pair of comparisons rather than a switch precisely because the rows are grouped, and
     * that grouping is why inst.def says a row may be added to the end of a family and never
     * reordered.
     */
    static constexpr Kind FirstInst = Nop;
    static constexpr Kind FirstUnary = Set;
    static constexpr Kind FirstUnaryArith = Neg;
    static constexpr Kind LastUnaryArith = Round;
    static constexpr Kind LastUnary = LastUnaryArith;
    static constexpr Kind FirstBinary = Add;
    static constexpr Kind LastBinary = Cmp;
    static constexpr Kind FirstVector = VecSplat;
    static constexpr Kind LastVector = VecReduce;
    static constexpr Kind FirstAtomic = AtomicLoad;
    static constexpr Kind LastAtomic = SpinHint;
    static constexpr Kind FirstTerminator = Je;
    static constexpr Kind LastTerminator = Unreachable;
    static constexpr Kind LastInst = X86LowBit;

    explicit LowerInst(Kind kind): kind(kind) {}

    LowerPtr<LowerBlock> block = nullptr; // Block this instruction belongs to.
    LocationId source = kNullLocation;     // Reference to original source code location.
    U16 liveId = kNullLive;                // Index into liveness information.
    Kind kind;                             // Instruction type.
    U8 createdCount = 0;                   // Number of values created (embedded values packed into the same allocation after this header).
    U8 usedCount = 0;                      // Number of values used (pointers packed into the same allocation after created values).
    U8 flags = 0;                          // Type-specific content.

    Buffer<LowerValue> created() {
        auto created = (LowerValue*)(this + 1);
        return { created, createdCount };
    }

    Buffer<LowerPtr<LowerValue>> used() {
        auto created = (LowerValue*)(this + 1);
        auto used = (LowerPtr<LowerValue>*)(created + createdCount);
        return { used, usedCount };
    }

    /*
     * ## The memory this instruction touches, as the answer a kind that touches none inherits
     *
     * Declared here so that "this instruction names no address" is what a struct means by saying
     * nothing, and overridden - by plain hiding, since none of this is virtual - beside the fields
     * each one is about. Which operand of a copy is the destination is a fact about `LowerInstCopy`,
     * and a column in inst.def naming the field would be a second spelling of the struct.
     *
     * The point is that a consumer never writes a switch over kinds to ask. `visitLowerInstruction`
     * turns a kind into its concrete type once, from inst.def, and `lowerInstAccesses` is one loop
     * over whatever that type declares - so a kind that touches memory reaches every pass that walks
     * accesses rather than the ones whose switch was updated. Both passes that walk them had their
     * own list, neither list had the atomics on it, and both then read "an address I do not
     * understand" as a reason to give up on the function.
     *
     * `kAccessCount` is what the flags already say in the coarse form: a kind with an access is a
     * kind with `kLowerReads` or `kLowerWrites`. The converse does not hold - a call, an intrinsic
     * and a fence touch storage and name no address - and a pass that walks accesses to account for
     * everything a function does to memory has to treat that difference as the refusal it is.
     */
    static constexpr Size kAccessCount = 0;

    LowerAccess accessAt(Size) { return {}; }
};

/*
 * The row for a kind, and the questions asked through it.
 *
 * Everything below is one read of one table. The point is not that it is faster than a switch - it
 * is that adding an instruction is a row in inst.def and nothing else, so a kind cannot be missing
 * from a pass that was written before it existed.
 */
inline const LowerInstTraits& lowerInstTraits(LowerInst::Kind kind) {
    assertTrue(Size(kind) < LowerInst::kKindCount);
    return kLowerInstTraits[Size(kind)];
}

inline const LowerInstTraits& lowerInstTraits(LowerInst* inst) {
    return lowerInstTraits(inst->kind);
}

/*
 * The flags an instruction answers with, which for all but one kind are its row's.
 *
 * The exception is `Intrinsic`, whose row cannot say: "emit this operation" covers a population
 * count and a `clflush`, and one answer for both has to be the pessimistic one. So an intrinsic
 * carries its own flags in the registry beside its arity (`LowerIntrinsicDesc`) and this reads them
 * from there - which is what lets a `popcnt` be hoisted out of a loop and a `clflush` retire every
 * load in scope, where before every intrinsic did the second and none did the first.
 *
 * Declared here and defined at the bottom of this file, `LowerInstIntrinsic` being declared between.
 */
inline U16 lowerInstFlags(LowerInst* inst);

inline bool hasLowerTrait(LowerInst* inst, U16 trait) {
    return (lowerInstFlags(inst) & trait) != 0;
}

// Produces its results and nothing else - see kLowerPure.
inline bool isPure(LowerInst* inst) {
    return hasLowerTrait(inst, kLowerPure);
}

// May be computed again, elsewhere, or not at all - see kLowerRepeatable.
inline bool isRepeatable(LowerInst* inst) {
    return hasLowerTrait(inst, kLowerRepeatable);
}

// Commutative in the sense every pass here needs: the operands may be compared, or exchanged, as a
// pair rather than in order. A comparison is not one of these - swapping its operands is a different
// comparison unless the relation is swapped with them, and there is nothing here to swap it against.
inline bool isCommutative(LowerInst* inst) {
    return hasLowerTrait(inst, kLowerCommutative);
}

/*
 * Whether this may have written storage some earlier load read.
 *
 * The whole memory model this tier has, and deliberately the coarsest one: any write retires every
 * load in scope rather than the loads that could alias it. There is no place information here - a
 * `Load` names an address that arithmetic produced, and whether two addresses are the same is the
 * question a pass is being asked rather than one it can answer - so an alias rule would be either
 * "the same address", which is already the unification test, or a guess.
 *
 * The name is "writes storage" and what it is actually asked is "may a load answered from above
 * this still be answered from above it", which is why every ordered operation says yes including
 * the atomic load: what an acquire acquires is precisely the right to see writes another thread
 * published. See kLowerOrdered.
 */
inline bool writesStorage(LowerInst* inst) {
    return hasLowerTrait(inst, kLowerWrites | kLowerOrdered);
}

// Whether anything this instruction does could read *or* write storage - the same question one
// answer wider, for the passes that have to see a read as well as a write.
inline bool touchesStorage(LowerInst* inst) {
    return hasLowerTrait(inst, kLowerReads | kLowerWrites | kLowerOrdered);
}

/*
 * How large an allocation of this kind is, or 0 for a kind that carries a trailing array past its
 * struct and therefore cannot be copied flat - see kLowerVariadic.
 *
 * This and "may this be duplicated at all" used to be two switches in codegen/x64/transform_loop.cpp,
 * and a kind named in one but not the other was a copy of the wrong number of bytes.
 */
inline Size lowerInstSize(LowerInst* inst) {
    return lowerInstTraits(inst).size;
}

inline bool isCast(LowerInst* inst) {
    return inst->kind == LowerInst::Cast || inst->kind == LowerInst::Bitcast;
}

inline bool isUnary(LowerInst* inst) {
    return inst->kind >= LowerInst::FirstUnary && inst->kind <= LowerInst::LastUnary;
}

inline bool isUnaryArith(LowerInst* inst) {
    return inst->kind >= LowerInst::FirstUnaryArith && inst->kind <= LowerInst::LastUnaryArith;
}

inline bool isBinary(LowerInst* inst) {
    return inst->kind >= LowerInst::FirstBinary && inst->kind <= LowerInst::LastBinary;
}

inline bool isTerminator(LowerInst* inst) {
    return inst->kind >= LowerInst::FirstTerminator && inst->kind <= LowerInst::LastTerminator;
}

inline bool isCall(LowerInst* inst) {
    return inst->kind == LowerInst::Call;
}

// One of the five that only a vector value can be an operand or a result of. Everything else a
// vector does is one of the instructions above, with vector operands.
inline bool isVectorInst(LowerInst* inst) {
    return inst->kind >= LowerInst::FirstVector && inst->kind <= LowerInst::LastVector;
}

inline bool isPhi(LowerInst* inst) {
    return inst->kind == LowerInst::Phi;
}

// One of the six an atomic program is written out of. Every one of them carries `kLowerOrdered`, so
// what a pass asks before deciding it may move something across one is `writesStorage` above; this
// is for the places that need the family itself, which are the verifier and the backends.
inline bool isAtomicInst(LowerInst* inst) {
    return inst->kind >= LowerInst::FirstAtomic && inst->kind <= LowerInst::LastAtomic;
}

struct LowerInstSingle: LowerInst {
    LowerInstSingle(Kind kind, StringId name, LowerType type): LowerInst(kind), result(this, type, name) {
        createdCount = 1;
    }

    // Embedded values must be first after the base class.
    LowerValue result;
};

struct LowerInstGlobal: LowerInstSingle {
    LowerInstGlobal(StringId name, LowerPtr<LowerGlobal> target):
        LowerInstSingle(Global, name, LowerType::Pointer), target(target) {}

    LowerPtr<LowerGlobal> target;
};

struct LowerInstFun: LowerInstSingle {
    LowerInstFun(StringId name, LowerPtr<LowerFunction> target):
        LowerInstSingle(Fun, name, LowerType::Pointer), target(target) {}

    LowerPtr<LowerFunction> target;
};

// A value provided through a function parameter.
struct LowerArg: LowerInstSingle {
    LowerArg(StringId name, LowerType type, U8 index):
        LowerInstSingle(Arg, name, type)
    {
        flags = index;
    }

    U32 getIndex() const {
        return flags;
    }
};

// An immediate value that can be used by instructions.
struct LowerImm: LowerInstSingle {
    LowerImm(StringId name, LowerType type, U64 i):
        LowerInstSingle(Imm, name, type)
    {
        this->i = i;
    }

    LowerImm(StringId name, LowerType type, F64 f):
        LowerInstSingle(Imm, name, type)
    {
        this->f = f;
    }

    union {
        U64 i;
        F64 f;
    };
};

/*
 * Arithmetic.
 */

struct LowerInstUnary: LowerInstSingle {
    LowerInstUnary(Kind kind, StringId name, LowerType type, LowerPtr<LowerValue> from):
        LowerInstSingle(kind, name, type), from(from)
    {
        assertTrue(kind >= LowerInst::FirstUnary && kind <= LowerInst::LastUnary);
        usedCount = 1;
    }

    // Used values must be first after embedded values.
    LowerPtr<LowerValue> from;
};

struct LowerInstCast: LowerInstUnary {
    LowerInstCast(StringId name, LowerType type, LowerPtr<LowerValue> from, bool signedSource, bool signedResult):
        LowerInstUnary(Cast, name, type, from)
    {
        U8 f = 0;
        if(signedSource) f |= 1;
        if(signedResult) f |= 2;

        flags = f;
    }

    bool isSignedSource() const {
        return flags & 1;
    }

    bool isSignedResult() const {
        return flags & 2;
    }

    // Whether the source already holds the result's representation, so the extension or truncation
    // this cast describes needs no instruction of its own. Recorded by the backend rather than by
    // the lowering - what it means is a question about registers - in the same way a Copy carries
    // its chosen encoding and a Select its folded comparison. See trySkipCastExtend in the x64
    // transform, which is the only thing that sets it.
    bool skipsExtend() const {
        return flags & 4;
    }

    void setSkipsExtend(bool skips) {
        flags = U8((flags & ~4) | (skips ? 4 : 0));
    }
};

struct LowerInstBinary: LowerInstSingle {
    /*
     * Set on a division that answers a zero divisor by *trusting a test above it* rather than by
     * building the answer itself - see `divisorKnownNonZero` in lower/lower_divide.cpp.
     *
     * Every other division this compiler emits is total, which is what lets the passes below treat
     * the four dividing operations as ordinary arithmetic. One with this bit is the exception, and
     * the exception is exactly a *position*: it cannot fault where it stands, and would where the
     * test does not reach. So the only reader is the one pass that moves a computation to a point it
     * did not run at - `mayFault` in lower_licm.cpp - and the bit says "not above this branch"
     * rather than "may trap".
     *
     * Read only on Div/IDiv/Rem/IRem, where `flags` is otherwise unused: `LowerInstCmp` has the low
     * four bits and its own kind, and `LowerInstCast` is a unary.
     */
    static constexpr U8 kTrustsDivisorTest = 0x40;

    bool trustsDivisorTest() const { return (flags & kTrustsDivisorTest) != 0; }
    void setTrustsDivisorTest() { flags |= kTrustsDivisorTest; }

    LowerInstBinary(StringId name, LowerType type, LowerPtr<LowerValue> lhs, LowerPtr<LowerValue> rhs, Kind kind):
        LowerInstSingle(kind, name, type), lhs(lhs), rhs(rhs)
    {
        assertTrue(kind >= LowerInst::FirstBinary && kind <= LowerInst::LastBinary);
        usedCount = 2;
    }

    // Used values must be first after embedded values.
    LowerPtr<LowerValue> lhs, rhs;
};

struct LowerInstCmp: LowerInstBinary {
    // The result type is a parameter rather than a constant because a comparison of two vectors
    // answers a mask of their shape where one of two scalars answers a Bool - which is the whole of
    // what a vector comparison changes about this instruction (§3.1 of Implementation-Vector.md).
    LowerInstCmp(StringId name, LowerPtr<LowerValue> lhs, LowerPtr<LowerValue> rhs, LowerCmp cmp,
                 LowerType type = LowerType::Int32):
        LowerInstBinary(name, type, lhs, rhs, Cmp)
    {
        flags = (U8)cmp;
    }

    LowerCmp getCmp() const {
        return (LowerCmp)(flags & kCmpMask);
    }

    // Rewrites the comparison in place, for a target that answers it the other way round: the x64
    // backend turns `a < b` into `b > a` because only one of the two reads correctly for a NaN.
    void setCmp(LowerCmp cmp) {
        flags = U8((flags & ~kCmpMask) | (U8)cmp);
    }

    /*
     * Whether the answer to this comparison is already in the machine's flags where it stands, so
     * that it is carried rather than performed.
     *
     * `a - 1 != 0` is the shape: the subtraction set the zero flag from its own result, and the
     * comparison below it recomputes what is standing right there. Set by the x64 compare folding
     * (`tryElideCompare` in codegen/x64/transform_peephole.cpp) and read by its form selection; nothing
     * outside that backend writes it, and nothing anywhere reads it to mean anything else.
     *
     * It lives here for the same reason `LowerInstJe::setEmbeddedCmp` and `LowerValue::Implicit` do:
     * what it records is which instruction ends up producing a value, and the instructions are where
     * the backends write that down. It is deliberately *not* printed - a fold that changes no
     * operand and no edge has nothing to say in the IR text.
     */
    bool getFlagsLive() const {
        return (flags & kFlagsLive) != 0;
    }

    void setFlagsLive() {
        flags |= kFlagsLive;
    }

private:
    static constexpr U8 kCmpMask = 0x0f;   // ten comparison kinds, so four bits hold them
    static constexpr U8 kFlagsLive = 0x80;
};

/*
 * Whether a computation may fault where it is being moved to.
 *
 * One bit on one family, and it is *not* the old "a division can trap" rule this replaces. Every
 * division `makeDivisionTotal` leaves is total and moves freely - that is most of what defining
 * `x / 0` bought. The exception is the division that pass left unguarded because a test above it had
 * already settled the divisor: it cannot fault where it stands and would above the test, so it is
 * the one computation whose safety is a property of its *position* rather than of its operands.
 * See LowerInstBinary::kTrustsDivisorTest, which is set nowhere else.
 *
 * Nothing else marked `kLowerRepeatable` can fault: a shift count is masked, a float operation
 * answers a NaN, and a vector operation is lane-wise arithmetic.
 */
inline bool mayFault(LowerInst* inst) {
    if(!hasLowerTrait(inst, kLowerDivides)) return false;
    return ((LowerInstBinary*)inst)->trustsDivisorTest();
}

inline Maybe<LowerCmp> decodeOptionalCmp(U8 flags) {
    if(flags & 1) {
        return Just(LowerCmp(flags >> 1));
    } else {
        return Nothing();
    }
}

inline U8 encodeOptionalCmp(Maybe<LowerCmp> cmp) {
    return cmp ? (U8(cmp.unwrap()) << 1) | 1 : 0;
}

struct LowerInstSelect: LowerInstSingle {
    LowerInstSelect(StringId name, LowerPtr<LowerValue> lhs, LowerPtr<LowerValue> rhs, LowerPtr<LowerValue> cmp, LowerType type):
        LowerInstSingle(Select, name, type), lhs(lhs), rhs(rhs), cmp(cmp)
    {
        usedCount = 3;
    }

    // Set if the comparison uses retained flags, rather than a register-stored comparison.
    // In this case, `cmp` is only used implicitly.
    Maybe<LowerCmp> getEmbeddedCmp() const {
        return decodeOptionalCmp(flags);
    }

    void setEmbeddedCmp(Maybe<LowerCmp> c) {
        flags = encodeOptionalCmp(c);
    }

    // Used values must be first after embedded values.
    LowerPtr<LowerValue> lhs, rhs, cmp;
};

/*
 * `a * b + c`, at most once rounded - see the kind's note above.
 *
 * Three used values and one type: all three operands and the result are the same float, or the same
 * vector of floats. There is nothing to embed and nothing optional, which is why this is a struct of
 * its own rather than a Binary with a third field bolted on.
 */
struct LowerInstFma: LowerInstSingle {
    LowerInstFma(StringId name, LowerType type, LowerPtr<LowerValue> a, LowerPtr<LowerValue> b,
                 LowerPtr<LowerValue> c):
        LowerInstSingle(Fma, name, type), a(a), b(b), c(c)
    {
        usedCount = 3;
    }

    // Used values must be first after embedded values.
    LowerPtr<LowerValue> a, b, c;
};

/*
 * Which of the SHA extension's two-operand instructions a `ShaBinary` is.
 *
 * The same nine as `ShaOp` in resolve/inst.h and a second enum rather than that one, on
 * `LowerReduce`'s terms: the two tiers do not include each other's headers, and a seam that has to
 * translate is a seam that cannot silently drift when one side gains a member.
 */
enum class LowerSha: U8 {
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

// The nine as text - what the lower printer writes and what a `.lower` fixture reads back.
inline StringView nameOfLowerSha(LowerSha op) {
    switch(op) {
        case LowerSha::Sha1Msg1:    return "sha1msg1"_v;
        case LowerSha::Sha1Msg2:    return "sha1msg2"_v;
        case LowerSha::Sha1NextE:   return "sha1nexte"_v;
        case LowerSha::Sha1Rounds0: return "sha1rnds4_0"_v;
        case LowerSha::Sha1Rounds1: return "sha1rnds4_1"_v;
        case LowerSha::Sha1Rounds2: return "sha1rnds4_2"_v;
        case LowerSha::Sha1Rounds3: return "sha1rnds4_3"_v;
        case LowerSha::Sha256Msg1:  return "sha256msg1"_v;
        default:                    return "sha256msg2"_v;
    }
}

/*
 * The SHA extension's two-operand instructions - `LowerInst::ShaBinary`.
 *
 * Two used values, one type, and the instruction itself in `flags`. All three of the operands and
 * the result are a four-lane vector of 32-bit words, which the resolve verifier has already checked
 * and which is the register the machine reads.
 *
 * `flags` and not a field past the operands, which is `VecShuffle`'s mistake read the other way: a
 * value there is compared by `sameCarriedData` for nothing, so two of these that are different
 * instructions over the same pair of vectors are told apart by CSE without a case being written.
 */
struct LowerInstShaBinary: LowerInstSingle {
    LowerInstShaBinary(StringId name, LowerType type, LowerPtr<LowerValue> lhs, LowerPtr<LowerValue> rhs,
                       LowerSha op):
        LowerInstSingle(ShaBinary, name, type), lhs(lhs), rhs(rhs)
    {
        usedCount = 2;
        flags = (U8)op;
    }

    LowerSha getSha() const { return (LowerSha)flags; }

    // Used values must be first after embedded values.
    LowerPtr<LowerValue> lhs, rhs;
};

/*
 * `sha256rnds2` - the one SHA instruction with three operands.
 *
 * `state` is the four working words it advances, `feed` is the four it reads them against, and
 * `keys` is the message words with their round constants already added. On the machine that third
 * operand is **implicitly xmm0**, which is a constraint the x64 form carries and nothing at this
 * tier has to know: here it is an ordinary third use.
 */
struct LowerInstSha256Rounds: LowerInstSingle {
    LowerInstSha256Rounds(StringId name, LowerType type, LowerPtr<LowerValue> state,
                          LowerPtr<LowerValue> feed, LowerPtr<LowerValue> keys):
        LowerInstSingle(Sha256Rounds, name, type), state(state), feed(feed), keys(keys)
    {
        usedCount = 3;
    }

    // Used values must be first after embedded values.
    LowerPtr<LowerValue> state, feed, keys;
};

/*
 * Vectors.
 */

/*
 * Which reduction a VecReduce performs.
 *
 * One kind with a field rather than six kinds, so that `any`, `all`, `horizontalSum` and the rest
 * are one instruction to fold, to cost and to expand. The signed pair follows the naming the binary
 * operations already use: `Min` is the unsigned or floating one and `IMin` the signed integer one,
 * exactly as `Div` and `IDiv` are.
 *
 * The order of a floating-point reduction is a stated language property rather than an
 * implementation detail - see Design-Vector §4.5 - so this says *what* is combined and the
 * expansion owes the pairwise tree that says in which order.
 */
enum class LowerReduce: U8 {
    Add,
    Mul,
    Min,
    IMin,
    Max,
    IMax,
    And,
    Or,

    /*
     * The lanes of a *mask*, as bits of an integer - lane `i` in bit `i`, and nothing above the lane
     * count.
     *
     * Not a combination at all, which is why it sits after the eight rather than among them: it is
     * the one thing a machine can do to a mask that the eight are then arithmetic on. `any` is it
     * against zero, `all` is it against the full pattern, `count` is its population and `firstSet`
     * is its lowest set bit - four answers off one instruction, where each of the four expanded on
     * its own is a reduction tree.
     *
     * **Written by a backend for itself, and never by anything above one.** `x86` has `pmovmskb` and
     * ARM does not, so a target that lacks the instruction never sees this kind rather than having
     * to expand it; the portable spelling of all four stays what `simd.cpp` emits. The validator
     * still states its rule, the printer still names it and the parser still reads it back, because
     * an IR this backend hands to its own next pass has to be one that round-trips.
     */
    Bits,

    /*
     * The lowest set lane of a mask, or the lane count where none is set.
     *
     * The portable one of the pair, and the reason `Bits` may stay private: a lane index is a thing
     * every backend can answer and no two answer alike, so what crosses this boundary is the
     * question rather than one target's way of asking it. x64 reads it off the movemask above with a
     * bit scan, LLVM bitcasts the `<N x i1>` to an integer and counts its trailing zeros, and the
     * JavaScript backend - where a lane is a variable and there is no mask to scan - emits the chain
     * of conditionals that is the same answer.
     *
     * Its result is an `Int32` and not the lane's scalar form, for the reason `Bits` is one: a mask
     * of thirty-two `i8` lanes answers 32 where nothing is set, which is not a value an `i8` holds.
     */
    FirstSet,
};

// Every lane of the result is the same scalar. The source is the lane type's scalar form - see
// scalarFormOf, which is what an 8- or 16-bit lane arrives in.
struct LowerInstVecSplat: LowerInstSingle {
    LowerInstVecSplat(StringId name, LowerType type, LowerPtr<LowerValue> from):
        LowerInstSingle(VecSplat, name, type), from(from)
    {
        usedCount = 1;
    }

    // Used values must be first after embedded values.
    LowerPtr<LowerValue> from;
};

/*
 * One lane read out of a vector, and a vector with one lane written into it.
 *
 * Two kinds sharing a struct, the way Init and Assign do above: the second is the first plus the
 * value to write, and every pass that reasons about the lane index reasons about both.
 */
struct LowerInstVecLane: LowerInstSingle {
    // `vlane v, i`: the result is the lane's scalar form.
    LowerInstVecLane(StringId name, LowerType type, LowerPtr<LowerValue> from, U8 lane):
        LowerInstSingle(VecLane, name, type), from(from), value(nullptr)
    {
        usedCount = 1;
        flags = lane;
    }

    // `vwithlane v, i, x`: the result is the vector.
    LowerInstVecLane(StringId name, LowerType type, LowerPtr<LowerValue> from, U8 lane, LowerPtr<LowerValue> value):
        LowerInstSingle(VecWithLane, name, type), from(from), value(value)
    {
        usedCount = 2;
        flags = lane;
    }

    U8 getLane() const { return flags; }

    // Used values must be first after embedded values. `value` is not one of them for a VecLane,
    // which states usedCount = 1 and leaves it null.
    LowerPtr<LowerValue> from, value;
};

/*
 * Lanes selected from two vectors by a constant pattern.
 *
 * The pattern has one entry per lane of the *result*, each naming a lane of the concatenation of the
 * two sources: `i < lanes` is a lane of the first and `i >= lanes` a lane of the second. A shuffle
 * within one vector names the same value twice, which is what every backend expects to see and what
 * keeps the pattern's meaning independent of how many sources were meant.
 *
 * The pattern is stored past the used values, the way a phi stores its source blocks: it is as long
 * as the result has lanes, so it is not a fixed field, and it belongs to the instruction's own
 * allocation rather than to a list somewhere else.
 */
struct LowerInstVecShuffle: LowerInstSingle {
    LowerInstVecShuffle(StringId name, LowerType type, LowerPtr<LowerValue> left, LowerPtr<LowerValue> right):
        LowerInstSingle(VecShuffle, name, type), left(left), right(right)
    {
        usedCount = 2;
    }

    // One entry per lane of the result. Written by whoever allocated the instruction, which has to
    // have reserved `type.lanes()` bytes past the used values for it.
    Buffer<U8> pattern() {
        auto u = used();
        auto p = (U8*)(u.ptr + u.length);

        // The pattern starts where this instruction ends, which is what the allocation reserving
        // `patternBytes` past the struct assumes. Both halves of that are stated in one place, so a
        // field added above cannot silently move the pattern into the middle of itself.
        assertTrue(p == (U8*)this + sizeof(LowerInstVecShuffle));
        return { p, result.type.lanes() };
    }

    // How much an allocation of one of these needs beyond the struct itself.
    static Size patternBytes(LowerType type) { return type.lanes(); }

    // Used values must be first after embedded values.
    LowerPtr<LowerValue> left, right;
};

// Every lane combined into one scalar, in the pairwise order Design-Vector §4.5 states. The result
// is the lane type's scalar form; for a mask it is an Int32, so `any` is Or, `all` is And and the
// number of lanes set is Add.
struct LowerInstVecReduce: LowerInstSingle {
    LowerInstVecReduce(StringId name, LowerType type, LowerPtr<LowerValue> from, LowerReduce reduce):
        LowerInstSingle(VecReduce, name, type), from(from)
    {
        usedCount = 1;
        flags = (U8)reduce;
    }

    LowerReduce getReduce() const { return (LowerReduce)flags; }

    // Used values must be first after embedded values.
    LowerPtr<LowerValue> from;
};

/*
 * Memory.
 */

// Allocates space on the stack.
struct LowerInstAlloca: LowerInstSingle {
    LowerInstAlloca(StringId name, LowerPtr<LowerValue> byteCount, U32 alignment):
        LowerInstSingle(Alloca, name, LowerType::Pointer), byteCount(byteCount), alignment(alignment)
    {
        usedCount = 1;
        assertTrue(alignment != 0 && (alignment & (alignment - 1)) == 0); // a power of two
    }

    // Used values must be first after embedded values.
    LowerPtr<LowerValue> byteCount;

    // What the allocated address has to be a multiple of. Stated by whoever knows what the memory is
    // going to hold rather than guessed from the byte count downstream: a 16-byte allocation of two
    // pointers wants 8, and a 4-byte one of a vector lane wants 16, and the size does not say which.
    // A power of two, and at least 1.
    U32 alignment;
};

inline bool isSignedLoad(U8 memFlags) {
    return memFlags & 8;
}

/*
 * That a load reads up to a vector's width past the end of what it names, deliberately.
 *
 * A vector loop's last iteration reads a whole register where only part of it is wanted, and the
 * tail-read guarantee is what makes that safe: storage the language allocated has room after it, so
 * the bytes past the end are unspecified rather than unmapped (Design-Vector §5.4). This flag is how
 * an instruction says it is relying on that, and it is load-bearing in three places above the
 * backend - the range analysis must not keep a bounds check for it, provenance must treat it as
 * reading the place it names and nothing more, and the verifier checks that the place is rooted in
 * storage that carries the guarantee.
 *
 * Below the backend it means one further thing, which is why it is carried into the lower IR at all:
 * the legalizer must not narrow such a load, and nothing may conclude anything about the bytes it
 * read past the end.
 *
 * No store ever carries it, which is why this is on the load's flags and not in makeMemoryFlags.
 */
inline bool isOverreadLoad(U8 memFlags) {
    return memFlags & 16;
}

inline U32 getMemoryWidth(U8 memFlags) {
    return 1 << (memFlags & 7);
}

inline U8 makeMemoryFlags(U32 width, bool isSigned) {
    auto flags = width ? Math::findFirstBit(width) : 0;
    if(isSigned) flags |= 8;
    return flags;
}

// Loads a value from memory into a register.
// The source must be a pointer type.
// The target type determines the number of bytes loaded from memory.
struct LowerInstLoad: LowerInstSingle {
    LowerInstLoad(LowerPtr<LowerValue> from, StringId name, LowerType type, U32 width, bool isSigned):
        LowerInstSingle(Load, name, type), from(from)
    {
        usedCount = 1;
        flags = makeMemoryFlags(width, isSigned);
    }

    U32 getWidth() const {
        return getMemoryWidth(flags);
    }

    bool isSigned() const {
        return isSignedLoad(flags);
    }

    bool isOverread() const {
        return isOverreadLoad(flags);
    }

    void setOverread() {
        flags |= 16;
    }

    // Used values must be first after embedded values.
    LowerPtr<LowerValue> from;

    static constexpr Size kAccessCount = 1;

    LowerAccess accessAt(Size) { return { &from, getWidth() }; }
};

// Stores a value from a register into memory.
// The value stored into must be a pointer.
struct LowerInstStore: LowerInst {
    LowerInstStore(LowerPtr<LowerValue> to, LowerPtr<LowerValue> value, U32 width):
        LowerInst(Store), to(to), value(value)
    {
        usedCount = 2;
        flags = makeMemoryFlags(width, false);
    }

    U32 getWidth() const {
        return getMemoryWidth(flags);
    }

    // Used values must be first after embedded values.
    LowerPtr<LowerValue> to, value;

    static constexpr Size kAccessCount = 1;

    LowerAccess accessAt(Size) { return { &to, getWidth() }; }
};

/*
 * A location read, combined with a value and written back in place - see LowerInst::X86StoreOp.
 *
 * Written exactly as a store is, and the two differ in one field: `op` says which operation combines
 * what is there with what arrives. It is one of the five the machine can perform through its r/m
 * field - Add, Sub, And, Or, Xor - and `getWidth` is the store's width, which is the width the
 * operation is performed at as well.
 */
struct LowerInstX86StoreOp: LowerInst {
    LowerInstX86StoreOp(LowerPtr<LowerValue> to, LowerPtr<LowerValue> value, U32 width, Kind op):
        LowerInst(X86StoreOp), to(to), value(value), op(op)
    {
        usedCount = 2;
        flags = makeMemoryFlags(width, false);
    }

    U32 getWidth() const {
        return getMemoryWidth(flags);
    }

    Kind getOp() const {
        return op;
    }

    // Used values must be first after embedded values.
    LowerPtr<LowerValue> to, value;

    // Which of the five, as the binary instruction kind it was folded out of.
    Kind op;

    static constexpr Size kAccessCount = 1;

    LowerAccess accessAt(Size) { return { &to, getWidth() }; }
};

// Copies memory from the source pointer to the target pointer.
struct LowerInstCopy: LowerInst {
    LowerInstCopy(LowerPtr<LowerValue> to, LowerPtr<LowerValue> from, LowerPtr<LowerValue> count):
        LowerInst(Copy), to(to), from(from), count(count)
    {
        usedCount = 3;
    }

    // Used values must be first after embedded values.
    LowerPtr<LowerValue> to, from, count;

    static constexpr Size kAccessCount = 2;

    // The destination first, which is the order the operands stand in and the order the printer
    // emits them. Neither end states a width: what a copy moves is `count` bytes, and a caller that
    // cannot fold it to a constant knows the address and not the extent.
    LowerAccess accessAt(Size which) { return { which == 0 ? &to : &from, 0, count }; }
};

// Copies a pattern byte to the target pointer.
// `vzeroupper` - see `LowerInst::VZeroUpper`. Nothing to carry: no operands, no result, no fields.
struct LowerInstVZeroUpper: LowerInst {
    LowerInstVZeroUpper(): LowerInst(VZeroUpper) {}
};

struct LowerInstSetPattern: LowerInst {
    LowerInstSetPattern(LowerPtr<LowerValue> to, LowerPtr<LowerValue> count, LowerPtr<LowerValue> pattern):
        LowerInst(SetPattern), to(to), count(count), pattern(pattern)
    {
        usedCount = 3;
    }

    // Used values must be first after embedded values.
    // The declaration order is what used() reports, and the printer emits used() positionally, so
    // it has to match the textual operand order the parser accepts: `setpattern to, count, pattern`.
    LowerPtr<LowerValue> to, count, pattern;

    static constexpr Size kAccessCount = 1;

    LowerAccess accessAt(Size) { return { &to, 0, count }; }
};

/*
 * Atomics - Analysis-Atomics.md §5.1. See LowerInst::FirstAtomic for why they are kinds.
 *
 * All four location operations carry their width in `flags` through `makeMemoryFlags`, which is the
 * same encoding `Load` and `Store` use and is read back with `getWidth`. The width is the access,
 * not the register: a narrow atomic touches exactly its declared one or two bytes and the result is
 * extended afterwards under the ordinary narrow-integer rule.
 *
 * The order is a member rather than more bits of `flags`, and it comes *after* the used pointers.
 * That placement is load-bearing rather than stylistic: `created()` and `used()` read the bytes
 * immediately following the header positionally, so anything declared between the base class and
 * the last operand would be read as an operand.
 */

// An indivisible read of `getWidth()` bytes - `load atomic`, `mov`, `ldar`.
struct LowerInstAtomicLoad: LowerInstSingle {
    LowerInstAtomicLoad(LowerPtr<LowerValue> from, StringId name, LowerType type, U32 width,
                        bool isSigned, LowerOrder order):
        LowerInstSingle(AtomicLoad, name, type), from(from), order(order)
    {
        usedCount = 1;
        flags = makeMemoryFlags(width, isSigned);
    }

    U32 getWidth() const { return getMemoryWidth(flags); }
    bool isSigned() const { return isSignedLoad(flags); }

    // Used values must be first after embedded values.
    LowerPtr<LowerValue> from;

    // One of the three `isLoadOrder` admits.
    LowerOrder order;

    static constexpr Size kAccessCount = 1;

    LowerAccess accessAt(Size) { return { &from, getWidth() }; }
};

// An indivisible write of `getWidth()` bytes - `store atomic`, `mov` or `xchg`, `stlr`.
struct LowerInstAtomicStore: LowerInst {
    LowerInstAtomicStore(LowerPtr<LowerValue> to, LowerPtr<LowerValue> value, U32 width, LowerOrder order):
        LowerInst(AtomicStore), to(to), value(value), order(order)
    {
        usedCount = 2;
        flags = makeMemoryFlags(width, false);
    }

    U32 getWidth() const { return getMemoryWidth(flags); }

    // Used values must be first after embedded values.
    LowerPtr<LowerValue> to, value;

    // One of the three `isStoreOrder` admits.
    LowerOrder order;

    static constexpr Size kAccessCount = 1;

    LowerAccess accessAt(Size) { return { &to, getWidth() }; }
};

/*
 * A location read, combined with a value and written back, indivisibly - `atomicrmw`, `lock xadd`.
 *
 * The result is the value from *before* the update, which is what §3.4 fixes and what both x86 and
 * LLVM hand back without a second instruction. An exchange is the same instruction with `op` set to
 * `Exchange`, the previous value simply being the whole of what it answers.
 *
 * **No sign flag, where the load has one.** A narrow signed update - `Atomic(I8).fetchAdd` - answers
 * a value that has to be sign-extended into its register, and that extension is written as an
 * ordinary `Cast` after the instruction rather than as a bit on it. The load carries the flag
 * because `Load` does and the two have to read alike; a read-modify-write has no such twin, and
 * three more mnemonics to spell the signed halves of `add`, `sub`, `and`, `or` and `xor` would buy
 * an instruction that both backends fold away.
 *
 * Nothing may promote a weaker order to a stronger one here even where the target's instruction is
 * physically stronger. A locked RMW on x86 is a full barrier whatever `order` says, and rewriting a
 * relaxed one to sequential in the IR would make the same program behave differently on AArch64 and
 * would forbid motion that is legal - see §5.3.
 */
struct LowerInstAtomicRmw: LowerInstSingle {
    LowerInstAtomicRmw(LowerPtr<LowerValue> to, LowerPtr<LowerValue> value, StringId name,
                       LowerType type, U32 width, LowerAtomicOp op, LowerOrder order):
        LowerInstSingle(AtomicRmw, name, type), to(to), value(value), op(op), order(order)
    {
        usedCount = 2;
        flags = makeMemoryFlags(width, false);
    }

    U32 getWidth() const { return getMemoryWidth(flags); }

    // Used values must be first after embedded values.
    LowerPtr<LowerValue> to, value;

    LowerAtomicOp op;

    // Any of the five: an operation that both reads and writes can carry either half, or neither.
    LowerOrder order;

    static constexpr Size kAccessCount = 1;

    LowerAccess accessAt(Size) { return { &to, getWidth() }; }
};

/*
 * Compare and exchange - `cmpxchg`, `lock cmpxchg`, the `ldaxr`/`stlxr` pair.
 *
 * **Two results, and the second is not `previous == expected`.** For the strong form it would be,
 * which is why this looked like one result for a while. The weak form is the one that makes it two:
 * it may fail spuriously, answering the value it was handed and *not* having stored, and a caller
 * that reconstructed success by comparing would read that as a win. So the flag is carried
 * separately, which is also the shape LLVM's `cmpxchg` returns and the shape x86 leaves in ZF.
 *
 * `previous` is always the value read, on both paths. Neither form mutates its expected operand and
 * neither loses the observed value on failure - §3.4.
 *
 * Both orders are always stored. `success` is what the exchange performs when it stores; `failure`
 * is what the comparison performs when it does not, and is held to `isLegalFailureOrder`.
 *
 * §3.5's derivation is *not* applied here. It is a rule about what a caller may leave unsaid, and
 * the library's one-order form runs it through `failureOrderFor` on the way in; both that form and
 * `Advanced`'s two-order one therefore reach this instruction with the pair already chosen. Leaving
 * the derivation out means this IR has one shape, the verifier has one rule, and a dump says what
 * the failure path does rather than what it can be computed to do.
 */
struct LowerInstAtomicCas: LowerInst {
    LowerInstAtomicCas(StringId previousName, StringId exchangedName, LowerType type,
                       LowerPtr<LowerValue> to, LowerPtr<LowerValue> expected,
                       LowerPtr<LowerValue> desired, U32 width, bool weak,
                       LowerOrder success, LowerOrder failure, LowerType flagType = LowerType::Int32):
        LowerInst(AtomicCas),
        previous(this, type, previousName), exchanged(this, flagType, exchangedName),
        to(to), expected(expected), desired(desired),
        success(success), failure(failure), weak(weak)
    {
        createdCount = 2;
        usedCount = 3;
        flags = makeMemoryFlags(width, false);
    }

    U32 getWidth() const { return getMemoryWidth(flags); }

    // Embedded values must be first after the base class, and in this order: `created()` reads them
    // positionally, so `previous` is result 0 and `exchanged` is result 1 everywhere.
    LowerValue previous, exchanged;

    // Used values must be first after embedded values.
    LowerPtr<LowerValue> to, expected, desired;

    LowerOrder success, failure;

    // Whether a spurious failure is permitted. The form for a retry loop on a machine whose
    // primitive is a load-linked/store-conditional pair, where the strong form costs a loop of its
    // own; on x86 the two lower identically and the flag only stops a caller from assuming.
    bool weak;

    static constexpr Size kAccessCount = 1;

    LowerAccess accessAt(Size) { return { &to, getWidth() }; }
};

/*
 * An ordering edge attached to no location - `fence`, `mfence`, `dmb`.
 *
 * It does not by itself synchronize two threads: the surrounding atomic operations still have to
 * establish the relation, and a fence between two plain accesses orders nothing that was racing
 * anyway. What it does is let a release be spelled apart from the store it publishes, which is the
 * shape a reclamation algorithm's last-release path needs.
 */
struct LowerInstFence: LowerInst {
    explicit LowerInstFence(LowerOrder order): LowerInst(Fence), order(order) {}

    // One of the four `isFenceOrder` admits.
    LowerOrder order;
};

/*
 * That the loop containing this is polling - `pause`, `yield`, or nothing.
 *
 * Neither a compiler fence nor a memory fence, and it establishes no happens-before edge: a program
 * whose correctness changes when every `SpinHint` is deleted was already wrong. It is an
 * instruction rather than an intrinsic only so that it sits beside the five above it and is
 * excluded from motion by the same predicate; what it costs a backend is one opcode.
 */
struct LowerInstSpinHint: LowerInst {
    LowerInstSpinHint(): LowerInst(SpinHint) {}
};

// Stores one argument into the outgoing argument area, for a call whose convention passes it on the
// stack rather than in a register.
//
// It exists to break the argument's lifetime. Written straight into the call's operand list, a stack
// argument would have to stay in a register from wherever it was computed all the way to the call,
// competing for registers with every other argument being computed in between - which is exactly
// where a call with more arguments than registers is under the most pressure. Storing it early ends
// its live range at the store, and only memory holds it from there on.
//
// That is also why this has to be an instruction rather than a move attached to some other one:
// liveness runs over instructions, so the store has to be visible to it to shorten anything.
//
// The result stands in for the argument in the call's operand list so that the call still names all
// of its arguments in order. It is implicit - nothing reads it, and it occupies no location.
struct LowerInstX86PushArg: LowerInstSingle {
    LowerInstX86PushArg(LowerPtr<LowerValue> arg, U32 stackOffset, LowerType type):
        LowerInstSingle(X86PushArg, StringId(), type), arg(arg), stackOffset(stackOffset)
    {
        result.flags = LowerValue::Implicit;
        usedCount = 1;
    }

    // Used values must be first after embedded values.
    LowerPtr<LowerValue> arg;

    // Byte offset within the argument area, as the calling convention assigned it.
    U32 stackOffset;
};

/*
 * Which minimum or maximum a `X86MinMax` performs.
 *
 * The naming the binary operations and `LowerReduce` already use: `Min` is the unsigned or floating
 * one and `IMin` the signed integer one, exactly as `Div` and `IDiv` are. A float lane is `Min` -
 * there being one ordering of floats - and the signedness of an integer lane is the lane type's.
 */
enum class LowerMinMax: U8 {
    Min,
    IMin,
    Max,
    IMax,
};

// The lane-wise minimum or maximum of two vectors - see LowerInst::X86MinMax, which states why this
// is the backend's own instruction and why its operand order may not be exchanged at a float lane.
struct LowerInstX86MinMax: LowerInstSingle {
    LowerInstX86MinMax(StringId name, LowerType type, LowerPtr<LowerValue> lhs, LowerPtr<LowerValue> rhs,
                       LowerMinMax kind):
        LowerInstSingle(X86MinMax, name, type), lhs(lhs), rhs(rhs)
    {
        usedCount = 2;
        flags = U8(kind);
    }

    LowerMinMax getMinMax() const { return (LowerMinMax)flags; }

    bool isMax() const {
        auto kind = getMinMax();
        return kind == LowerMinMax::Max || kind == LowerMinMax::IMax;
    }

    // Whether the lanes are read as signed, which for an integer lane is what picks `pminsd` over
    // `pminud`. A float lane answers false and nothing asks: `minps` is the only float ordering.
    bool isSignedLanes() const {
        auto kind = getMinMax();
        return kind == LowerMinMax::IMin || kind == LowerMinMax::IMax;
    }

    // Used values must be first after embedded values.
    LowerPtr<LowerValue> lhs, rhs;
};

// The widening multiply of the even 32-bit lanes - see LowerInst::X86MulWide, which states why this
// is the backend's own instruction and why its signedness is not the type's.
struct LowerInstX86MulWide: LowerInstSingle {
    LowerInstX86MulWide(StringId name, LowerType type, LowerPtr<LowerValue> lhs,
                        LowerPtr<LowerValue> rhs, bool signedLanes):
        LowerInstSingle(X86MulWide, name, type), lhs(lhs), rhs(rhs)
    {
        usedCount = 2;
        flags = signedLanes ? 1 : 0;
    }

    // Whether the 32-bit lanes are widened as signed, which is `pmuldq` against `pmuludq`.
    bool isSignedLanes() const { return flags != 0; }

    // Used values must be first after embedded values.
    LowerPtr<LowerValue> lhs, rhs;
};

// The sign of a narrow value put back into the whole register - see LowerInst::X86Sext, which states
// why the width read is the instruction's own and not its operand type's.
struct LowerInstX86Sext: LowerInstSingle {
    LowerInstX86Sext(StringId name, LowerType type, LowerPtr<LowerValue> from, U8 sourceBytes):
        LowerInstSingle(X86Sext, name, type), from(from)
    {
        usedCount = 1;
        flags = sourceBytes;
    }

    // How many bytes of the operand are read: 1, 2 or 4. See LowerInst::X86Sext.
    U8 sourceBytes() const { return flags; }

    // Used values must be first after embedded values.
    LowerPtr<LowerValue> from;
};

// `~lhs & rhs` - see LowerInst::X86AndNot, which states why the complemented operand is named by
// the instruction rather than left to an operand order.
struct LowerInstX86AndNot: LowerInstSingle {
    LowerInstX86AndNot(StringId name, LowerType type, LowerPtr<LowerValue> lhs, LowerPtr<LowerValue> rhs):
        LowerInstSingle(X86AndNot, name, type), lhs(lhs), rhs(rhs)
    {
        usedCount = 2;
    }

    // Used values must be first after embedded values. `lhs` is the complemented one.
    LowerPtr<LowerValue> lhs, rhs;
};

/*
 * Which of the three lowest-bit operations an `X86LowBit` performs - see the kind.
 *
 * Named for the answer rather than for the instruction, which is the naming `LowerMinMax` above
 * uses and the naming `Core.Bits` uses at the language end: `Clear` is `blsr`, `Isolate` is `blsi`
 * and `Mask` is `blsmsk`, and each name says what the result *is* rather than which three letters
 * encode it.
 */
enum class LowerX86LowBit: U8 {
    Clear,   // the operand with its lowest set bit cleared - `x & (x - 1)`
    Isolate, // the lowest set bit alone - `x & -x`
    Mask,    // every bit below the lowest set one, and that one - `x ^ (x - 1)`
};

// One of the three lowest-bit operations - see LowerInst::X86LowBit.
struct LowerInstX86LowBit: LowerInstSingle {
    LowerInstX86LowBit(StringId name, LowerType type, LowerPtr<LowerValue> from, LowerX86LowBit which):
        LowerInstSingle(X86LowBit, name, type), from(from)
    {
        usedCount = 1;
        flags = U8(which);
    }

    LowerX86LowBit getLowBit() const { return (LowerX86LowBit)flags; }

    // Used values must be first after embedded values.
    LowerPtr<LowerValue> from;
};

// A vector permuted by a vector of lane indices - see LowerInst::X86Permute. The operand order is
// the machine's: `vpermd ymm1, ymm2, ymm3` reads the indices out of `ymm2`, which the encoding puts
// in `vvvv`, and the vector being permuted out of the r/m operand.
struct LowerInstX86Permute: LowerInstSingle {
    LowerInstX86Permute(StringId name, LowerType type, LowerPtr<LowerValue> indices,
                        LowerPtr<LowerValue> from):
        LowerInstSingle(X86Permute, name, type), indices(indices), from(from)
    {
        usedCount = 2;
    }

    // Used values must be first after embedded values.
    LowerPtr<LowerValue> indices, from;
};

// A vector masked by a mask of its own shape - see LowerInst::X86MaskAnd, which states why the
// operand order is the machine's and why the complemented form takes them the other way round.
struct LowerInstX86MaskAnd: LowerInstSingle {
    LowerInstX86MaskAnd(StringId name, LowerType type, LowerPtr<LowerValue> lhs,
                        LowerPtr<LowerValue> rhs, bool complemented):
        LowerInstSingle(X86MaskAnd, name, type), lhs(lhs), rhs(rhs)
    {
        usedCount = 2;
        flags = complemented ? 1 : 0;
    }

    // Whether the mask is the arm that is *dropped* rather than the one that is kept, which is
    // `pandn` against `pand`. The mask is then `lhs`, that being the operand `pandn` complements.
    bool isComplemented() const { return flags != 0; }

    // Used values must be first after embedded values.
    LowerPtr<LowerValue> lhs, rhs;
};

// Represents an address calculation (base + index * scale) + displacement.
// Used with two different instruction kinds:
//  - X86Address: purely embedded into whatever instruction uses it (Load/Store) - never
//    materialized into a register of its own, so its result is always Implicit.
//  - X86Lea: materializes the computed address into a real register (LEA), e.g. for pointer
//    arithmetic that doesn't immediately feed a Load/Store.
struct LowerInstX86Address: LowerInstSingle {
    LowerInstX86Address(LowerInst::Kind kind, StringId name, LowerPtr<LowerValue> base, LowerPtr<LowerValue> index, U8 scale, U32 displacement):
        LowerInstSingle(kind, name, LowerType::Pointer),
        first(base ? base : index), second(base && index ? index : nullptr),
        displacement(displacement), scale(scale),
        hasBase(base != nullptr), hasIndex(index != nullptr)
    {
        assertTrue(kind == LowerInst::X86Address || kind == LowerInst::X86Lea);

        usedCount = U8((hasBase ? 1 : 0) + (hasIndex ? 1 : 0));

        if(kind == LowerInst::X86Address) {
            result.flags |= LowerValue::Implicit;
        }
    }

    // The operand slots, named by position rather than by role. used() is one contiguous buffer, so
    // an address with no base - the no-base SIB form, `[index*scale + disp32]` - holds its index in
    // the first slot: a hole where the absent base would have been is a null operand that every
    // consumer walking used() would dereference. Read them through base() and index() below.
    LowerPtr<LowerValue> first, second;

    /*
     * `[rip + g]` instead of a computed address: a global named in the encoding rather than a
     * pointer held in a register.
     *
     * A field rather than an operand because that is exactly what it is not - the form has nothing
     * to place, and a `Global` instruction feeding this would be a value the allocator had to find a
     * register for. Set only where base and index are both absent, since the rip-relative form has
     * neither field.
     *
     * What it buys is one instruction where there were two: a pooled constant read once becomes
     * `addsd xmm, [rip + k]` rather than a load into a register and an add of it.
     */
    LowerPtr<LowerGlobal> symbol = nullptr;

    U32 displacement;
    U8 scale;
    bool hasBase;
    bool hasIndex;

    LowerPtr<LowerValue> base() const { return hasBase ? first : nullptr; }
    LowerPtr<LowerValue> index() const { return hasIndex ? (hasBase ? second : first) : nullptr; }
};

/*
 * Intrinsics.
 *
 * A machine operation the program asked for by name, rather than one the lowering derived from
 * something more abstract. The instructions above all mean something a target-independent optimizer
 * could reason about; an intrinsic means "emit this operation", and what it does to the machine is
 * the target's description of it rather than anything stated here.
 *
 * So this carries an identifier and its operands, and nothing else. Everything an intrinsic needs in
 * order to be allocated and emitted - which registers it forces its operands into, what it clobbers,
 * which of its results goes where, how it is encoded - is one row of the target's intrinsic registry
 * (see codegen/x64/intrinsic.cpp). Adding one is that row plus a name here.
 *
 * The identifiers are named centrally, the way LowerCallType names conventions the lower IR does not
 * itself implement: an intrinsic is meaningless without a target, but the IR still has to be able to
 * write one down, print it and parse it back.
 */
enum class LowerIntrinsic: U16 {
    // A byte reversal is *not* here, and the absence is the point: `bswap` is `LowerInst::Bswap`, an
    // ordinary unary this IR's own passes can fold, hoist and combine. An intrinsic is opaque to all
    // three - see the note on that kind, and §3.1.3 of codegen/x64/README.md for the memory forms
    // that opacity would also have cost.
    Popcnt,  // count the set bits of an integer

    /*
     * The number of zero bits below the lowest set bit of an integer, in two kinds that differ only
     * in what a zero operand answers - which is the one thing about this operation that is not the
     * same everywhere, and so is the one thing the IR has to say out loud.
     *
     * `Cttz` is **undefined at zero**, which is `llvm.cttz`'s poison zero and x86's `bsf`. An
     * emitter owes it a non-zero operand.
     *
     * `CttzWidth` **answers the operand's width at zero** - 32 for an `i32`, 64 for an `i64` - which
     * is `tzcnt` and needs the feature level that has it. It is not a convenience: a mask whose bits
     * fill their word has no bit above them to mark, so an operand that may be zero is the shape
     * rather than an oversight, and the width is exactly the answer wanted. See `expandMaskFirstSet`,
     * which spends the sentinel where one fits and this where none does.
     */
    Cttz,
    CttzWidth,

    /*
     * The other end of the word, in the same two kinds and for the same reason - and the pair is
     * *not* symmetric with the one above, which is x86's doing rather than a choice made here.
     *
     * `Bsr` answers the **index of the highest set bit**, not a count of anything: 31 for an `i32`
     * whose top bit is set, 0 for the value 1, and nothing at all for zero. That is `bsr`, it is
     * baseline, and the leading-zero count a caller wants is `width - 1 - Bsr(x)` - one subtraction
     * that the expansion below pays and the instruction above does not.
     *
     * `ClzWidth` answers the **leading-zero count** with the operand's width at zero, which is
     * `lzcnt` and needs LZCNT (v3, beside BMI1). It is what `Value::LeadingZeros` becomes, and
     * `expandBitScans` in the x64 backend is what turns it back into `Bsr` and a correction on a
     * target that has no such level.
     *
     * There is deliberately no poison-at-zero leading count to pair with `Cttz`: nothing produces
     * one. `Bsr` is here because the expansion needs a name for what it emits, and it is written
     * only by a backend that has the instruction - `LowerIntrinsic::Bzhi`'s case exactly, and
     * refused by the LLVM backend the same way.
     */
    Bsr,
    ClzWidth,

    /*
     * The low `index` bits of a value, with everything above them cleared - `bzhi`, BMI2.
     *
     * Two operands, the value and the bit count, in that order. **An index at or above the value's
     * width clears nothing**, which is the machine's rule and is stated here because a caller
     * relying on the other reading would be relying on nothing: the index is read from the low byte
     * of its operand, so a count of 256 is a count of zero and a negative one is a count of 255.
     * Whoever emits this owes it an index it has narrowed itself.
     *
     * Written only by a backend that has the instruction, for the reason `LowerReduce::Bits` is:
     * without it this is three general-register instructions and a constant, and a target with a
     * different answer should write its own rather than expand somebody else's.
     */
    Bzhi,

    /*
     * The two directions of an arbitrary bit permutation - `pext` and `pdep`, BMI2.
     *
     * `Pext` packs the bits of its first operand at the set positions of its second down into a
     * contiguous low field; `Pdep` spreads a low field back out into those positions. Both are total
     * and neither has a rule the machine and the IR disagree about, which is the difference from
     * `Bzhi` above: a permutation has no count for a byte to truncate.
     *
     * Written only by a backend that has the instructions, on `Bzhi`'s terms and refused by the LLVM
     * backend the same way. `LowerInst::GatherBits` and `LowerInst::ScatterBits` are what every tier
     * above a backend carries instead, and `expandBitOperations` in the x64 backend is what turns
     * one into the other where the feature is present - and into the parallel-suffix network where
     * it is not.
     *
     * **A caution that belongs with the instruction rather than with any caller**: these two are
     * microcoded on AMD's Zen 1 and Zen 2, at tens of cycles against the three a Zen 3 or an Intel
     * part takes. A build that names a feature level gets what it asked for; nothing here tries to
     * detect a vendor.
     */
    Pext,
    Pdep,
    Cpuid,   // query the processor's feature information
    Rdtscp,  // read the processor's timestamp counter and its id
    Rdtsc,   // read the processor's timestamp counter

    // Memory ordering. Neither reads nor writes anything itself; each one constrains where the
    // accesses around it may be moved to, which is why the IR has to be able to name them at all.
    MFence,
    LFence,
    SFence,
    Pause,   // a hint that this is a spin loop, not a barrier - see the registry

    // Cache and translation control. Each takes the address it operates on, and only that.
    Prefetch,     // fetch a line towards the processor, with no architectural effect
    PrefetchNta,  // the same, without displacing what the caches already hold
    Clflush,      // write a line back and evict it, everywhere it is held
    Invlpg,       // drop the translation of one page

    // Interrupts and processor state. The four below take nothing and answer nothing: what they do
    // is to the processor rather than to any value.
    Hlt,
    Cli,
    Sti,
    Swapgs,

    // Model-specific and extended-state registers.
    Rdmsr,
    Wrmsr,
    Xgetbv,

    // Port I/O, at the two widths a port is addressed at. The port number is a value rather than a
    // constant here; see the registry for why that is not the shorter encoding's fault.
    In8,
    In32,
    Out8,
    Out32,

    // Control registers. One intrinsic per register rather than one taking a number, because the
    // number is part of the encoding rather than an operand of it.
    ReadCr0,
    ReadCr2,
    ReadCr3,
    ReadCr4,
    WriteCr0,
    WriteCr3,
    WriteCr4,

    /*
     * Starting a thread, which is the one intrinsic here that is not an instruction.
     *
     * It is several - a system call, a branch, and the child's whole entry sequence - and it is an
     * intrinsic rather than a library function because none of it can be written above a backend.
     * The child begins with its own stack pointer and the *parent's* every other register, so any
     * code the compiler generated for the enclosing frame reads the wrong frame in it; the entry has
     * to be reached before the child touches memory at all. See PseudoKind::CloneThread.
     */
    CloneThread,

    LastIntrinsic = CloneThread,
};

static constexpr Size kLowerIntrinsicCount = Size(LowerIntrinsic::LastIntrinsic) + 1;

/*
 * What the IR itself knows about an intrinsic: how it is written, how many values go in and come
 * out, and what it does beyond them.
 *
 * The arity is here rather than in the target registry because it is what the parser and the
 * validator check, and both run before any target has been chosen. The flags are here for a sharper
 * reason: they are what every optimization pass reads, and the pass runs before a target has been
 * chosen too. They used to be stated only in codegen/x64/intrinsic.cpp, where nothing read them, and
 * what every pass at this tier assumed instead was that an intrinsic reads and writes all of memory
 * - so `popcnt` was pinned inside the loop that called it and `clflush` retired nothing.
 *
 * The same `kLower*` bits an instruction row carries, and the ones that mean anything here are
 * `kLowerPure`, `kLowerRepeatable`, `kLowerReads`, `kLowerWrites`, `kLowerOrdered` and
 * `kLowerPrivileged`. An intrinsic is never `kLowerPlain` - it carries its identifier past its
 * operands - and never `kLowerVariadic` in the sense the row means, its operand count being fixed by
 * this table rather than by the instruction.
 */
struct LowerIntrinsicDesc {
    StringView name;
    U8 results;
    U8 args;
    U16 flags;
};

const LowerIntrinsicDesc& lowerIntrinsicDesc(LowerIntrinsic id);

// Looks an intrinsic up by the name it is written as, hashed as every identifier is. Nothing if
// there is no such intrinsic. This is what makes the table in lower.cpp the one statement of which
// intrinsics exist: the parser registers a handler per name from it and recovers the identifier
// through here, so adding a row makes an intrinsic writable with no line of its own anywhere.
Maybe<LowerIntrinsic> findLowerIntrinsic(StringId name);

struct LowerInstIntrinsic: LowerInst {
    LowerInstIntrinsic(LowerIntrinsic intrinsic, Size createdCount, Size usedCount):
        LowerInst(Intrinsic), intrinsic(intrinsic)
    {
        this->createdCount = createdCount;
        this->usedCount = usedCount;
    }

    LowerIntrinsic getIntrinsic() const { return intrinsic; }

    // LowerInst::used contains the operand list and LowerInst::created the results, both in the
    // order the intrinsic's description states.
    LowerIntrinsic intrinsic;
};

/*
 * `movbe r, [address]` and `movbe [address], r` - see LowerInst::X86MovbeLoad.
 *
 * Written exactly as the `Load` and `Store` they were folded out of, one field lighter: there is no
 * sign to extend, a reversal being defined only where the value is the whole of its register.
 */
struct LowerInstX86MovbeLoad: LowerInstSingle {
    LowerInstX86MovbeLoad(LowerPtr<LowerValue> from, StringId name, LowerType type, U32 width):
        LowerInstSingle(X86MovbeLoad, name, type), from(from)
    {
        usedCount = 1;
        flags = makeMemoryFlags(width, false);
    }

    U32 getWidth() const { return getMemoryWidth(flags); }

    // Used values must be first after embedded values.
    LowerPtr<LowerValue> from;

    static constexpr Size kAccessCount = 1;

    LowerAccess accessAt(Size) { return { &from, getWidth() }; }
};

struct LowerInstX86MovbeStore: LowerInst {
    LowerInstX86MovbeStore(LowerPtr<LowerValue> to, LowerPtr<LowerValue> value, U32 width):
        LowerInst(X86MovbeStore), to(to), value(value)
    {
        usedCount = 2;
        flags = makeMemoryFlags(width, false);
    }

    U32 getWidth() const { return getMemoryWidth(flags); }

    // Used values must be first after embedded values.
    LowerPtr<LowerValue> to, value;

    static constexpr Size kAccessCount = 1;

    LowerAccess accessAt(Size) { return { &to, getWidth() }; }
};

struct LowerInstCall: LowerInst {
    explicit LowerInstCall(Size createdCount, Size usedCount, LowerCallType callType): LowerInst(Call) {
        this->createdCount = createdCount;
        this->usedCount = usedCount;
        this->flags = (U8)callType;
    }

    LowerCallType getCallType() const {
        return (LowerCallType)flags;
    }

    // LowerInst::used contains the argument list. The first value is the called function.
    // LowerInst::created contains the returned values.
};

/*
 * Control flow.
 */

// Conditional branch to one of two blocks.
// Can only exist in the terminator field of a block.
struct LowerInstJe: LowerInst {
    LowerInstJe(LowerPtr<LowerValue> cond, LowerPtr<LowerBlock> then, LowerPtr<LowerBlock> otherwise):
        LowerInst(Je), then(then), otherwise(otherwise), cond(cond)
    {
        usedCount = 1;
    }

    // Set if the comparison uses retained flags, rather than a register-stored comparison.
    // In this case, `cond` is only used implicitly.
    Maybe<LowerCmp> getEmbeddedCmp() const {
        return decodeOptionalCmp(flags);
    }

    void setEmbeddedCmp(Maybe<LowerCmp> cmp) {
        flags = encodeOptionalCmp(cmp);
    }

    // Whether either edge carries a claim about how likely it is, as against both being unknown.
    bool hasLikelihood() const {
        return likelihood[0].source != LikelihoodSource::Unknown
            || likelihood[1].source != LikelihoodSource::Unknown;
    }

    LowerPtr<LowerValue> cond;
    LowerPtr<LowerBlock> then;
    LowerPtr<LowerBlock> otherwise;

    // How likely each edge is relative to the other - see EdgeLikelihood. Index 0 is `then` and
    // index 1 is `otherwise`, which is also the order the containing block lists them in
    // `outgoing`, so the two are indexed alike everywhere.
    //
    // Indexed by edge rather than named per target block, which is what makes the metadata survive
    // a CFG transform for free: splitting an edge retargets `then` or `otherwise` in place, and the
    // weight of edge 0 is still the weight of edge 0 when it now leads to the split block instead.
    // The inserted block's own jump has one successor and so needs no weight at all.
    EdgeLikelihood likelihood[2];
};

// Unconditional branch to a different block.
// Can only exist in the terminator field of a block.
struct LowerInstJmp: LowerInst {
    LowerInstJmp(LowerPtr<LowerBlock> then): LowerInst(Jmp), then(then) {}

    LowerPtr<LowerBlock> then;
};

// Return the provided value to the parent function.
// Can only exist in the terminator field of a block.
struct LowerInstRet: LowerInst {
    explicit LowerInstRet(): LowerInst(Ret) {}

    // LowerInst::usedValues contains the returned values list.
    // The list can be empty if the function returns nothing.
};

/*
 * The end of a block nothing arrives at the end of.
 *
 * No operands and no successors, which is what makes every walk in the backend already correct about
 * it: liveness ends here because nothing is live out of a block with no edges, and the frame is
 * never restored because control does not leave. The x64 form encodes zero bytes - see FormNoReturn,
 * which is the only form in the table that emits nothing and is allowed to.
 */
struct LowerInstUnreachable: LowerInst {
    explicit LowerInstUnreachable(): LowerInst(Unreachable) {}
};

// SSA ϕ-node. Can only exist in the list of phi nodes of a block.
struct LowerInstPhi: LowerInstSingle {
    LowerInstPhi(StringId name, LowerType type): LowerInstSingle(Phi, name, type) {}

    // Returns a list of source blocks where each block corresponds to a value in LowerInst::used().
    Buffer<LowerPtr<LowerBlock>> sources() {
        auto u = used();
        return { (LowerPtr<LowerBlock>*)(u.ptr + u.length), u.length };
    }

    // LowerInst::used contains the values to pick from.
    // The list of corresponding blocks is located past the used values list.
};

/*
 * One kind turned into its concrete type, and the only place in the lower IR that does it.
 *
 * `f` is called with a reference of that type, so what it does with the members below is resolved
 * statically: a loop over `kAccessCount` is a loop over a constant, and a kind that touches no
 * memory compiles to nothing at all. Which is what makes this a replacement for the switches rather
 * than an indirection in front of them - the generated code is the same jump table, and the arms are
 * whatever `f` is.
 *
 * Generated from inst.def, so the case list and the enum are one statement. The trailing call is
 * unreachable and exists to give the switch a value on every path; a bare `LowerInst` answers with
 * the defaults, which is the harmless answer to a question about a kind that does not exist.
 */
template<class F>
inline decltype(auto) visitLowerInstruction(LowerInst& inst, F&& f) {
    switch(inst.kind) {
#define YANA_LOWER_INST(kind, Struct, mnemonic, used, created, flags) \
    case LowerInst::kind: return f((Struct&)inst);
#include "inst.def"
#undef YANA_LOWER_INST
    }

    return f(inst);
}

/*
 * Where an instruction touches memory, from the instruction's own declaration - see
 * `LowerInst::accessAt`.
 *
 * Writes them into `target` and returns how many. A copy names two, which is the most any kind
 * names, so `target` needs room for kMaxAccesses.
 *
 * An answer of zero is *not* "this instruction leaves memory alone" - ask `touchesStorage` for that.
 * It is "this instruction names no address I can point at", which a call, an intrinsic and a fence
 * all answer while doing plenty to memory, and which every caller here has to read as a refusal
 * rather than as an absence.
 */
static constexpr Size kMaxAccesses = 2;

inline Size lowerInstAccesses(LowerInst* inst, LowerAccess* target) {
    return visitLowerInstruction(*inst, [&](auto& i) -> Size {
        for(Size n = 0; n < i.kAccessCount; n++) target[n] = i.accessAt(n);
        return i.kAccessCount;
    });
}

// The definition promised beside `hasLowerTrait` - see there for why an intrinsic answers from its
// own row.
inline U16 lowerInstFlags(LowerInst* inst) {
    if(inst->kind == LowerInst::Intrinsic) {
        return lowerIntrinsicDesc(((LowerInstIntrinsic*)inst)->getIntrinsic()).flags;
    }

    return lowerInstTraits(inst).flags;
}

inline LiveSet* Liveness::getBlock(LowerBlock* b) {
    return &blockMap[b->index];
}

inline LowerValue* Liveness::getValue(LiveId id) {
    assertTrue(id < kNullLive);
    return valueMap[id];
}

inline LiveId LowerValue::liveId() {
    auto i = inst();
    auto first = i->liveId;
    if(first == kNullLive) return kNullLive;

    return first + (this - i->created().ptr);
}
