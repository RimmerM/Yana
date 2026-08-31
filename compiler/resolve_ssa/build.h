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

    /*
     * The same region, *copied* - what the MIR conversion has instead of a span.
     *
     * A conversion is not building a nesting; it is transcribing one that was already checked, and
     * the regions of a flattened lens are not contiguous in block order - a frame's blocks stand on
     * both sides of the continuation's. So the pair above cannot express it, and the honest form is
     * a region named per block by something that already knows which. Nothing but a copy has any
     * business calling these: `enterRegion` is what a *builder* uses, and it is the one that cannot
     * put a block in a region its emission is not inside.
     */
    ContinuationId addRegion(Continuation::Kind kind, Continuation::Multiplicity multiplicity,
                             ContinuationId parent);
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
    Inst* addAlloc(LocationId source, const Occurrence& held, StorageClass storage);
    Inst* addFieldAddress(LocationId source, Use base, DeclPtr<mir::Type> record, U16 field);

    // The same, where the subject is a type parameter: the offset is the property slot's and `held`
    // is what the requirement promised is at it.
    Inst* addPropertyAddress(LocationId source, Use base, U16 property, DeclPtr<mir::Type> held);
    Inst* addLoad(LocationId source, Use address, const Occurrence& result, Convention convention);
    Inst* addCopy(LocationId source, Use address, const Occurrence& result, Convention convention);
    Inst* addStore(LocationId source, Use address, Use value);

    // A record from one value per field, in field order. The occurrence is the record's, and the
    // convention is `conventionFor`'s answer for it - the same function every other result goes
    // through, so an aggregate of a type with a teardown is an owner without this deciding so.
    Inst* addAggregate(LocationId source, const Use* components, Size count,
                       const Occurrence& result, Convention convention);

    // The end of the life of what is at `address`. No result: what dies was in storage, and this is
    // the instruction that says so - see `InstDrop`.
    Inst* addDrop(LocationId source, Use address, const Occurrence& held);

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
