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

    /*
     * The ways out, one slot per arm of the terminator and in the terminator's own order:
     * `successor(0)` is the `then` arm of a branch and the target of a jump, `successor(1)` is the
     * `else` arm and null for everything else.
     *
     * There are always `kMaxSuccessors` of them rather than as many as this block uses, so a walk
     * skips the nulls rather than stopping at one - and the width is the *instruction's* number
     * (resolve/inst.h), because a slot here is an arm there. That is the one thing tying the two
     * together: `IrEditor` copies one array into the other, and a terminator with more arms than
     * this block has slots would be a set of edges half of which no block records.
     */
    ModulePtr<Block> successor(Size index) const { return outgoingBlocks[index]; }
    Buffer<const ModulePtr<Block>> successors() const {
        return Buffer<const ModulePtr<Block>>(outgoingBlocks, kMaxSuccessors);
    }

    /*
     * The one block this leaves to, or null where it leaves to none or to more than one.
     *
     * The question a pass asks when it wants to know that control goes exactly one way from here -
     * a loop's preheader, a merge candidate - and it is a method so that asking it is not "arm 0 is
     * X and arm 1 is empty", which stops being the same question the moment there is an arm 2.
     */
    ModulePtr<Block> soleSuccessor() const {
        ModulePtr<Block> only = nullptr;

        for(auto outgoing: successors()) {
            if(!outgoing) continue;
            if(only) return nullptr;

            only = outgoing;
        }

        return only;
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
    // One slot per arm a terminator may have - see `successors()` for why it is that number.
    ModulePtr<Block> outgoingBlocks[kMaxSuccessors] = {};
};
