#pragma once

#include "inst.h"
#include "../util/container.h"

struct Function;

// A sequence of instructions that is executed without interruption.
struct Block {
    ModulePtr<Function> function;

    // Instructions in the block.
    ModulePtr<Inst> terminator = nullptr;
    SmallList<ModuleRegion, ModulePtr<InstPhi>> phis;
    SmallList<ModuleRegion, ModulePtr<Inst>, false> instructions;

    // The defined values with a name in this block up to this point.
    HashMap<StringId, Value*> namedValues;

    // All blocks that can branch to this one.
    SmallList<ModuleRegion, ModulePtr<Block>> incoming;

    // All blocks this one can possibly branch to.
    // Due to the way the instruction set is structured, each block branches to either 0, 1 or 2 other blocks.
    ModulePtr<Block> outgoing[2] = { nullptr, nullptr };

    // The closest block that always executes before this one.
    // Set to null if this is the entry point.
    ModulePtr<Block> preceding = nullptr;

    // The closest block that always executes after this one.
    // Set to null if the block returns.
    ModulePtr<Block> succeeding = nullptr;

    void* codegen = nullptr;

    // Unique id of this block within the function.
    U32 id;

    // Set if this block is the target of a loop.
    // Backward branches are only allowed to loop targets.
    bool loop = false;

    Block(ModulePtr<Function> function, U32 id):
        function(function), id(id) {}

    bool isComplete() const {
        return terminator != nullptr;
    }

    bool returns(RegionBase<ModuleRegion> base) const {
        return terminator != nullptr && base[terminator]->kind == Value::InstRet;
    }

    Value* use(Module* module, Value* value, Inst* user);
    Value* inst(Module* module, Size size, StringId name, Inst::Kind kind, Type* type);
    Value* findValue(Module* module, StringId name);
};

Block* block(Module* module, Function* fun, bool deferAdd = false);

// Updates the name of an existing value.
// Overrides any existing value with this name in its block.
void setName(Module* module, Value* v, StringId name);
