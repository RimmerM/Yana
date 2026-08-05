#pragma once

#include "inst.h"

struct Module;

// Recording one ordinary instruction as a user of everything it names - see block.cpp. `Block::add`
// does this itself; a pass that splices an instruction into a block without going through it owes
// this call, or it leaves an instruction that reads storage and is in no use list.
void recordInstUses(Module& module, Inst* inst);

/*
 * One entry of one use list, which is what everything above is made of.
 *
 * Exposed because there is one caller that has to record a use the ordinary path could not. A
 * specialization's local table is filled in *after* its body, since the value each slot holds is
 * produced by an instruction that has to be cloned first - so every place rooted in a local was
 * added while the slot still held nothing, and `addPlaceUse` had no value to attribute the use to.
 * See cloneBody, which pays exactly the slots that were empty at the time and no others.
 *
 * A use recorded twice is as wrong as one not recorded at all - `dropUse` removes one entry per
 * naming - so a caller reaching for this is one that knows precisely which entries are missing.
 */
void recordUse(Module& module, ModulePtr<Value> value, ModulePtr<Inst> user);

struct Block {
    Block(ModulePtr<Function> function, StringId name, U16 index):
        function(function), name(name), index(index) {}

    Inst* add(Module& module, Inst* inst);

    bool isComplete() const { return terminator != nullptr; }

    ModulePtr<Function> function;
    ModulePtr<Inst> terminator = nullptr;
    ModuleList<ModulePtr<InstPhi>> phis;
    ModuleList<ModulePtr<Inst>, false> instructions;
    ModuleList<ModulePtr<Block>> incoming;
    ModulePtr<Block> outgoing[2] = { nullptr, nullptr };

    StringId name = 0;
    LocationId source = kNullLocation;
    U16 index;
};
