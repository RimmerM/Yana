#pragma once

#include "inst.h"

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

    Block* addBlock(StringId name);

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
    Inst* addAlloc(LocationId source, const Occurrence& held, StorageClass storage);
    Inst* addLoad(LocationId source, Use address, const Occurrence& result, Convention convention);
    Inst* addStore(LocationId source, Use address, Use value);

    // The result convention is the caller's, not this call's: a comparison answers a `Bool` and an
    // addition answers its operand type, and which of those it is was settled in MIR. Passing it in
    // keeps `conventionFor` the one place an occurrence becomes a convention.
    Inst* addBinary(LocationId source, BinaryOp op, Use lhs, Use rhs,
                    const Occurrence& result, Convention convention);
    Inst* addUnary(LocationId source, UnaryOp op, Use value,
                   const Occurrence& result, Convention convention);

    /*
     * A call. `hasResult` is false where the callee answers `{}`, and that is a decision rather than
     * a shortcut: a `{}` has no bytes and therefore no register and no lower value, so a result value
     * for one would be a value nothing below this can map. Making it *absent* means an operand that
     * tried to name it fails loudly at conversion instead of silently lowering to nothing.
     */
    Inst* addCall(LocationId source, DeclPtr<mir::Function> callee, const Use* arguments, Size count,
                  const Occurrence& result, Convention convention, bool hasResult);

    Inst* addReturn(LocationId source, const Use* values, Size count);
    Inst* addJump(LocationId source, Block& target, const Use* arguments, Size count);
    Inst* addBranch(LocationId source, Use condition, Block& whenTrue, Block& whenFalse);
    Inst* addUnreachable(LocationId source);

    Value* resultOf(Inst& inst, Size index = 0);
    Use use(Value& value, UseKind kind);

    // Fills the predecessor lists from the edges. Derived, so it is computed once the forward
    // direction is complete rather than maintained twice while it is being built.
    void seal();

private:
    Block* current = nullptr;

    Inst* append(Inst* inst);
    Value* addResult(Inst& inst, const Occurrence& occurrence, Convention convention);
    void recordUse(Inst& inst, Use used);
};

} // namespace ssa
