#pragma once

#include "edit.h"

/*
 * Instruction construction, in the same spirit as lower/lower_builder.h.
 *
 * Creating a resolve instruction is four separate things - allocate it in the module arena,
 * record where in the source it came from, give it the id print.cpp names it by, and append it
 * to its block - and every one of them has to happen for the IR to be printable and walkable.
 * Doing that once here is what keeps the resolver's own code about the language instead of
 * about bookkeeping, and it is why every Inst constructor takes (block, type) first.
 */

// Creates an instruction without appending it. Calls and phis need this: `IrEditor::append` is what
// records an instruction's uses, so an operand list that is filled in after construction has to
// be complete before the instruction reaches its block.
template<class T, class... Args>
inline T* createInst(Module& module, Function& function, Block& block, LocationId source, StringId name, TypePtr type, Args&&... args) {
    auto base = *module.arena;
    auto inst = new (module.arena) T(&block - base, type, forward<Args>(args)...);

    inst->source = source;
    inst->name = name;
    inst->id = function.valueCounter++;

    return inst;
}

template<class T, class... Args>
inline T* addInst(Module& module, Function& function, Block& block, LocationId source, StringId name, TypePtr type, Args&&... args) {
    auto inst = createInst<T>(module, function, block, source, name, type, forward<Args>(args)...);
    IrEditor(module, function).append(block, inst);
    return inst;
}

// Constants belong to no block: they are printed inline wherever they are used, and
// resolve/lower.cpp materializes each one once per function in the entry block. They still get
// an id and a source, because everything that walks values expects both.
template<class T, class... Args>
inline T* addConstant(Module& module, Function& function, Block& block, LocationId source, TypePtr type, Args&&... args) {
    auto base = *module.arena;
    auto value = new (module.arena) T(&block - base, type, forward<Args>(args)...);

    value->source = source;
    value->id = function.valueCounter++;

    return value;
}
