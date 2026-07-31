#include "opt_pass.h"

/*
 * A branch whose condition is a constant, and the blocks that stop being reachable because of it.
 *
 * This is the one rewrite in the directory that changes the *shape* of the function rather than what
 * an instruction computes, which is why opt_fold.cpp left it alone: dominance, the natural loops,
 * every phi's alternatives and the ownership passes' block-level facts all rest on the graph, and a
 * pass that edits an edge without editing all four leaves an IR that prints correctly and walks
 * wrongly. So it is here, it does the whole job, and it does it in three steps.
 *
 *  1. **`je` on a constant becomes `jmp`.** The untaken successor loses one incoming edge, and every
 *     phi in it loses the alternative that arrived over that edge.
 *  2. **Blocks nothing reaches are deleted**, along with their edges into the blocks that survive.
 *     One folded branch usually strands a whole arm rather than a single block, and an arm that
 *     jumps to a join strands nothing at all - so this is a reachability sweep rather than a rule
 *     about the block that was just orphaned.
 *  3. **A phi left with one alternative is that alternative.** Which is the step that actually pays:
 *     a diamond whose two arms merge a constant each collapses to one of them, and everything
 *     downstream then folds against it.
 *
 * The use lists are rebuilt rather than repaired. What changed is which instructions exist at all,
 * and an instruction in a deleted block is a reader of values that are still live elsewhere - so the
 * cheap correct move is the repair the driver already performs once per function.
 *
 * ## Why the constant is there at all
 *
 * Because something else put it there. `match` on a value the caller passed as a literal, a `Bool`
 * that inlining turned into `True`, a comparison of two constants the folder answered - each arrives
 * as a `je` on a `ConstInt` and none of them is written that way in the source. `Default.yana`'s
 * `boxes()` was `if (1) { return 4096 } else { return 0 }` in the emitted JavaScript, which is the
 * whole of a function that should have been `return 4096`.
 *
 * ## The empty block a fold leaves, and why removing it is not optional
 *
 * A block whose test was the thing folded away has nothing left in it, and step 1 turns it into a
 * bare `jmp`. Splicing that out looked like tidiness and is not: it is what keeps a *loop* something
 * codegen/js can still emit.
 *
 * `while True:` puts the exit test in the loop header, so the header's immediate post-dominator is
 * the block after the loop - which is exactly what the structurizer takes for the loop's follow
 * block. Fold `je True, body, exit` to `jmp body` and the header's post-dominator becomes the
 * *body*, which is inside the loop: the structurizer then emits a one-block loop and meets the back
 * edge with nothing on its exit stack, which is `Guard.yana`'s `retry` and the diagnostic it
 * produced. Splicing the empty header out makes the body the header, whose post-dominator is
 * correctly nothing at all - every way out of that loop is a `return`.
 *
 * So the two steps belong together. A fold that leaves the block behind has moved a loop's exit test
 * out of its header without moving the header, and there is no third pass that would put that right.
 *
 * Blocks are not merged more generally. A target that still has phis is left alone as well, because
 * redirecting an edge around a block means every phi in the target needs one alternative per
 * predecessor of the block that went away - and where a predecessor could already reach the target
 * directly, the two alternatives it would then have are not required to agree.
 */

namespace {

// One edge from `from` into `into`, removed: the predecessor entry and the phi alternative that
// arrived over it. One of each rather than all, because two edges between the same pair of blocks
// are two entries and removing one is what folding one of them means.
void removeEdge(OptContext& opt, ModulePtr<Block> into, ModulePtr<Block> from) {
    auto block = opt.local[into];

    for(Size i = 0; i < block->incoming.size(); i++) {
        if(block->incoming.get(opt.local, i) != from) continue;

        block->incoming.remove(opt.local, i);
        break;
    }

    for(auto phiPointer: block->phis.contents(opt.local)) {
        auto phi = opt.local[phiPointer];

        for(Size i = 0; i < phi->inputs.size(); i++) {
            if(phi->inputs.get(opt.local, i).block != from) continue;

            phi->inputs.remove(opt.local, i);
            break;
        }
    }
}

/*
 * A block containing nothing but a jump, spliced out of the graph.
 *
 * Every predecessor's edge is pointed at the target instead, and the block is left with no
 * predecessors - which is what the reachability sweep then removes it for. The entry block is never
 * spliced: it has no predecessors to redirect and something has to be first.
 */
bool spliceEmptyBlock(OptContext& opt, ModulePtr<Block> pointer) {
    auto block = opt.local[pointer];

    if(block->index == 0) return false;
    if(block->instructions.isNotEmpty() || block->phis.isNotEmpty()) return false;
    if(!block->terminator || opt.local[block->terminator]->kind != Value::Jmp) return false;

    auto target = ((InstJmp&)*opt.local[block->terminator]).target;

    // A block that jumps to itself is an infinite loop rather than an empty one, and a target that
    // merges values cannot take an edge that arrives from somewhere else - see the file comment.
    if(target == pointer) return false;
    if(opt.local[target]->phis.isNotEmpty()) return false;

    for(auto predecessor: block->incoming.contents(opt.local)) {
        auto from = opt.local[predecessor];
        if(!from->terminator) continue;

        auto terminator = opt.local[from->terminator];

        if(terminator->kind == Value::Jmp) {
            ((InstJmp&)*terminator).target = target;
        } else if(terminator->kind == Value::Je) {
            auto& branch = (InstJe&)*terminator;
            if(branch.thenBlock == pointer) branch.thenBlock = target;
            if(branch.elseBlock == pointer) branch.elseBlock = target;
        } else {
            continue;
        }

        for(auto& outgoing: from->outgoing) {
            if(outgoing == pointer) outgoing = target;
        }

        opt.local[target]->incoming.push(opt.program.arena, predecessor);
    }

    // The block's own edge into the target, and its predecessors, both of which are now nobody's.
    removeEdge(opt, target, pointer);
    while(block->incoming.size()) block->incoming.remove(opt.local, block->incoming.size() - 1);

    opt.changed = true;
    return true;
}

bool foldBranch(OptContext& opt, Block& block) {
    if(!block.terminator) return false;

    auto terminator = opt.local[block.terminator];
    if(terminator->kind != Value::Je) return false;

    auto& branch = (InstJe&)*terminator;
    auto condition = constantValueOf(opt, branch.cond);
    if(!condition) return false;

    auto pointer = (ModulePtr<Block>)(&block - opt.local);
    auto taken = condition.unwrap() ? branch.thenBlock : branch.elseBlock;
    auto untaken = condition.unwrap() ? branch.elseBlock : branch.thenBlock;

    // Before the terminator is replaced, because this is the edge the old one owned. Both arms
    // leading to one block is not a special case: there were two edges into it and now there is one.
    removeEdge(opt, untaken, pointer);
    dropUse(opt, branch.cond, block.terminator);

    /*
     * Written into the block directly rather than through `Block::add`, which asserts that a block
     * has no terminator yet and would record the edge into `taken` a second time. That edge is the
     * one being kept, so its bookkeeping is already right and the only thing to change is which
     * instruction ends the block.
     */
    auto jump = createInst<InstJmp>(*opt.module, *opt.function, block, terminator->source, 0,
                                    opt.program.scalar.unit, taken);

    block.terminator = (ModulePtr<Inst>)(jump - opt.local);
    block.outgoing[0] = taken;
    block.outgoing[1] = nullptr;

    opt.changed = true;
    return true;
}

// The blocks nothing reaches, dropped from the function along with their edges into the ones that
// survive. Answers whether anything went, since the block list has to be renumbered if so.
bool removeUnreachableBlocks(OptContext& opt) {
    Array<U8> reachable;
    computeReachable(opt, reachable);

    Array<ModulePtr<Block>> kept;
    for(auto pointer: opt.function->blocks.contents(opt.local)) {
        if(reachable[opt.local[pointer]->index]) kept.push(pointer);
    }

    if(kept.size() == opt.function->blocks.size()) return false;

    // While the old indices still mean something, since `reachable` is indexed by them.
    for(auto pointer: kept) {
        auto block = opt.local[pointer];

        for(Size i = block->incoming.size(); i-- > 0;) {
            auto from = block->incoming.get(opt.local, i);
            if(reachable[opt.local[from]->index]) continue;

            block->incoming.remove(opt.local, i);
        }

        for(auto phiPointer: block->phis.contents(opt.local)) {
            auto phi = opt.local[phiPointer];

            for(Size i = phi->inputs.size(); i-- > 0;) {
                auto input = phi->inputs.get(opt.local, i);
                if(reachable[opt.local[input.block]->index]) continue;

                phi->inputs.remove(opt.local, i);
            }
        }
    }

    // A block's index is its position in this list, which is what every walk in opt_flow.cpp
    // assumes - so compacting the list means renumbering with it.
    opt.function->blocks.clear();

    U16 index = 0;
    for(auto pointer: kept) {
        opt.function->blocks.push(opt.program.arena, pointer);
        opt.local[pointer]->index = index++;
    }

    opt.changed = true;
    return true;
}

// A phi with one alternative left, which is what a folded branch leaves at every join it stranded an
// arm of. Iterated, because a phi that reads another one collapses only after that one has.
void collapseSinglePhis(OptContext& opt) {
    auto changed = true;

    while(changed) {
        changed = false;

        for(auto blockPointer: opt.function->blocks.contents(opt.local)) {
            auto block = opt.local[blockPointer];

            for(Size i = block->phis.size(); i-- > 0;) {
                auto pointer = block->phis.get(opt.local, i);
                auto phi = opt.local[pointer];
                if(phi->inputs.size() != 1) continue;

                auto only = phi->inputs.get(opt.local, 0).value;

                // Its own value is not an alternative, and a phi that is only itself has nothing to
                // become. Nothing produces one here, but replacing a value with itself would be a
                // silent no-op rather than the assertion `replaceValue` is entitled to make.
                if(only == (ModulePtr<Value>)pointer) continue;

                replaceValue(opt, (ModulePtr<Value>)pointer, only);
                dropUse(opt, only, (ModulePtr<Inst>)pointer);
                block->phis.remove(opt.local, i);
                changed = true;
            }
        }
    }
}

}

void foldBranches(OptContext& opt) {
    auto folded = false;
    for(auto blockPointer: opt.function->blocks.contents(opt.local)) {
        folded = foldBranch(opt, *opt.local[blockPointer]) || folded;
    }

    if(!folded) return;

    // Only where a fold happened, which is what keeps this from being a CFG cleanup that runs over
    // every function on every round: an empty block nothing folded is one the resolver emitted and
    // both backends already deal with.
    for(auto blockPointer: opt.function->blocks.contents(opt.local)) {
        spliceEmptyBlock(opt, blockPointer);
    }

    removeUnreachableBlocks(opt);
    collapseSinglePhis(opt);

    // What the deletion left behind: an instruction in a block that no longer exists is still in the
    // use list of everything it read. Rebuilding is the repair, and it is the same one the driver
    // performs once per function for the drop pass's benefit.
    rebuildUses(opt);
}
