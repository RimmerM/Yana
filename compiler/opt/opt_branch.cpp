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
 * **Supplying those alternatives was built, and it is the wrong thing to do** - §44 of
 * test/bench/findings.md. The values are available: this block computes nothing, so what a phi in the
 * target takes on the edge from here is defined above here, and a definition that dominates a block
 * and is not in it dominates every predecessor of that block. So an `IrEditor` operation copying one
 * predecessor's alternatives onto another is fifteen lines and it works. It also costs 38 bytes on
 * `test/resolve/Adaptor.yana` and nothing anywhere else, for the reason the shape exists: **a phi
 * alternative is a copy on an edge, and an empty block in front of a join is where a set of them is
 * shared.** Splicing it hands every predecessor its own copy, and where the predecessor branches it
 * re-creates the critical edge this block was the split of. Restricted to the case that cannot cost -
 * one predecessor, ending in a plain jump - it fires on nothing at all, because `mergeInto` below has
 * already folded that block into its predecessor.
 *
 * So the refusal stands on what it is worth rather than on what it would take, and the count is 146
 * sites over the 233 `test/resolve` programs and none at all on the benchmark corpus.
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

/*
 * Whether a phi in `target` would lose something by this block's two edges into it becoming one.
 *
 * With `je %c, X, X` a phi in `X` holds two alternatives whose `block` is this one, and *nothing
 * distinguishes them*: an alternative names the block it arrives from, not the arm, so which of the
 * two the condition selects is a question the IR has no way to ask. Two alternatives that agree are
 * therefore the only well-formed case, and collapsing them is exact. Two that disagree are a shape
 * no pass may build - what it means is a `Select` - so this declines rather than picking one, and
 * what is left is a `je` the lower IR refuses by name.
 */
bool armsAgree(OptContext& opt, Block& block, ModulePtr<Block> target) {
    auto pointer = (ModulePtr<Block>)(&block - opt.local);

    for(auto phiPointer: opt.local[target]->phis(opt.local)) {
        auto phi = opt.local[phiPointer];
        ModulePtr<Value> first = nullptr;
        auto found = false;

        for(auto input: phi->inputs.contents(opt.local)) {
            if(input.block != pointer) continue;
            if(found && input.value != first) return false;

            first = input.value;
            found = true;
        }
    }

    return true;
}

bool foldBranch(OptContext& opt, Block& block) {
    if(!block.terminator()) return false;

    auto terminator = opt.local[block.terminator()];
    if(terminator->kind != Value::Je) return false;

    auto& branch = (InstJe&)*terminator;

    /*
     * A branch whose two arms are one block, which decides nothing and is a jump.
     *
     * Ahead of the constant test rather than inside it, because the condition is exactly what this
     * case does not have: `spliceEmptyBlock` below redirects each predecessor edge of an empty block
     * on its own, so a `je` whose arms were two empty blocks in front of one join comes out of it
     * naming that join twice. Nothing downstream folds a doubled arm - the resolve IR admits one on
     * purpose, since an arm is an edge and two arms at one block are two edges - and lowering then
     * refuses it: `LowerBlock::addInst` asserts the two are distinct and `validateLowerModule` says
     * "same target block for all branches". `-inline speed` over `File.entries` is where that came
     * out, as a build that produced no executable and named a `yield` as the reason.
     */
    if(branch.thenBlock == branch.elseBlock) {
        if(!armsAgree(opt, block, branch.thenBlock)) return false;

        auto jump = createInst<InstJmp>(*opt.module, *opt.function, block, terminator->source,
                                        StringId(), opt.program.scalar.unit, branch.thenBlock);

        // One of the two edge records goes and the other stays, phi alternative included - the same
        // multiset difference the fold below relies on, with the sets one deep instead of two.
        opt.ir().setTerminator(block, jump);
        return true;
    }

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
    auto& reachable = reachableOf(opt);

    SmallArray<ModulePtr<Block>, 16> kept;
    for(auto pointer: opt.function->blocks.contents(opt.local)) {
        if(reachable[opt.local[pointer]->index]) kept.push(pointer);
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
        if(reachable[opt.local[pointer]->index]) continue;

        opt.ir().discardBlock(*opt.local[pointer]);
    }

    opt.ir().setBlockOrder(Buffer<ModulePtr<Block>>(kept.pointer(), kept.size()));

    opt.changed = true;
    return true;
}

/*
 * The block list, laid back out in reverse postorder from the entry.
 *
 * The invariant `lowerProgram` reads: it walks a function's blocks in list order and requires every
 * operand it meets to have been lowered already, phis excepted. Reverse postorder is exactly that
 * property, because a block precedes everything it dominates and a non-phi use is dominated by its
 * definition.
 *
 * Owed by any rewrite that *moves* an edge rather than removing one - see `threadBooleanBranches`,
 * which is the caller. The inliner has its own copy of the walk over a callee, since it needs the
 * order before the blocks are grafted rather than after; this is the one over a function that is
 * already whole.
 *
 * Whatever the walk does not reach is kept, at the end, on `reorderBlocks`' terms: deleting an
 * unreachable block is `removeUnreachableBlocks`' job and it does it with the phi bookkeeping that
 * belongs to it. Answers whether the order actually changed, so that a pass that only ever rewrites
 * a straight line does not renumber for nothing.
 */
bool reorderBlocksInRpo(OptContext& opt) {
    auto count = opt.function->blocks.size();
    if(count < 2) return false;

    ScratchSet seen(opt.sets, count);
    SmallArray<ModulePtr<Block>, 32> postorder;
    SmallArray<ModulePtr<Block>, 32> pending;
    SmallArray<U32, 32> next;

    seen->set(opt.local[opt.function->blocks.get(opt.local, 0)]->index, true);
    pending.push(opt.function->blocks.get(opt.local, 0));
    next.push(0);

    while(pending.size()) {
        auto pointer = pending[pending.size() - 1];
        auto block = opt.local[pointer];

        if(next[next.size() - 1] < 2) {
            auto successor = block->successor(next[next.size() - 1]++);
            if(!successor) continue;

            auto target = opt.local[successor]->index;
            if((*seen)[target]) continue;

            seen->set(target, true);
            pending.push(successor);
            next.push(0);
            continue;
        }

        postorder.push(pointer);
        pending.pop();
        next.pop();
    }

    SmallArray<ModulePtr<Block>, 32> order;
    for(Size i = postorder.size(); i-- > 0;) order.push(postorder[i]);
    for(auto pointer: opt.function->blocks.contents(opt.local)) {
        if(!(*seen)[opt.local[pointer]->index]) order.push(pointer);
    }

    auto same = true;
    for(Size i = 0; i < order.size() && same; i++) {
        same = order[i] == opt.function->blocks.get(opt.local, i);
    }

    if(same) return false;

    opt.ir().setBlockOrder(Buffer<ModulePtr<Block>>(order.pointer(), order.size()));
    opt.changed = true;
    return true;
}

namespace {

/*
 * One incoming value of a boolean phi, and how its edge can be sent to the branch's destination.
 *
 * Usually the incoming block ends in `jmp join`, so it can end in `je value, then, else` instead.
 * The important short-circuit case has one empty block in between: `pred` has already branched on
 * `value`, one of its edges reaches the empty block, and that block jumps to the join. On that edge
 * the value is a constant fact, so the predecessor can be pointed straight at the corresponding
 * destination and the materialized boolean becomes dead.
 */
struct BooleanThread {
    ModulePtr<Block> incoming;
    ModulePtr<Value> value;

    // Non-null for the second shape: the block whose conditional edge already proves the value.
    ModulePtr<Block> provingPred = nullptr;
    bool provedValue = false;
};

bool planBooleanThread(OptContext& opt, ModulePtr<Block> join, PhiInput input,
                       BooleanThread& into)
{
    auto incoming = opt.local[input.block];
    if(!incoming->terminator() || opt.local[incoming->terminator()]->kind != Value::Jmp) return false;
    if(((InstJmp&)*opt.local[incoming->terminator()]).target != join) return false;

    into = BooleanThread { input.block, input.value };

    // A non-empty incoming block simply gets the branch the join used to hold. The value is a phi
    // alternative from this block, so it dominates the edge by the validity rule for the phi itself.
    if(incoming->instructionCount() != 0 || incoming->phiCount() != 0 ||
       incoming->predecessorCount() != 1)
    {
        return true;
    }

    auto predPointer = incoming->predecessorAt(opt.local, 0);
    auto pred = opt.local[predPointer];
    if(!pred->terminator() || opt.local[pred->terminator()]->kind != Value::Je) return true;

    auto& branch = (InstJe&)*opt.local[pred->terminator()];
    if(branch.cond != input.value) return true;

    // Exactly one arm has to be the edge in question. If both arms arrive here, taking the edge says
    // nothing about the condition and redirectSuccessor would move both of them at once.
    auto onThen = branch.thenBlock == input.block;
    auto onElse = branch.elseBlock == input.block;
    if(onThen == onElse) return true;

    into.provingPred = predPointer;
    into.provedValue = onThen;
    return true;
}

bool threadBooleanBranch(OptContext& opt, ModulePtr<Block> joinPointer) {
    auto join = opt.local[joinPointer];
    if(join->instructionCount() != 0 || join->phiCount() != 1) return false;
    if(!join->terminator() || opt.local[join->terminator()]->kind != Value::Je) return false;

    auto& branch = (InstJe&)*opt.local[join->terminator()];
    auto phiPointer = join->phiAt(opt.local, 0);
    auto phi = opt.local[phiPointer];
    if(branch.cond != (ModulePtr<Value>)phiPointer || phi->useCount() != 1) return false;

    // New edges cannot invent alternatives for a phi in either destination. The useful producer -
    // short-circuit boolean control - leads to ordinary body blocks, so decline the harder CFG case
    // rather than growing a second phi-repair mechanism beside IrEditor.
    //
    // Counted, this refuses **nothing**: not one site over the 233 `test/resolve` programs and not
    // one over the benchmark corpus. The sentence above describes a case the language does not
    // produce, and §44 of test/bench/findings.md is where that was measured.
    if(opt.local[branch.thenBlock]->phiCount() != 0 || opt.local[branch.elseBlock]->phiCount() != 0) {
        return false;
    }
    if(branch.thenBlock == joinPointer || branch.elseBlock == joinPointer) return false;

    SmallArray<BooleanThread, 8> threads;
    for(auto input: phi->inputs.contents(opt.local)) {
        BooleanThread thread;
        if(!planBooleanThread(opt, joinPointer, input, thread)) return false;
        threads.push(thread);
    }

    if(threads.size() != join->predecessorCount()) return false;

    for(auto& thread: threads) {
        if(thread.provingPred) {
            auto target = thread.provedValue ? branch.thenBlock : branch.elseBlock;
            auto pred = opt.local[thread.provingPred];

            // The planner established one matching arm, so moving anything other than one edge is a
            // broken invariant and stopping here is safer than silently changing both arms.
            if(opt.ir().redirectSuccessor(*pred, thread.incoming, target) != 1) return false;
            continue;
        }

        auto incoming = opt.local[thread.incoming];
        auto oldTerminator = opt.local[incoming->terminator()];
        auto replacement = createInst<InstJe>(
            *opt.module, *opt.function, *incoming, oldTerminator->source, StringId(),
            opt.program.scalar.unit, thread.value, branch.thenBlock, branch.elseBlock);

        opt.ir().setTerminator(*incoming, replacement);
    }

    return true;
}

}

bool threadBooleanBranches(OptContext& opt) {
    SmallArray<ModulePtr<Block>, 64> blocks;
    for(auto pointer: opt.function->blocks.contents(opt.local)) blocks.push(pointer);

    auto changed = false;
    for(auto pointer: blocks) changed = threadBooleanBranch(opt, pointer) || changed;
    if(!changed) return false;

    // Every predecessor of a threaded join now goes to one of its successors, so the join and any
    // empty relay blocks have no way in. The ordinary CFG cleanup owns removing them and collapsing
    // whatever single-predecessor boundaries that leaves.
    removeUnreachableBlocks(opt);
    mergeBlocks(opt);

    /*
     * And the order, which is this pass's own to restore and not the cleanup's.
     *
     * Everything above *moves* an edge rather than removing one, and that is the operation reverse
     * postorder does not survive: a predecessor that used to jump to the join now branches to one of
     * the join's successors, so a block that was reached only through the join can end up listed in
     * front of blocks the new edges reach first. Removing edges cannot do this - it only adds
     * domination, and the pairs already in the list were already ordered - which is why
     * `removeUnreachableBlocks` and `mergeBlocks` filter the list rather than rebuilding it.
     *
     * What breaks is invisible here and fatal two stages later: `lowerProgram` walks the blocks in
     * list order and asserts that every operand it meets has already been lowered, so an out-of-order
     * list is `resolve value was used before it was lowered` from inside `mappedValue`, or - in a
     * build without the assertions - a segmentation fault with nothing to point at.
     *
     * A `while` inside the taken arm of a short-circuit `&&` is the shape that reaches it, which is
     * why this went unnoticed: the threading has to move an edge *into* a region whose own blocks
     * were laid out after the join.
     */
    reorderBlocksInRpo(opt);

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

                /*
                 * The storage first, where what is left occupies none.
                 *
                 * A phi of a memory type *is* a slot - each arm writes the value into it, and the
                 * readers below name the slot rather than the phi. Collapsing it repoints that slot
                 * at whatever is left, and that is a copy made by not making one: it is valid only
                 * while the surviving value is itself in memory. A `String` constant is on one
                 * target and is not on the other, so `if c then "b" else ""` folded against a known
                 * `c` left `move %local1` reading a local that nothing on JS had written, and the
                 * emitter answered `null`. See `valueOccupiesStorage`, and the same fix in
                 * `spliceStraightLine` - this is the second pass to repoint a slot and the second
                 * one to need it.
                 *
                 * So the storage is made here and written into, which is what the arms were doing
                 * before the fold removed them. In front of the block rather than beside the value:
                 * the phi is the first thing in its block, and what replaces it has to dominate
                 * everything the phi did.
                 */
                auto slot = phi->slot;

                /*
                 * Only for a memory type, and the guard is the ruling rather than a precaution.
                 *
                 * A slot holding a *direct* value is a register value with a name, which is what
                 * every promoted local in the IR is - repointing one at another register value is
                 * exactly right, and materializing storage for it would hand the backend an address
                 * where the type says there is a number. It did: `continuation$26` in Lens.yana
                 * returned `alloca` instead of the `I32` it is declared to return, and the lowered
                 * golden is the only thing that said so.
                 */
                if(slot < opt.function->localCount() && isMemoryType(opt.global, phi->type) &&
                   !valueOccupiesStorage(opt.local, *opt.function, only)) {
                    auto& module = *opt.module;
                    auto& function = *opt.function;
                    auto source = phi->source;
                    auto type = phi->type;

                    InstList written;
                    auto storage = createInst<InstAlloc>(module, function, *block, source,
                                                         StringId(), type, slot);
                    written.push((Inst*)storage);
                    written.push((Inst*)createInst<InstInit>(
                        module, function, *block, source, StringId(), module.scalar.unit,
                        Place::inLocal(slot), only, Value::Init));

                    opt.ir().insert(*block, 0, written);
                    only = (ModulePtr<Value>)((Value*)storage - opt.local);
                }

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

/*
 * A block whose last call does not come back, ended there - §10 item 2 of test/bench/findings.md.
 *
 * Every bounds check and every `@bits` narrowing is `checkCondition`, which the inliner takes: what
 * arrives here is `je %failed, abort, ok` with `call checkFailed` in the abort arm and both arms
 * jumping to the block below the check. The abort arm does not come back - `checkFailed` is a
 * declared `noReturn`, see `Function::noReturn` - so that second edge is one no execution ever
 * takes, and everything it costs is paid by the code that does execute:
 *
 *  - the block below the check is a *join*, so it starts a new block-local scope. `forwardPlaces`
 *    stops there, and so does anything else that reasons within a block.
 *  - `killBetween` in opt_value.cpp finds the abort arm on a path between two reads of one place, so
 *    `out.length` read by two checks of one index is two loads, and the second check is then a
 *    comparison of two values the first check's are not.
 *  - the branch itself survives, since `je` on a condition that has not folded is a branch.
 *
 * With the edge gone the join has one predecessor, `mergeBlocks` folds it back, and all three go at
 * once. `Matrix`'s innermost loop is where this was measured; it is the largest single piece of it.
 *
 * The call is the last instruction of its block by construction, because the inliner refuses a
 * `noReturn` callee - which is what keeps the fact readable at all, and is the other half of this
 * item. A call in the middle of a block would leave instructions behind it that are equally dead;
 * they are left alone, because nothing produces one and a rule with no producer is a rule with no
 * test.
 */
bool endNonReturningBlocks(OptContext& opt) {
    auto ended = false;

    for(auto pointer: opt.function->blocks.contents(opt.local)) {
        auto block = opt.local[pointer];

        auto terminator = block->terminator();
        if(!terminator || opt.local[terminator]->kind == Value::Unreachable) continue;

        auto count = block->instructionCount();
        if(!count) continue;

        auto& last = *opt.local[block->instructionAt(opt.local, count - 1)];
        if(last.kind != Value::Call) continue;

        auto callee = ((InstCall&)last).callee;
        if(!callee || !opt.local[callee]->noReturn) continue;

        auto end = createInst<InstUnreachable>(*opt.module, *opt.function, *block,
                                               opt.local[terminator]->source, StringId(),
                                               opt.program.scalar.unit);

        // Every edge this block owned goes, with the phi alternatives that arrived over each: the
        // new successor set is empty, so `setTerminator`'s multiset diff removes all of them.
        opt.ir().setTerminator(*block, end);
        ended = true;
    }

    if(!ended) return false;

    // The same cleanup a folded branch owes, and for the same reasons in the same order: the arm
    // that stopped leading anywhere may have been a block's only way in, a join left with one
    // predecessor keeps its phis until they are collapsed, and a block with phis is one the merge
    // declines.
    removeUnreachableBlocks(opt);
    collapseSinglePhis(opt);
    mergeBlocks(opt);

    opt.changed = true;
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

    // And the branches the splice above left naming one block twice, before the merge rather than on
    // the next round: a doubled arm is two ways into the target, so `mergeInto` declines it and a
    // join that could have been absorbed here would wait for a round that may not come.
    for(auto blockPointer: opt.function->blocks.contents(opt.local)) {
        foldBranch(opt, *opt.local[blockPointer]);
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
