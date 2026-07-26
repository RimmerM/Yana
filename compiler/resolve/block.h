#pragma once

#include "inst.h"

struct Module;

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
