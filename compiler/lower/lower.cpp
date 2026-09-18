#include "lower.h"
#include "lower_inst.h"

/*
 * The intrinsics the IR can name. How each one is *encoded* is the target's and lives in its
 * registry; this is what the IR itself has to know - enough to write one down, read it back, and
 * decide whether a pass may move it. See LowerIntrinsic and LowerIntrinsicDesc.
 *
 * The flags column is the half that used to be missing. Nothing here said what an intrinsic did, so
 * every pass at this tier assumed the worst about all of them; the x64 registry stated it properly
 * and nothing ever read what it said.
 */
static constexpr LowerIntrinsicDesc kIntrinsics[kLowerIntrinsicCount] = {
    [Size(LowerIntrinsic::Popcnt)] = { "popcnt"_v, 1, 1, kLowerPure | kLowerRepeatable },
    [Size(LowerIntrinsic::Cttz)] = { "cttz"_v, 1, 1, kLowerPure | kLowerRepeatable },
    [Size(LowerIntrinsic::CttzWidth)] = { "cttz_width"_v, 1, 1, kLowerPure | kLowerRepeatable },
    [Size(LowerIntrinsic::Bsr)] = { "bsr"_v, 1, 1, kLowerPure | kLowerRepeatable },
    [Size(LowerIntrinsic::ClzWidth)] = { "clz_width"_v, 1, 1, kLowerPure | kLowerRepeatable },

    // The value and the bit count, in that order, and the value with everything above that count
    // cleared.
    [Size(LowerIntrinsic::Bzhi)] = { "bzhi"_v, 1, 2, kLowerPure | kLowerRepeatable },
    [Size(LowerIntrinsic::Pext)] = { "pext"_v, 1, 2, kLowerPure | kLowerRepeatable },
    [Size(LowerIntrinsic::Pdep)] = { "pdep"_v, 1, 2, kLowerPure | kLowerRepeatable },
    [Size(LowerIntrinsic::Nop)] = { "nop"_v, 0, 0, kLowerOrdered },
    [Size(LowerIntrinsic::Lretq)] = { "lretq"_v, 0, 1, kLowerOrdered | kLowerPrivileged },

    // Takes the leaf and subleaf, and returns the four information registers in their usual order.
    [Size(LowerIntrinsic::Cpuid)] = { "cpuid"_v, 4, 2, kLowerOrdered },

    // Returns the counter as two halves, low then high, because that is how the machine hands it
    // back: one register each, and combining them is the caller's arithmetic rather than ours.
    [Size(LowerIntrinsic::Rdtscp)] = { "rdtscp"_v, 2, 0, kLowerOrdered },

    // The same counter as one number, which is the target's arithmetic rather than the caller's:
    // it is three instructions there and two here, and the two would be allocated as if the halves
    // were values worth keeping. See the registry.
    [Size(LowerIntrinsic::Rdtsc)] = { "rdtsc"_v, 1, 0, kLowerOrdered },

    [Size(LowerIntrinsic::MFence)] = { "mfence"_v, 0, 0, kLowerReads | kLowerWrites | kLowerOrdered },
    [Size(LowerIntrinsic::LFence)] = { "lfence"_v, 0, 0, kLowerReads | kLowerOrdered },
    [Size(LowerIntrinsic::SFence)] = { "sfence"_v, 0, 0, kLowerWrites | kLowerOrdered },
    [Size(LowerIntrinsic::Pause)] = { "pause"_v, 0, 0, kLowerOrdered },

    [Size(LowerIntrinsic::Prefetch)] = { "prefetch"_v, 0, 1, kLowerReads },
    [Size(LowerIntrinsic::PrefetchNta)] = { "prefetchnta"_v, 0, 1, kLowerReads },
    [Size(LowerIntrinsic::Clflush)] = { "clflush"_v, 0, 1, kLowerReads | kLowerWrites | kLowerOrdered },
    [Size(LowerIntrinsic::Invlpg)] = { "invlpg"_v, 0, 1, kLowerWrites | kLowerOrdered | kLowerPrivileged },

    [Size(LowerIntrinsic::Hlt)] = { "hlt"_v, 0, 0, kLowerOrdered | kLowerPrivileged },
    [Size(LowerIntrinsic::Cli)] = { "cli"_v, 0, 0, kLowerOrdered | kLowerPrivileged },
    [Size(LowerIntrinsic::Sti)] = { "sti"_v, 0, 0, kLowerOrdered | kLowerPrivileged },
    [Size(LowerIntrinsic::Swapgs)] = { "swapgs"_v, 0, 0, kLowerOrdered | kLowerPrivileged },

    // Takes the register number and returns its two halves; the write takes the number and both
    // halves. Split rather than joined because that is how the machine addresses them, and a
    // 64-bit value here would be arithmetic in front of an instruction that cannot read it.
    [Size(LowerIntrinsic::Rdmsr)] = { "rdmsr"_v, 2, 1, kLowerOrdered | kLowerPrivileged },
    [Size(LowerIntrinsic::Wrmsr)] = { "wrmsr"_v, 0, 3, kLowerOrdered | kLowerPrivileged },
    [Size(LowerIntrinsic::Xgetbv)] = { "xgetbv"_v, 2, 1, kLowerOrdered },

    [Size(LowerIntrinsic::In8)] = { "in8"_v, 1, 1, kLowerOrdered | kLowerPrivileged },
    [Size(LowerIntrinsic::In16)] = { "in16"_v, 1, 1, kLowerOrdered | kLowerPrivileged },
    [Size(LowerIntrinsic::In32)] = { "in32"_v, 1, 1, kLowerOrdered | kLowerPrivileged },
    [Size(LowerIntrinsic::Out8)] = { "out8"_v, 0, 2, kLowerOrdered | kLowerPrivileged },
    [Size(LowerIntrinsic::Out16)] = { "out16"_v, 0, 2, kLowerOrdered | kLowerPrivileged },
    [Size(LowerIntrinsic::Out32)] = { "out32"_v, 0, 2, kLowerOrdered | kLowerPrivileged },

    [Size(LowerIntrinsic::Ins32)] = { "ins32"_v, 0, 3, kLowerWrites | kLowerOrdered | kLowerPrivileged },
    [Size(LowerIntrinsic::Outs32)] = { "outs32"_v, 0, 3, kLowerReads | kLowerOrdered | kLowerPrivileged },

    [Size(LowerIntrinsic::Interrupt)] = { "int"_v, 0, 1, kLowerOrdered },
    [Size(LowerIntrinsic::Lidt)] = { "lidt"_v, 0, 1, kLowerReads | kLowerOrdered | kLowerPrivileged },
    [Size(LowerIntrinsic::Lgdt)] = { "lgdt"_v, 0, 1, kLowerReads | kLowerOrdered | kLowerPrivileged },
    [Size(LowerIntrinsic::MovDs)] = { "mov_ds"_v, 0, 1, kLowerOrdered | kLowerPrivileged },
    [Size(LowerIntrinsic::MovEs)] = { "mov_es"_v, 0, 1, kLowerOrdered | kLowerPrivileged },
    [Size(LowerIntrinsic::MovFs)] = { "mov_fs"_v, 0, 1, kLowerOrdered | kLowerPrivileged },
    [Size(LowerIntrinsic::MovGs)] = { "mov_gs"_v, 0, 1, kLowerOrdered | kLowerPrivileged },
    [Size(LowerIntrinsic::MovSs)] = { "mov_ss"_v, 0, 1, kLowerOrdered | kLowerPrivileged },

    [Size(LowerIntrinsic::ReadCr0)] = { "readcr0"_v, 1, 0, kLowerOrdered | kLowerPrivileged },
    [Size(LowerIntrinsic::ReadCr2)] = { "readcr2"_v, 1, 0, kLowerOrdered | kLowerPrivileged },
    [Size(LowerIntrinsic::ReadCr3)] = { "readcr3"_v, 1, 0, kLowerOrdered | kLowerPrivileged },
    [Size(LowerIntrinsic::ReadCr4)] = { "readcr4"_v, 1, 0, kLowerOrdered | kLowerPrivileged },
    [Size(LowerIntrinsic::WriteCr0)] = { "writecr0"_v, 0, 1, kLowerOrdered | kLowerPrivileged },
    [Size(LowerIntrinsic::WriteCr3)] = { "writecr3"_v, 0, 1, kLowerOrdered | kLowerPrivileged },
    [Size(LowerIntrinsic::WriteCr4)] = { "writecr4"_v, 0, 1, kLowerOrdered | kLowerPrivileged },

    // Flags, the top of the child's stack, the two words of the function it runs, its argument, and
    // where the kernel is to clear the thread id - and the identifier, or a negated errno, back.
    // See the registry.
    [Size(LowerIntrinsic::CloneThread)] = { "clonethread"_v, 1, 6, kLowerReads | kLowerWrites | kLowerOrdered },

    // The top of the stack being prepared, the two words of the entry function and its argument -
    // and the saved stack pointer the first switch to it reads. `kLowerOrdered` beside the memory
    // bits because what it writes is not memory this function otherwise names: a pass that moved it
    // past the switch that reads it would be moving it past its only reader.
    [Size(LowerIntrinsic::StackInit)] = { "stackinit"_v, 1, 4, kLowerReads | kLowerWrites | kLowerOrdered },
};

/*
 * Every intrinsic has a row, which the designated initializers above cannot say for themselves.
 *
 * A missing row is not a hole - it is a zeroed one, and a zeroed row is the dangerous answer twice
 * over: an empty name that `findLowerIntrinsic` would match nothing against, and flags saying the
 * intrinsic touches no memory and orders nothing, which is what lets a pass move it anywhere. The
 * name is what is checked because it is the one field no real row leaves empty.
 */
static constexpr bool everyIntrinsicNamed() {
    for(auto& desc: kIntrinsics) {
        if(desc.name.length == 0) return false;
    }

    return true;
}

static_assert(everyIntrinsicNamed(), "every LowerIntrinsic has a row in kIntrinsics");

const LowerIntrinsicDesc& lowerIntrinsicDesc(LowerIntrinsic id) {
    assertTrue(Size(id) < kLowerIntrinsicCount);
    return kIntrinsics[Size(id)];
}

Maybe<LowerIntrinsic> findLowerIntrinsic(StringId name) {
    for(Size i = 0; i < kLowerIntrinsicCount; i++) {
        if(Context::nameHash(kIntrinsics[i].name) == name) return Just(LowerIntrinsic(i));
    }

    return Nothing();
}

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

    /*
     * The edges the terminator names, restated on the block and registered with the targets.
     *
     * One walk over `terminatorEdges` rather than an arm per kind: a terminator's successors are
     * already stated in one place (lower_inst.h), and having the two agree by construction is what
     * kept `Switch` from being a third copy of this with a list in it.
     */
    if(isTerminator(inst)) {
        assertTrue(outgoing.isEmpty());

        terminatorEdges(inst, [&](LowerPtr<LowerBlock> target) {
            // Duplicate-free, per LowerBlock::outgoing: an edge is the pair of blocks it joins
            // everywhere below here, so a target named twice is one edge with two sets of phi
            // arguments. A `Je` with equal arms and a `Switch` with a repeated case are both this.
            assertTrue(!base[target]->incoming.contents(base).containsValue(this - base));

            outgoing.push(arena, target);
            base[target]->incoming.push(arena, this - base);
        });
    }

    return inst;
}

void LowerBlock::setSuccessors(LowerBase base, const LowerPtr<LowerBlock>* blocks, Size count) {
    auto& arena = base[fun]->arena;

    outgoing = {};
    for(Size i = 0; i < count; i++) outgoing.push(arena, blocks[i]);
}

bool LowerBlock::dominates(LowerBlock* block, const DominatorTree& dominators) {
    if(block == this) return true;

    /*
     * A block the postorder walk never reached - `buildPostorder` leaves `kNullBlock` on one, and
     * only a pass that deletes unreachable code takes it away. Under `-no-ir-opt` nothing does, so
     * the validator asks this question about blocks that are still there and cannot run.
     *
     * Two answers and they are not the same one. An unreachable **use** is dominated vacuously -
     * there is no path to it for a definition to be missing on - and answering otherwise reports a
     * function that is correct. An unreachable **definition** dominates nothing that can run.
     *
     * Checked here rather than at each caller, which is where it had been: `lower_thread.cpp`
     * guards the same field twice and this walk did not, so `dominators.tree[kNullBlock]` was a
     * read past the end of the tree - Tritium's `Array.h:276` assertion, on every unoptimized
     * build of a program with a dead block in it.
     */
    if(block->postIndex == kNullBlock) return true;
    if(postIndex == kNullBlock) return false;

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
        functionOrder.push(f - *arena);
    }

    return (*arena)[*result.value];
}
