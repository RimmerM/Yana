#pragma once

#include "../resolve_mir/decl.h"

namespace ssa {

using mir::Occurrence;
using mir::Contract;
using mir::Access;
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
struct Loan;

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

    Convention convention;

    /*
     * What this value is borrowed from - §7.1's "valid only while the loan it depends on is live",
     * as a term the verifier can check against. Set on exactly the `Guaranteed` values and null on
     * every other convention.
     *
     * **An OSSA-local identity, minted by the conversion**, which is the shape milestone 5 predicted
     * and deliberately did not build. What must not be here is an `mir::LoanId`: those are dense in
     * *one MIR body*, the MIR region is released the moment conversion finishes (§8.8), and what
     * would arrive is an index into a table that no longer exists. So the conversion mints a `Loan`
     * of this body's own as it emits the borrow, from the address it borrowed - see `Editor::use`
     * and `Body::loans`.
     *
     * The decision this reverses is "neither yet". It was right while nothing moved instructions:
     * loan extents, overlap and signature conformance are decided over OMIR by `analyze_loan.cpp`,
     * where the places and the origin environment are, and until a pass exists there is nothing at
     * this tier for a dependency to protect against. What changed is that the verifier now has a
     * position-dominance test and an endpoint rule to state, and both of them need the term: "this
     * value depends on that storage" is not recoverable from the operands after a rewrite, because a
     * rewrite is exactly what changes them.
     */
    SsaPtr<Loan> loan = nullptr;

    SsaList<SsaPtr<Inst>> uses;

    DeclPtr<mir::Type> type() const { return occurrence.type; }
    Contract contract() const { return occurrence.contract; }
};

/*
 * One borrow at this tier - the OSSA-local half of `mir::Loan`.
 *
 * Not a copy of it and not derived from it: an MIR loan is a *place* and an extent computed over the
 * origin environment, and this is a dependency between two SSA values. What they share is the word,
 * because they are the same fact seen from the two tiers - which is why the conversion mints these
 * rather than translating those, and why nothing here holds an `mir::LoanId`.
 *
 * `storage` null is the loan a **caller** granted: a borrowed parameter arrives already borrowed,
 * its lender is outside this body, and no instruction here can end it. That is `Origin::CallBounded`
 * seen from the inside, and it is a representable state rather than a missing one - the alternative
 * is every consumer having to ask whether a null means "the caller's" or "nobody filled this in".
 */
struct Loan {
    Loan(Access capability, LocationId source): source(source), capability(capability) {}

    LoanId id;

    /*
     * The storage the borrow may be rooted in - **a set**, and empty for the caller's.
     *
     * A set because a borrow can be rooted in more than one place and the tier below has to be right
     * about all of them: `if flag then borrow(a) else borrow(b)` is one value whose lender is `a` on
     * one path and `b` on the other, and a representation that held one of the two would call a use
     * after `a` was released correct. So the loan ends when *any* member is released, which is the
     * conservative direction and the only one that is sound for a may-set.
     *
     * Empty is the loan a **caller** granted: a borrowed parameter arrives already borrowed, its
     * lender is outside this body, and no instruction here can end it. That is `Origin::CallBounded`
     * seen from the inside, and it is a representable state rather than a missing one - the
     * alternative is every consumer having to ask whether an empty set means "the caller's" or
     * "nobody filled this in".
     */
    SsaList<SsaPtr<Value>> storage;

    LocationId source = kNullLocation;

    // Shared or unique - never `Owned`. A loan is by definition an occurrence somebody else owns.
    Access capability = Access::Shared;
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
 * The address of one field of a record.
 *
 * The **field**, not an offset. An offset is the target's answer and belongs to the manifestation
 * table, and an instruction that carried one would be a place where the layout has been decided a
 * second time - which is exactly how the current compiler ended up with seven place walks that have
 * to agree. What this names is the record type and the field index, and lowering asks the one table.
 *
 * Pure: it is arithmetic on an address and reads nothing, so it may be recomputed or sunk. What is
 * *at* the address is somebody else's instruction.
 */
struct InstFieldAddress: Inst {
    InstFieldAddress(LocationId source, Use base, DeclPtr<mir::Type> record, U16 field):
        Inst(FieldAddress, source), base(base), record(record), field(field) {}

    Use base;
    DeclPtr<mir::Type> record;
    U16 field;

    template<class F> void eachUse(SsaBase, F&& f) { f(base); }
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

/*
 * A second owner, made by duplicating the bytes - Analysis-IR.md §5.8's `copy.trivial`.
 *
 * Distinct from `Load`, and the distinction is the *meaning* rather than the machine form: a load
 * produces the object that is at an address, and this produces a new one that is equal to it. For a
 * value carried in a register the two are the same thing, so the conversion emits a load; for one
 * that lives in storage a load is the address it already has and this is fresh storage with the
 * bytes copied into it, which is the difference between handing a callee the caller's record and
 * handing it a copy.
 *
 * The evidence is `TrivialCopy` and is checked in MIR, where `UseMode::CopyTrivial` claims it. An
 * authored `copy` is a call and stays a call (§5.8).
 */
struct InstCopy: Inst {
    InstCopy(LocationId source, Use address): Inst(Copy, source), address(address) {}

    Use address;

    template<class F> void eachUse(SsaBase, F&& f) { f(address); }
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
 * A record built from one value per field - §5.7, carried down from MIR unchanged.
 *
 * One instruction rather than an allocation and a run of stores, because the components' *uses* are
 * what the linear rule counts: a field filled by an owned value consumes it exactly once, and a walk
 * that had already been expanded into stores would make that a property of a group of instructions
 * nothing names. §7.4's "scalarize an aggregate while preserving per-field consumption" is an
 * optimizer's licence, and it needs the unscalarized form to start from.
 */
struct InstAggregate: Inst {
    InstAggregate(LocationId source): Inst(Aggregate, source) {}

    UseList components;

    template<class F> void eachUse(SsaBase base, F&& f) {
        for(auto used: components.contents(base)) f(used);
    }

    template<class F> void eachEdge(F&&) {}
};

/*
 * The end of one object's life - §6.7.
 *
 * It names the **storage** and what is in it, not an SSA value, and that is the tier boundary doing
 * its job: an owner that lives in a place stopped being an SSA value when the store installed it,
 * and the linear rule this tier checks is about values in flight. Which object dies where is the
 * move-path dataflow's answer, and it is carried here as a placement rather than re-derived.
 *
 * `held` is the occurrence, so the teardown that runs is decided from the same three things
 * everything else is: what a type owes is `ownershipOf`, and the walk into a derived one is
 * `lower_inst.cpp`'s. **`Drop` and `Reclaim` are still one instruction here** - §6.7's "they remain
 * separate until representation lowering decides how storage is returned", read as: this says the
 * object's life ends, and the file that knows how storage is returned emits the two halves.
 */
struct InstDrop: Inst {
    InstDrop(LocationId source, Use address, const Occurrence& held):
        Inst(Drop, source), address(address), held(held) {}

    Use address;
    Occurrence held;

    template<class F> void eachUse(SsaBase, F&& f) { f(address); }
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
/*
 * A region of blocks with a nesting rule - Analysis-IR.md §9.4's continuation regions.
 *
 * Every block says which region owns it, and a region says which region owns *it*. That is a
 * statement the CFG cannot make: "these blocks are the cleanup arm of that point" is not a shape -
 * a diamond is a diamond whether its right arm is a cleanup, an `else` or the second half of a
 * short-circuit - and a pass that recovered it from the graph would be inferring an intent the
 * builder already knew.
 *
 * `Body` is one region per body and is what almost every block is in. `Cleanup` is the one nested
 * kind that exists: the arm of a conditional teardown, which the conversion synthesizes around a
 * drop flag rather than the source having written it. A lens frame and a yielded continuation are
 * further kinds - they are call-site regions with the same nesting rule and different entry and exit
 * conditions - and what this structure is here for is that they arrive as rows in this enum rather
 * than as a concept blocks did not have.
 *
 * The rule the verifier holds it to: a block in a region may be entered only from that region or
 * from its parent, and may leave only to that region or to an ancestor. That is what "nested" means
 * operationally, and it is checkable now.
 */
struct Continuation {
    enum Kind: U8 {
        Body,
        Cleanup,
    };

    Continuation(ContinuationId id, Kind kind, ContinuationId parent):
        id(id), parent(parent), kind(kind) {}

    ContinuationId id;

    // The enclosing region; invalid for the body's own, which is the only one with no parent.
    ContinuationId parent;

    Kind kind = Body;
};

struct Block {
    Block(BlockId index, StringId name, ContinuationId region):
        name(name), index(index), region(region) {}

    // Embedded: a merge usually carries none or one, and the non-embedding form allocated an
    // eight-entry buffer for each of those.
    SsaList<SsaPtr<Value>, true> arguments;

    SsaList<SsaPtr<Inst>, false> instructions;
    SsaList<SsaPtr<Block>> incoming;

    StringId name {};
    BlockId index;

    // Which continuation region owns this block - `Continuation`. Set when the block is made and
    // never derived: the builder knows, and afterwards nobody can.
    ContinuationId region;
};

struct Body {
    Body() = default;

    SsaPtr<Block> entry = nullptr;

    RegionArray<SsaRegion, SsaPtr<Block>> blocks;
    RegionArray<SsaRegion, SsaPtr<Value>> values;
    RegionArray<SsaRegion, SsaPtr<Inst>> insts;

    /*
     * Every borrow this body holds, in creation order - `Loan`.
     *
     * A dense table for the reason MIR's is one: the endpoint dataflow's state is a byte per loan
     * per block, and a set of handles would be a hash lookup in the inner loop of a check that runs
     * after every ownership-sensitive pass. Minted by `Editor::loanOn` and by nothing else, which is
     * what keeps two borrows of one address from becoming two loans that can be ended separately.
     */
    RegionArray<SsaRegion, SsaPtr<Loan>> loans;

    // The continuation regions, root first - `Continuation`. Never empty: a body has at least the
    // one every ordinary block is in, which is what makes "which region owns this block" a question
    // with an answer rather than an optional one.
    RegionArray<SsaRegion, SsaPtr<Continuation>> regions;

    Block* block(SsaBase base, BlockId index) const { return base[blocks.get(base, index.index)]; }
    Value* value(SsaBase base, ValueId id) const { return base[values.get(base, id.index)]; }
    Inst* inst(SsaBase base, InstId id) const { return base[insts.get(base, id.index)]; }
    Loan* loan(SsaBase base, LoanId id) const { return base[loans.get(base, id.index)]; }

    Continuation* region(SsaBase base, ContinuationId id) const {
        return id.valid() ? base[regions.get(base, id.index)] : nullptr;
    }

    // Whether `outer` encloses `inner`, reflexively. The nesting test itself, so that the verifier
    // and a later lowering ask it once rather than each walking the parent chain.
    bool encloses(SsaBase base, ContinuationId outer, ContinuationId inner) const {
        for(auto at = inner; at.valid(); at = region(base, at)->parent) {
            if(at == outer) return true;
        }

        return false;
    }
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
