#pragma once

#include "inst.h"

struct Function;

// A sequence of instructions that is executed without interruption.
struct Block {
    Function* function;
    Array<Inst*> instructions;

    // The defined values with a name in this block up to this point.
    HashMap<Value*, Id> namedValues;

    // All blocks that can branch to this one.
    Array<Block*> incoming;

    // All blocks this one can possibly branch to.
    // Due to the way the instruction set is structured, each block branches to either 0, 1 or 2 other blocks.
    ArrayF<Block*, 2> outgoing;

    // The closest block that always executes before this one.
    Block* preceding = nullptr;

    // The closest block that always executes after this one.
    Block* succeeding = nullptr;

    // Set if this block returns at the end.
    bool returns = false;

    // Set when the block contains a terminating instruction.
    // Appending instructions after this is set will have no effect.
    bool complete = false;

    void* codegen = nullptr;

    Value* use(Value* value, Inst* user);
    Inst* inst(Size size, Id name, Inst::Kind kind, Type* type);
};
