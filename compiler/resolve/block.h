#pragma once

#include "inst.h"

struct Module;
struct IrEditor;

/*
 * One basic block, and the four lists that make it one.
 *
 * All of them are private, and `IrEditor` (edit.h) is the only friend. That is not encapsulation for
 * its own sake: each list is half of a statement the IR makes twice - an instruction names its block
 * back, a successor records where its edges came from, a phi has one alternative per predecessor -
 * and a pass that could push into one of them directly is a pass that could state half of it. What
 * is left here is the reading half, which hands out contents rather than the list.
 */
struct Block {
    Block(ModulePtr<Function> function, StringId name, U16 index):
        function(function), name(name), index(index) {}

    bool isComplete() const { return terminatorInst != nullptr; }

    auto instructions(ModuleBase base) { return instructionList.contents(base); }
    Size instructionCount() { return instructionList.size(); }
    ModulePtr<Inst> instructionAt(ModuleBase base, Size index) { return instructionList.get(base, index); }

    auto phis(ModuleBase base) { return phiList.contents(base); }
    Size phiCount() { return phiList.size(); }
    ModulePtr<InstPhi> phiAt(ModuleBase base, Size index) { return phiList.get(base, index); }

    auto incoming(ModuleBase base) { return incomingList.contents(base); }
    Size predecessorCount() { return incomingList.size(); }
    ModulePtr<Block> predecessorAt(ModuleBase base, Size index) { return incomingList.get(base, index); }

    // The two ways out, in the order a `je` names them: `successor(0)` is the `then` arm of a branch
    // and the target of a jump, `successor(1)` is the `else` arm and null for everything else. The
    // pair is always two entries long, so a walk of it skips the nulls rather than stopping at one.
    ModulePtr<Block> successor(Size index) const { return outgoingBlocks[index]; }
    Buffer<const ModulePtr<Block>> successors() const {
        return Buffer<const ModulePtr<Block>>(outgoingBlocks, 2);
    }
    ModulePtr<Inst> terminator() const { return terminatorInst; }

    ModulePtr<Function> function;

    StringId name = 0;
    LocationId source = kNullLocation;

    // The block's position in `Function::blocks`, which every walk in opt_flow.cpp indexes by - see
    // IrEditor::setBlockOrder, which is what keeps the two in step.
    U16 index;

private:
    friend struct IrEditor;

    ModulePtr<Inst> terminatorInst = nullptr;
    ModuleList<ModulePtr<InstPhi>> phiList;
    ModuleList<ModulePtr<Inst>, false> instructionList;
    ModuleList<ModulePtr<Block>> incomingList;
    ModulePtr<Block> outgoingBlocks[2] = { nullptr, nullptr };
};
