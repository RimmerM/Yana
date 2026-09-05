#pragma once

#include "inst.h"
#include "../resolve_mir/specialize.h"

namespace ssa {

/*
 * The one way SSA is built and rewritten - the gatekeeper the current `IrEditor` is, kept for the
 * tier where it belongs (§8.5).
 *
 * MIR is append-only and needs no editor; SSA is the optimizer's IR and does. Instruction order,
 * block arguments, CFG edges and use lists are four things that have to change together, and one
 * API is what keeps them from disagreeing - which is the same argument the existing editor makes,
 * and the reason it is one of the pieces this rewrite keeps rather than reconsiders.
 *
 * `Program` is the MIR stage's, because declarations are shared: a function here names an
 * `mir::Function`, never another body's instruction.
 */
struct Editor {
    Editor(Context& context, mir::Program& program, SsaArena& arena, mir::Function& function);

    Context& context;
    mir::Program& program;
    SsaArena& arena;
    mir::Function& function;
    Body& body;

    SsaBase base() { return *arena; }
    DeclBase declBase() { return *program.decl; }

    // A block in the region the editor is currently building into - see `enterRegion`. Almost every
    // block is in the body's own region, which is what makes the no-argument form the common one.
    Block* addBlock(StringId name);

    /*
     * A nested continuation region, and where it ends - `Continuation`.
     *
     * `enterRegion` opens one inside whatever is current and makes it current; `leaveRegion` returns
     * to the parent. A pair rather than a per-block argument because a region is a *span* of blocks -
     * the cleanup arm of a conditional drop is however many blocks that arm takes - and passing the
     * region at each `addBlock` is how one of them ends up in the wrong one.
     */
    ContinuationId enterRegion(Continuation::Kind kind, Continuation::Multiplicity multiplicity);
    void leaveRegion();

    // Which region a block made from here on belongs to. Read by the conversion, whose blocks all
    // exist before any of them is filled: an arm opened while one is being emitted has to nest in
    // *that* block's region and not in whichever one the previous block left current.
    void setRegion(ContinuationId region) { currentRegion = region; }

    // Where a region rejoins its parent - `Continuation::destination`. Separate from `leaveRegion`
    // because the block it names does not exist yet while the region is open: a cleanup arm's
    // successor is made after the arm, in the enclosing region, which is the whole point of it.
    void rejoinRegion(ContinuationId region, Block& destination);

    // Occurrences, not types: a merge of owned values and a merge of shared windows are the same
    // shape and different obligations, and the successor is where the obligation is declared.
    Value* addBlockArgument(Block& block, const Occurrence& occurrence, Convention convention);

    void setBlock(Block& block);
    Block* currentBlock() { return current; }

    /*
     * Instructions. Each returns the instruction; `resultOf` is how a caller names what it defined,
     * and asserts that it defined one - a raw index would let a caller name a result that is not
     * there.
     */

    Inst* addConst(LocationId source, const mir::Constant& constant);
    Inst* addGlobalAddress(LocationId source, DeclPtr<mir::Global> global);
    Inst* addGenValue(LocationId source, DeclPtr<mir::Type> variable, DeclPtr<mir::Type> type);

    // One measurement of `of`, as a value of `as` - see `InstMetric`.
    Inst* addMetric(LocationId source, mir::TypeMetric metric, DeclPtr<mir::Type> of,
                    DeclPtr<mir::Type> as);
    /*
     * An operation the machine has and this IR does not - see `InstNative`.
     *
     * `hasResult` rather than a unit result, exactly as `addCall` takes one: `setMemory` answers
     * `{}` and a value of no bits is not a value this tier names.
     */
    Inst* addNative(LocationId source, mir::NativeOp op, const Use* arguments, Size count,
                    const Occurrence& result, Convention convention, bool hasResult);

    /*
     * One atomic operation - the location first, then the shape `mir::AtomicForm` states.
     *
     * `hasResult` for the same reason `addNative` takes one: three of the rows answer `{}`, and a
     * result nothing can name is a value this tier would leave dead behind every store and fence.
     */
    Inst* addAtomic(LocationId source, mir::AtomicOp op, mir::AtomicOrder order,
                    mir::AtomicOrder failure, bool weak, const Use* arguments, Size count,
                    const Occurrence& result, Convention convention, bool hasResult);

    // Whether the compare-exchange named by `cas` stored - a projection of an instruction that has
    // already run, so it takes the `Bool` the library declared as its result type.
    Inst* addAtomicOk(LocationId source, Use cas, const Occurrence& result);

    /*
     * The four vector operations - the MIR nodes carried down, with the result occurrence stated
     * because three of the four answer something their operand does not decide: a splat's lane
     * count, a shuffle's whole shape and a `firstset`'s integer.
     *
     * `Convention::Trivial` throughout and not a parameter: a vector duplicates freely and releases
     * nothing, which is `mir::ownershipOf`'s constant for the kind rather than anything a site here
     * could decide differently.
     */
    // `a * b + c` at most once rounded - `mir::NodeFma` carried down.
    Inst* addFma(LocationId source, Use lhs, Use rhs, Use addend, const Occurrence& result);

    // A branch-free choice - `mir::NodeSelect` carried down, with the result stated because this
    // tier states every result rather than deriving one.
    Inst* addSelect(LocationId source, Use condition, Use whenTrue, Use whenFalse,
                    const Occurrence& result, Convention convention);

    Inst* addVecSplat(LocationId source, Use from, const Occurrence& result);
    Inst* addVecLane(LocationId source, Use from, U16 lane, const Occurrence& result);
    Inst* addVecWithLane(LocationId source, Use from, U16 lane, Use value,
                         const Occurrence& result);
    Inst* addVecShuffle(LocationId source, Use left, Use right, const U8* pattern, Size count,
                        const Occurrence& result);
    Inst* addVecReduce(LocationId source, Use from, mir::ReduceOp reduce,
                       const Occurrence& result);

    Inst* addAlloc(LocationId source, const Occurrence& held, StorageClass storage);
    // `shape` is the *subject's* interior structure - see `InstFieldAddress::shape`, where the
    // reason it cannot be read off `base` is written out. Null for every record that owns its
    // fields.
    Inst* addFieldAddress(LocationId source, Use base, DeclPtr<mir::Type> record, U16 field,
                          DeclPtr<mir::OwnershipSchema> shape = nullptr);

    // The address of one element - `xs[i]`. `container` is the fixed array or window it is an
    // element of, and what the result points at is that container's element type.
    Inst* addElementAddress(LocationId source, Use base, Use index, DeclPtr<mir::Type> container);

    // The same, where the subject is a type parameter: the offset is the property slot's and `held`
    // is what the requirement promised is at it.
    Inst* addPropertyAddress(LocationId source, Use base, U16 property, DeclPtr<mir::Type> held);
    Inst* addLoad(LocationId source, Use address, const Occurrence& result, Convention convention);
    Inst* addCopy(LocationId source, Use address, const Occurrence& result, Convention convention);
    Inst* addStore(LocationId source, Use address, Use value);

    // A record from one value per field, in field order. The occurrence is the record's, and the
    // convention is `conventionFor`'s answer for it - the same function every other result goes
    // through, so an aggregate of a type with a teardown is an owner without this deciding so.
    Inst* addAggregate(LocationId source, U16 constructor, const Use* components, Size count,
                       const Occurrence& result, Convention convention);

    // The end of the life of what is at `address`. No result: what dies was in storage, and this is
    // the instruction that says so - see `InstDrop`.
    Inst* addDrop(LocationId source, Use address, const Occurrence& held,
                  bool boxesVisible = true);

    // The result convention is the caller's, not this call's: a comparison answers a `Bool` and an
    // addition answers its operand type, and which of those it is was settled in MIR. Passing it in
    // keeps `conventionFor` the one place an occurrence becomes a convention.
    Inst* addBinary(LocationId source, BinaryOp op, Use lhs, Use rhs,
                    const Occurrence& result, Convention convention);
    Inst* addUnary(LocationId source, UnaryOp op, Use value,
                   const Occurrence& result, Convention convention);

    // A function value - G7. The result is the function type, address-only, so what comes out is
    // storage the lowering fills with the pair; `environment` is null where the value captured
    // nothing.
    Inst* addFunValue(LocationId source, DeclPtr<mir::Function> callee, const Use* environment,
                      const Occurrence& result, Convention convention);

    /*
     * A call, direct or through a value. `hasResult` is false where the callee answers `{}`, and
     * that is a decision rather than a shortcut: a `{}` has no bytes and therefore no register and
     * no lower value, so a result value for one would be a value nothing below this can map. Making
     * it *absent* means an operand that tried to name it fails loudly at conversion instead of
     * silently lowering to nothing.
     *
     * The whole `mir::CallSite` rather than a callee, because "which function does this reach" has
     * one answer with four shapes and `callTarget` already produced it. A parameter list that took
     * the target alone would be one a caller could satisfy while dropping the half that says the
     * target is a slot. `target` is the fourth shape's other half - the value the call goes through,
     * read only where `site.through` says so - which is what keeps one entry point for a call rather
     * than two that have to agree about the argument list, the result and the convention.
     */
    Inst* addCall(LocationId source, const mir::CallSite& site, Use target, const Use* arguments,
                  Size count, const Occurrence& result, Convention convention, bool hasResult);

    Inst* addReturn(LocationId source, const Use* values, Size count);
    Inst* addJump(LocationId source, Block& target, const Use* arguments, Size count);
    // A two-way branch, with what both arms receive. One argument list for both edges, which is
    // `mir::Builder::addBranch`'s rule and is stated there: a value live before a branch is live on
    // both sides of it.
    Inst* addBranch(LocationId source, Use condition, Block& whenTrue, Block& whenFalse,
                    const Use* arguments, Size count);
    Inst* addUnreachable(LocationId source);

    Value* resultOf(Inst& inst, Size index = 0);
    Use use(Value& value, UseKind kind);

    // Fills the predecessor lists from the edges. Derived, so it is computed once the forward
    // direction is complete rather than maintained twice while it is being built.
    void seal();

    /*
     * The loan on one set of storage, made once - `Loan`.
     *
     * Interned by the storage set and the capability together, so that two borrows rooted in the
     * same places are one loan and end at one point. An empty set is the caller's loan, which is the
     * whole of what a borrowed parameter depends on; there is one of those per capability and it
     * never ends inside this body.
     */
    Loan* loanOn(LocationId source, const SsaPtr<Value>* storage, Size count, Access capability);

    /*
     * What a borrowed value depends on, once the caller knows - `Value::loan`.
     *
     * Separate from creating the value because the two answers come from different places: the
     * convention is the occurrence's and is known where the instruction is built, and the lender is
     * the *origin environment's* and is known only to the conversion, which has the MIR body's loan
     * table in hand. A value keeps the caller's loan until this says otherwise, which is the
     * permissive answer and the right default for one this body cannot root.
     */
    void setDependency(Value& value, Loan& loan);

private:
    Block* current = nullptr;
    ContinuationId currentRegion;

    Inst* append(Inst* inst);
    Value* addResult(Inst& inst, const Occurrence& occurrence, Convention convention);
    void recordUse(Inst& inst, Use used);
};

} // namespace ssa
