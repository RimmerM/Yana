#include "opt_pass.h"

/*
 * A branch whose condition is a constant, and the blocks that stop being reachable because of it.
 *
 * This is the one rewrite in the directory that changes the *shape* of the function rather than what
 * an instruction computes, which is why opt_fold.cpp left it alone: dominance, the natural loops,
 * every phi's alternatives and the ownership passes' block-level facts all rest on the graph, and a
 * pass that edits an edge without editing all four leaves an IR that prints correctly and walks
 * wrongly. So it is here, it does the whole job, and it does it in four steps.
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
 * The use lists are repaired as the blocks go rather than rebuilt afterwards. What changed is which
 * instructions exist at all, and an instruction in a deleted block is a reader of values that are
 * still live elsewhere - which is what `IrEditor::discardBlock` settles at the point the block stops
 * existing. `rebuildUses` used to be the answer, and is now a repair nothing here needs.
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
 * A target that still has phis is left alone as well, because redirecting an edge *around* a block
 * means every phi in the target needs one alternative per predecessor of the block that went away -
 * and where a predecessor could already reach the target directly, the two alternatives it would
 * then have are not required to agree.
 *
 * ## The join a deleted arm leaves, which is the other half
 *
 * `mergeBlocks` is the fourth step and the one shared with opt_inline.cpp - see the comment on
 * `mergeInto`. Deleting an arm leaves the join it fed with one way in, which is a block boundary
 * with nothing on either side of it, and the block-local passes below stop at every one of those.
 * It runs after `collapseSinglePhis` rather than beside the splice above, because a join keeps its
 * phis until that step has answered them and a block with phis is one the merge declines.
 */

namespace {

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
    if(block->instructionCount() != 0 || block->phiCount() != 0) return false;
    if(!block->terminator() || opt.local[block->terminator()]->kind != Value::Jmp) return false;

    auto target = ((InstJmp&)*opt.local[block->terminator()]).target;

    // A block that jumps to itself is an infinite loop rather than an empty one, and a target that
    // merges values cannot take an edge that arrives from somewhere else - see the file comment.
    if(target == pointer) return false;
    if(opt.local[target]->phiCount() != 0) return false;

    /*
     * Every predecessor pointed at the target instead. Taken from the front rather than walked,
     * because a redirect removes the entry it acted on - a predecessor whose two arms both led here
     * is two entries and leaves as two - so the list this reads is the one it is emptying.
     *
     * Which means the loop terminates only if every round removes one, and that is a property of the
     * *IR* rather than of this code: a predecessor ends, by construction, in the instruction that
     * made the edge. So it is the redirect's own answer that is tested rather than anything read off
     * the terminator here - a stale entry is a broken IR, and stopping on one leaves a consistent
     * function (what has moved reaches `target` directly, what has not still arrives here) where
     * trusting the invariant leaves a compiler that hangs with nothing to say.
     */
    while(block->predecessorCount()) {
        auto from = opt.local[block->predecessorAt(opt.local, 0)];
        if(!opt.ir().redirectSuccessor(*from, pointer, target)) return false;
    }

    // And the block's own way out, which is now nobody's.
    opt.ir().clearTerminator(*block);

    opt.changed = true;
    return true;
}

/*
 * A block with one predecessor, folded back into it - the inverse of an edge split, and the one CFG
 * cleanup in this directory that removes a *join* rather than an arm.
 *
 * Two passes make the shape and neither is in a position to clean up after the other. Inlining cuts
 * the caller's block in two at the call and grafts a body into the gap, which leaves the second half
 * with one way in wherever the callee has one `ret`; folding deletes an arm, which leaves the join
 * it fed with one predecessor and a phi that `collapseSinglePhis` then answers.
 *
 * Leaving them costs more than tidiness. The passes that forward a read to the write above it are
 * *block-local*, so a body arriving in four blocks where it could have arrived in two is a body half
 * of whose stores stop being answerable - and on a managed target every extra block is one more join
 * for codegen/js/flow.cpp to find a structured form of.
 *
 * Three guards, and each one is a case this must not touch:
 *
 *  - the predecessor has to end in a plain `jmp`. A `je` cannot absorb anything, since the block
 *    would then have instructions after its own terminator;
 *  - the target has to have exactly one predecessor, and it has to be this one. That is what makes
 *    the concatenation unconditional rather than a guess about which way control came;
 *  - the target must have no phis. With one predecessor a phi is its one alternative, but saying so
 *    is `collapseSinglePhis`' rule and there is no reason to keep a second copy of it here.
 *
 * A loop is safe by construction rather than by a check: a header has a back edge, so it has two
 * predecessors, so it is never a target.
 */
bool mergeInto(OptContext& opt, Block& into, ModulePtr<Block> pointer) {
    auto block = opt.local[pointer];
    auto intoPointer = (ModulePtr<Block>)(&into - opt.local);

    if(pointer == intoPointer || block->index == 0) return false;
    if(block->phiCount() != 0 || !block->terminator()) return false;
    if(block->predecessorCount() != 1 || block->predecessorAt(opt.local, 0) != intoPointer) return false;

    opt.ir().spliceInto(into, *block);
    return true;
}

bool foldBranch(OptContext& opt, Block& block) {
    if(!block.terminator()) return false;

    auto terminator = opt.local[block.terminator()];
    if(terminator->kind != Value::Je) return false;

    auto& branch = (InstJe&)*terminator;
    auto condition = constantValueOf(opt, branch.cond);
    if(!condition) return false;

    auto taken = condition.unwrap() ? branch.thenBlock : branch.elseBlock;

    /*
     * `setTerminator` is what makes this three lines: the edge into `taken` is in both the old
     * successor set and the new one, so it is left exactly as it was - phi alternatives included -
     * and only the edge into `untaken` goes. Both arms leading to one block is not a special case
     * either: there were two edges into it and now there is one.
     */
    auto jump = createInst<InstJmp>(*opt.module, *opt.function, block, terminator->source, StringId(),
                                    opt.program.scalar.unit, taken);

    opt.ir().setTerminator(block, jump);
    return true;
}

}

// The blocks nothing reaches, dropped from the function along with their edges into the ones that
// survive. Answers whether anything went, since the block list has to be renumbered if so.
//
// Outside the anonymous namespace because it is the cleanup every CFG rewrite in this directory
// owes: folding a branch strands an arm, and if-conversion splices two of them out at once.
bool removeUnreachableBlocks(OptContext& opt) {
    ScratchSet reachable(opt.sets, 0);
    computeReachable(opt, *reachable);

    SmallArray<ModulePtr<Block>, 16> kept;
    for(auto pointer: opt.function->blocks.contents(opt.local)) {
        if((*reachable)[opt.local[pointer]->index]) kept.push(pointer);
    }

    if(kept.size() == opt.function->blocks.size()) return false;

    /*
     * The departing blocks, discarded: every value in one stops being a reader of what it read,
     * every slot one of them filled is emptied, and every edge one of them owned is removed from the
     * surviving successor that recorded it - predecessor entry and phi alternative together.
     *
     * All three used to be split up here, and the third was left for `rebuildUses` afterwards
     * because an instruction in a deleted block is still in the use list of everything it read. It
     * no longer is: this is where a block stops existing, so this is where it stops counting.
     */
    for(auto pointer: opt.function->blocks.contents(opt.local)) {
        if((*reachable)[opt.local[pointer]->index]) continue;

        opt.ir().discardBlock(*opt.local[pointer]);
    }

    opt.ir().setBlockOrder(Buffer<ModulePtr<Block>>(kept.pointer(), kept.size()));

    opt.changed = true;
    return true;
}

namespace {

// A phi with one alternative left, which is what a folded branch leaves at every join it stranded an
// arm of. Iterated, because a phi that reads another one collapses only after that one has.
void collapseSinglePhis(OptContext& opt) {
    auto changed = true;

    while(changed) {
        changed = false;

        for(auto blockPointer: opt.function->blocks.contents(opt.local)) {
            auto block = opt.local[blockPointer];

            for(Size i = block->phiCount(); i-- > 0;) {
                auto pointer = block->phiAt(opt.local, i);
                auto phi = opt.local[pointer];
                if(phi->inputs.size() != 1) continue;

                auto only = phi->inputs.get(opt.local, 0).value;

                // Its own value is not an alternative, and a phi that is only itself has nothing to
                // become. Nothing produces one here, but replacing a value with itself would be a
                // silent no-op rather than the assertion `replaceValue` is entitled to make.
                if(only == (ModulePtr<Value>)pointer) continue;

                opt.ir().replaceValue((ModulePtr<Value>)pointer, only);

                // The slots this phi filled, which `replaceValue` does not reach: a slot names the
                // value its storage came from rather than reading it, so the storage follows the phi
                // into whatever it became. Before the removal rather than after it, because
                // `erasePhi` empties every slot the phi was the whole contents of.
                opt.ir().repointLocalValue((ModulePtr<Value>)pointer, only);
                opt.ir().erasePhi(pointer);
                changed = true;
            }
        }
    }
}

}

bool mergeBlocks(OptContext& opt) {
    auto merged = false;

    // Over a snapshot, since the walk below reads the list while `removeUnreachableBlocks` rewrites
    // it - and a merged block is one of the ones that goes.
    SmallArray<ModulePtr<Block>, 64> blocks;
    for(auto pointer: opt.function->blocks.contents(opt.local)) blocks.push(pointer);

    for(auto pointer: blocks) {
        auto block = opt.local[pointer];

        // Repeatedly, because absorbing one block leaves this one ending in *that* block's jump -
        // which is how a chain of them collapses in one visit rather than one per round.
        while(block->terminator() && opt.local[block->terminator()]->kind == Value::Jmp) {
            if(!mergeInto(opt, *block, ((InstJmp&)*opt.local[block->terminator()]).target)) break;

            merged = true;
        }
    }

    if(!merged) return false;

    // What a merge leaves is a block with no terminator and no way in, which is exactly what the
    // reachability sweep already removes - and it renumbers, which the block list needs either way.
    removeUnreachableBlocks(opt);
    return true;
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

    /*
     * And the joins the deletion left with one way in, which is the step that could not have run any
     * earlier: a join keeps its phis until `collapseSinglePhis` has answered them, and a block with
     * phis is one `mergeInto` declines. So the arm that went away is what makes the merge possible,
     * and the merge is what keeps a folded diamond from leaving four blocks where one would do.
     */
    mergeBlocks(opt);
}
