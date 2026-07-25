#include "lower.h"
#include "lower_inst.h"

LowerArg* LowerFunction::addArg(LowerBase base, StringId argName, LowerType type) {
    assertTrue(blocks.isNotEmpty());

    auto arg = new (arena) LowerArg(argName, type, args.size());
    arg->block = blocks.get(base, 0);

    args.push(arena, arg - base);
    return arg;
}

LowerBlock* LowerFunction::addBlock(LowerBase base, StringId blockName) {
    // If this is the first explicitly added block, add an implicit jump from the entry point to here.
    auto block = new (arena) LowerBlock(this - base, blockName, blocks.size());

    if(blocks.size() == 1) {
        auto entryPoint = base[blocks.get(base, 0)];
        entryPoint->addInst(base, new (arena) LowerInstJmp(block - base));
    }

    blocks.push(arena, block - base);
    return block;
}

LowerInst* LowerBlock::addInst(LowerBase base, LowerInst* inst) {
    assertTrue(inst->block == nullptr || base[inst->block] == this);
    inst->block = this - base;

    auto& arena = base[fun]->arena;

    if(inst->kind == LowerInst::Phi) {
        phis.push(arena, (LowerInstPhi*)inst - base);
    } else if(isTerminator(inst)) {
        assertTrue(terminator == nullptr);
        terminator = inst - base;
    } else {
        instructions.push(arena, inst - base);
    }

    for(auto use: inst->used()) {
        base[use]->uses.push(arena, inst - base);
    }

    if(inst->kind == LowerInst::Je) {
        assertTrue(outgoing[0] == nullptr && outgoing[1] == nullptr);
        auto je = (LowerInstJe*)inst;

        assertTrue(je->then != je->otherwise);
        outgoing[0] = je->then;
        outgoing[1] = je->otherwise;

        assertTrue(!base[je->then]->incoming.contents(base).containsValue(this - base));
        assertTrue(!base[je->otherwise]->incoming.contents(base).containsValue(this - base));
        base[je->then]->incoming.push(arena, this - base);
        base[je->otherwise]->incoming.push(arena, this - base);
    } else if(inst->kind == LowerInst::Jmp) {
        assertTrue(outgoing[0] == nullptr && outgoing[1] == nullptr);
        auto jmp = (LowerInstJmp*)inst;
        outgoing[0] = jmp->then;

        assertTrue(!base[jmp->then]->incoming.contents(base).containsValue(this - base));
        base[jmp->then]->incoming.push(arena, this - base);
    }

    return inst;
}

bool LowerBlock::dominates(LowerBlock* block, const DominatorTree& dominators) {
    if(block == this) return true;
    if(postIndex == dominators.startIndex) return true;

    auto i = block->postIndex;

    while(i != dominators.startIndex) {
        i = dominators.tree[i];
        if(i == postIndex) return true;
    }

    return false;
}

bool LowerValue::dominates(LowerBase base, LowerInst* c, const DominatorTree& dominators) {
    auto in = inst();
    auto block = base[in->block];

    if(in == c) return true;

    // If the creating instruction is in the same block, make sure it was defined before being used.
    if(block == base[c->block]) {
        // If `c` is the terminator, any instruction in the block dominates it.
        if(c == base[block->terminator]) return true;

        // If `in` is a phi node, it dominates any instruction in the block.
        if(isPhi(in)) return true;

        // When going through the instructions in the block, `in` needs to be seen before `c`.
        for(auto v: block->instructions.contents(base)) {
            if(base[v] == in) return true;
            if(base[v] == c) return false;
        }

        // This should never happen unless the instruction list has been corrupted.
        assertTrue("corrupted block instruction list" == nullptr);
        return false;
    }

    // If not, make sure the block of the creating instruction dominates the using block.
    return block->dominates(base[c->block], dominators);
}

LowerFunction* LowerModule::addFunction(StringId funName) {
    auto result = functions.add(funName);
    if(!result.existed) {
        auto f = new (arena) LowerFunction(arena, this, funName);
        *result.value = f - *arena;
    }

    return (*arena)[*result.value];
}
