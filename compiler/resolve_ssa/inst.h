#pragma once

#include "../resolve_mir/decl.h"

namespace ssa {

using mir::Occurrence;
using mir::Contract;
using mir::BinaryOp;
using mir::UnaryOp;

struct Inst;
struct Block;
struct Body;

/*
 * What an SSA value is, in ownership terms - Analysis-IR.md §7.1.
 *
 * Four conventions, and every value has exactly one. This is the whole of the ownership model at
 * this tier: MIR has already decided what the source meant, so what is left is a linear invariant
 * small enough that an optimizer pass can be *required* to preserve it rather than asked to.
 *
 *  - `Owned` must be consumed exactly once on every reachable path, including by a drop.
 *  - `Guaranteed` is borrowed, and is valid only while the loan it depends on is live.
 *  - `Trivial` is freely duplicable and freely discardable.
 *  - `Address` is not an owner: it names storage, and its safety comes from an explicit base
 *    dependency or from the unsafe boundary.
 *
 * `Address` is a convention rather than a type so that the question "may this be dropped" has one
 * answer per value. The current resolver derives it from `isMemoryType`, which is what made a
 * pointer-loaded temporary look like a freshly owned aggregate.
 */
enum class Convention: U8 {
    Owned,
    Guaranteed,
    Trivial,
    Address,
};

/*
 * How one instruction uses one value - §7.2.
 *
 * A move is a consuming *edge* rather than a runtime instruction, which is what lets the optimizer
 * forward an owned result without leaving a `Move` node behind to be deleted later. A borrow is a
 * dependency the verifier checks against the loan's extent, and an inspect is a read of something
 * that owns nothing.
 */
enum class UseKind: U8 {
    Consume,
    Borrow,
    Inspect,
};

struct Value;

/*
 * One operand: which value, and what this instruction does with it.
 *
 * A pair rather than a bare handle for the same reason MIR's `Operand` is packed with a mode: the
 * relation "uses" and the relation "consumes" are different, and a representation with only the
 * first has to reconstruct the second.
 */
/*
 * One operand: which value, and what this instruction does with it - in four bytes.
 *
 * A pair rather than a bare handle for the same reason MIR's `Operand` is packed with a mode: the
 * relation "uses" and the relation "consumes" are different, and a representation with only the
 * first has to reconstruct the second.
 *
 * Packed rather than a handle beside a byte, which the compiler laid out as eight - so a `return`
 * with one operand allocated a sixty-four byte list to hold it. The kind rides in the low bits of
 * the handle, which are free because every allocation in these regions is eight-aligned (see
 * `AlignedRegion`); the null handle is offset zero and stays representable, since `Inspect` is zero
 * too.
 */
struct Use {
    static constexpr U32 kKindMask = 7;

    Use() = default;

    Use(SsaPtr<Value> value, UseKind kind) {
        assertTrue((value.offset & kKindMask) == 0);
        assertTrue(U32(kind) <= kKindMask);
        packed = value.offset | U32(kind);
    }

    SsaPtr<Value> value() const { return SsaPtr<Value>(packed & ~kKindMask); }
    UseKind kind() const { return UseKind(packed & kKindMask); }

    bool isSet() const { return (packed & ~kKindMask) != 0; }

    // The same value under a different kind - what an optimizer does when it turns a borrow it
    // proved was the last one into a consume.
    Use withKind(UseKind kind) const { return Use(value(), kind); }

    U32 packed = 0;
};

static_assert(sizeof(Use) == 4, "a Use is one word, and the layout note above says why");

/*
 * A list of operands.
 *
 * Without `SmallList`'s embedded encoding even though a `Use` now fits one, for the reason MIR's
 * `OperandList` gives: the encoding marks a present entry by setting the high bit *in the element*,
 * so an embedded entry is not what the list holds. §13.2 lists the operand encoding as a choice to
 * settle by measurement, and this is the safe end of it until then.
 */
using UseList = SsaList<Use, false>;

/*
 * An SSA value.
 *
 * Separate from the instruction that defines it, because an instruction may define several - a
 * compare-exchange, a call with several results - and a representation where a value *is* its
 * instruction has to grow a second mechanism the first time that happens. `definer` is null for a
 * block argument, whose defining position is the block.
 *
 * The use list is maintained by the editor and by nothing else, which is the discipline §8.5 keeps
 * from the current `IrEditor`: order, use lists and CFG edges are updated together by one API or
 * they disagree.
 */
struct Value {
    Value(const Occurrence& occurrence, Convention convention):
        occurrence(occurrence), convention(convention) {}

    /*
     * The whole occurrence, and not the logical type alone.
     *
     * This is the field the tier boundary turns on. `Convention` below is a four-way summary of what
     * the *linear rule* needs, and it is not a manifestation key: an owned `String` and a shared
     * window into one are both consumed-exactly-once or both not, agree on `type`, and have
     * different representations. A tier that kept only the type and the convention had thrown the
     * difference away by the time lowering asked, which is why every `manifestNative` call below
     * used to pass `ownedContract()` - manifesting a shared window as its owner.
     *
     * Carrying the occurrence keeps `manifest(logical type, contract, target)` answerable from a
     * value in hand, which is Analysis-Borrows-Revised.md §1.3's whole signature.
     */
    Occurrence occurrence;

    // Dense function-local ID: what a dump prints and what a side table is indexed by.
    ValueId id;

    /*
     * Where this value is defined: the instruction that produced it, or the block it is a parameter
     * of. Exactly one of the two is set, and the verifier is what holds them to that - unioning them
     * behind a flag would save four bytes that the eight-byte allocation rounding gives straight
     * back, and cost the two questions being asked separately.
     */
    SsaPtr<Inst> definer = nullptr;
    SsaPtr<Block> block = nullptr;

    /*
     * Which loan a `Guaranteed` value depends on - Analysis-IR.md §7.1's lifetime half.
     *
     * Without it, `Guaranteed` says "this is borrowed" and nothing about *from what*, so "may not
     * outlive the loan it depends on" is a rule with no term to check against. A lens result, a
     * yielded borrow, a value live across a suspension and an external iterator handle are all cases
     * where the loan and the value are not defined in the same place, and the dependency is the only
     * thing that connects them.
     *
     * Invalid for every other convention, and invalid at this milestone for `Guaranteed` too -
     * nothing produces a loan yet. The verifier requires it once loans exist rather than now, and
     * says so where it checks.
     */
    LoanId loan;

    Convention convention;

    SsaList<SsaPtr<Inst>> uses;

    DeclPtr<mir::Type> type() const { return occurrence.type; }
    Contract contract() const { return occurrence.contract; }
};

static constexpr U16 kSsaTerminator = 1;
static constexpr U16 kSsaPure = 2;
static constexpr U16 kSsaConsumes = 4;

struct Edge {
    SsaPtr<Block> target = nullptr;
    UseList arguments;
};

struct Inst {
    enum Kind: U8 {
#define YANA_SSA_INST(kind, Struct, mnemonic, flags) kind,
#include "inst.def"
#undef YANA_SSA_INST
        KindCount,
    };

    Inst(Kind kind, LocationId source): source(source), kind(kind) {}

    InstId id;
    LocationId source = kNullLocation;
    SsaPtr<Block> block = nullptr;
    Kind kind;

    /*
     * The values this instruction defines, in order. Empty for a store, a terminator or a drop.
     *
     * Embedded, which is the normal case rather than the exception: almost every instruction defines
     * exactly one value, and the non-embedding form allocated an eight-entry, thirty-two byte buffer
     * for each of them. Two fit inline, and the compare-exchange and multi-result call that need
     * more spill to a list the way any other list does. Read by value through `get`, never handed out
     * by reference, which is the condition the embedded encoding attaches.
     */
    SsaList<SsaPtr<Value>, true> results;
};

/*
 * A literal, held by value.
 *
 * Not a handle into a body's constant table and not one into the declaration region: a `mir::
 * Constant` is sixteen bytes, and a literal inside a function body has no business outliving the
 * body it is in. Copying it here at conversion is also what makes the MIR region releasable the
 * moment conversion ends - §8.8's rule, which a handle back into MIR's constant table would break.
 */
struct InstConst: Inst {
    InstConst(LocationId source, const mir::Constant& constant):
        Inst(Const, source), constant(constant) {}

    mir::Constant constant;

    template<class F> void eachUse(SsaBase, F&&) {}
    template<class F> void eachEdge(F&&) {}
};

// The address of a module-level `let`. An `Address` value, not an owner: what the global holds is
// the global's, and loading it is a separate instruction with its own convention.
struct InstGlobalAddress: Inst {
    InstGlobalAddress(LocationId source, DeclPtr<mir::Global> global):
        Inst(GlobalAddress, source), global(global) {}

    DeclPtr<mir::Global> global;

    template<class F> void eachUse(SsaBase, F&&) {}
    template<class F> void eachEdge(F&&) {}
};

/*
 * Where an object lives.
 *
 * `mir::StorageClass`, and deliberately not a second enumeration here. The placement decision is
 * ownership analysis' - whether a local escapes its frame, whether it outlives the call that made
 * it, whether an ambient region witness is in scope - and all three are questions about MIR. This
 * tier carries the answer to the allocation instruction; declaring the lattice again here would be
 * a second place to add a rung to, which is how one of them ends up missing one.
 */
using mir::StorageClass;

/*
 * Storage for one local, as an `Address` value.
 *
 * The address is not an owner - what the storage holds is, and the store that put it there is what
 * the linear rule counts. That separation is the whole reason `Address` is a convention: a slot
 * whose contents were moved out is the same slot, and a representation where the address itself
 * looked owned would have to drop it.
 *
 * Promotion out of memory is deliberately not attempted here. The lower IR's own promotion pass runs
 * over these on the way to machine code, so a scalar local that is never borrowed costs nothing in
 * the end; doing it during conversion would make the SSA dump depend on an optimization, which is
 * the one thing a fixture about ownership should not have to read around.
 */
struct InstAlloc: Inst {
    InstAlloc(LocationId source, const Occurrence& held, StorageClass storage):
        Inst(Alloc, source), held(held), storage(storage) {}

    /*
     * What the storage holds, as an occurrence rather than a type.
     *
     * Size and alignment are the manifestation's answer and are asked for at lowering - and the
     * manifestation needs all three of type, contract and shape. A slot holding a shared sequence
     * window is not a slot holding the sequence, and a field that recorded only the logical type
     * would have made the two the same allocation.
     */
    Occurrence held;
    StorageClass storage;

    template<class F> void eachUse(SsaBase, F&&) {}
    template<class F> void eachEdge(F&&) {}
};

/*
 * Reading storage.
 *
 * The result's convention says what was read: `Trivial` for a scalar, `Owned` where the load is a
 * destructive read that took the object out, `Guaranteed` where it produced a borrow of what the
 * address names. One instruction with an explicit answer, rather than one whose meaning depends on
 * the representation of its type - which is §1.2's `LoadPlace`.
 */
struct InstLoad: Inst {
    InstLoad(LocationId source, Use address): Inst(Load, source), address(address) {}

    Use address;

    template<class F> void eachUse(SsaBase, F&& f) { f(address); }
    template<class F> void eachEdge(F&&) {}
};

/*
 * Writing storage.
 *
 * The value's use is `Consume` where an owner is installed and `Inspect` where a trivial value is:
 * a store of an owned value is the point its lineage ends here and begins in the place, and the
 * count the verifier keeps has to see it. What the store does *not* say is whether the place held
 * something already - that was decided in MIR, by `Destination`, and a drop for the old contents is
 * emitted before this by the conversion rather than implied by it.
 */
struct InstStore: Inst {
    InstStore(LocationId source, Use address, Use value):
        Inst(Store, source), address(address), value(value) {}

    Use address;
    Use value;

    template<class F> void eachUse(SsaBase, F&& f) { f(address); f(value); }
    template<class F> void eachEdge(F&&) {}
};

struct InstBinary: Inst {
    InstBinary(LocationId source, BinaryOp op, Use lhs, Use rhs):
        Inst(Binary, source), lhs(lhs), rhs(rhs), op(op) {}

    Use lhs, rhs;
    BinaryOp op;

    template<class F> void eachUse(SsaBase, F&& f) { f(lhs); f(rhs); }
    template<class F> void eachEdge(F&&) {}
};

struct InstUnary: Inst {
    InstUnary(LocationId source, UnaryOp op, Use value):
        Inst(Unary, source), value(value), op(op) {}

    Use value;
    UnaryOp op;

    template<class F> void eachUse(SsaBase, F&& f) { f(value); }
    template<class F> void eachEdge(F&&) {}
};

/*
 * A call to a declared function.
 *
 * Not pure and never will be: a call is the one instruction whose effects this tier does not know,
 * so recomputing one or dropping one is not something an optimizer may do from the kind alone. What
 * it *may* do is read the callee's published summary - `mir::Summary`, through `callee` - which is
 * where "does this suspend", "does this retain an argument" and "what placement does this demand"
 * are answered. That is the point of the callee being a declaration handle rather than a name.
 *
 * `kSsaConsumes` because an argument filling a `->` parameter is a consuming use: the ownership
 * lineage ends in the caller at this instruction and begins in the callee. An argument filling a
 * borrowed parameter is a `Borrow`, and the two are told apart by the use rather than by the kind -
 * which is what makes one flag enough for both.
 */
struct InstCall: Inst {
    InstCall(LocationId source, DeclPtr<mir::Function> callee):
        Inst(Call, source), callee(callee) {}

    DeclPtr<mir::Function> callee;
    UseList arguments;

    template<class F> void eachUse(SsaBase base, F&& f) {
        for(auto used: arguments.contents(base)) f(used);
    }

    template<class F> void eachEdge(F&&) {}
};

struct InstReturn: Inst {
    InstReturn(LocationId source): Inst(Return, source) {}

    // Empty for a function whose result is `{}`. A list because a function may hand back several
    // values, which the lower IR already supports and the source language will.
    UseList values;

    template<class F> void eachUse(SsaBase base, F&& f) { for(auto use: values.contents(base)) f(use); }
    template<class F> void eachEdge(F&&) {}
};

struct InstJump: Inst {
    InstJump(LocationId source): Inst(Jump, source) {}

    Edge to;

    template<class F> void eachUse(SsaBase, F&&) {}
    template<class F> void eachEdge(F&& f) { f(to); }
};

struct InstBranch: Inst {
    InstBranch(LocationId source, Use condition): Inst(Branch, source), condition(condition) {}

    Use condition;
    Edge whenTrue;
    Edge whenFalse;

    template<class F> void eachUse(SsaBase, F&& f) { f(condition); }
    template<class F> void eachEdge(F&& f) { f(whenTrue); f(whenFalse); }
};

struct InstUnreachable: Inst {
    explicit InstUnreachable(LocationId source): Inst(Unreachable, source) {}

    template<class F> void eachUse(SsaBase, F&&) {}
    template<class F> void eachEdge(F&&) {}
};

template<class F>
auto visitInst(Inst& inst, F&& f) -> decltype(f((InstConst&)inst)) {
    switch(inst.kind) {
#define YANA_SSA_INST(kind, Struct, mnemonic, flags) case Inst::kind: return f((Struct&)inst);
#include "inst.def"
#undef YANA_SSA_INST
        case Inst::KindCount: break;
    }

    assertTrue(false);
    return f((InstConst&)inst);
}

/*
 * Every operand and every edge, declared beside the fields on each instruction.
 *
 * Operands arrive **by value**, not by reference, and that is the gatekeeper rule rather than an
 * inconvenience: an operand held in a `UseList` cannot be handed out by reference at all - reading
 * one back through the list is what `util/README.md`'s `contents()` trap is about - so a walk that
 * offered references for some kinds and not others would be an interface that lies about half its
 * cases. Rewriting an operand goes through the editor, which updates the use lists in the same call.
 *
 * A kind that declares neither walk does not compile: `visitInst` never passes a bare `Inst&`, and
 * `Inst` has no `eachUse`.
 */
template<class F>
void forEachUse(SsaBase base, Inst& inst, F&& f) {
    visitInst(inst, [&](auto& i) { i.eachUse(base, f); });
}

template<class F>
void forEachEdge(Inst& inst, F&& f) {
    visitInst(inst, [&](auto& i) { i.eachEdge(f); });
}

/*
 * A basic block, with arguments.
 *
 * Block arguments rather than phis, carried over from MIR for the same reason (§5.9, §8): the
 * transfer of an owned value happens on the predecessor's terminator, so there is no gap between a
 * consuming edge and the merge that assumed it.
 */
struct Block {
    Block(BlockId index, StringId name): name(name), index(index) {}

    // Embedded: a merge usually carries none or one, and the non-embedding form allocated an
    // eight-entry buffer for each of those.
    SsaList<SsaPtr<Value>, true> arguments;

    SsaList<SsaPtr<Inst>, false> instructions;
    SsaList<SsaPtr<Block>> incoming;

    StringId name {};
    BlockId index;
};

struct Body {
    Body() = default;

    SsaPtr<Block> entry = nullptr;

    RegionArray<SsaRegion, SsaPtr<Block>> blocks;
    RegionArray<SsaRegion, SsaPtr<Value>> values;
    RegionArray<SsaRegion, SsaPtr<Inst>> insts;

    Block* block(SsaBase base, BlockId index) const { return base[blocks.get(base, index.index)]; }
    Value* value(SsaBase base, ValueId id) const { return base[values.get(base, id.index)]; }
    Inst* inst(SsaBase base, InstId id) const { return base[insts.get(base, id.index)]; }
};

StringView instMnemonic(Inst::Kind kind);
U16 instFlags(Inst::Kind kind);
StringView conventionName(Convention convention);
StringView useKindName(UseKind kind);

using mir::storageClassName;

inline bool isTerminator(Inst::Kind kind) { return (instFlags(kind) & kSsaTerminator) != 0; }
inline bool isPure(Inst::Kind kind) { return (instFlags(kind) & kSsaPure) != 0; }
inline bool mayConsume(Inst::Kind kind) { return (instFlags(kind) & kSsaConsumes) != 0; }

} // namespace ssa
