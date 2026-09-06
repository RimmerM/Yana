#include "pass.h"

/*
 * The graph itself: the branches that decide nothing, the blocks nothing reaches, and the block
 * boundaries with nothing on either side of them - README.md's section B, "One `cleanCfg`".
 *
 * Three rewrites in one worklist over blocks, because each of the three produces the shape the other
 * two consume. A folded branch strands an arm; deleting the arm leaves the join it fed with one way
 * in; merging that join back leaves *its* terminator here, which may be another branch on a constant.
 * The shipping stage runs `foldBranches`, `removeUnreachableBlocks` and `mergeBlocks` as three passes
 * and re-runs the group until it settles; here they are one queue that empties.
 *
 * `threadBranches` is at the bottom of this file rather than inside that walk, and section B says
 * why: it *creates* edges, so it cannot run inside a loop holding a block cursor, and the shipping
 * stage deliberately puts it behind if-conversion because a diamond that can be one `select` should
 * be one.
 *
 * ## What is new here, and it is the region rule
 *
 * `Continuation` says which region owns each block, and the verifier holds every edge to it: an edge
 * may stay inside a region, enter a **direct child at its `entry`**, or leave for the `destination`
 * that child declared. Nothing about a diamond's shape says which of its arms is a cleanup, so a
 * pass that reroutes an edge by looking at the graph alone is a pass that can thread a branch
 * straight past the flag test a teardown arm begins with - which is `checkEdgeRegion`'s report and
 * the reason it exists.
 *
 * Every rewrite below therefore takes the same shape of decision, and it is deliberately the blunt
 * one: **a rewrite that moves or removes an edge is confined to one region.** A merge folds a block
 * into a predecessor in the same region; a splice redirects a predecessor to a target in the same
 * region; a thread sends a predecessor to an arm in the same region. Anything crossing a boundary is
 * declined rather than reasoned about, because the only thing that could make it legal is
 * reproducing the verifier's case analysis at every rewrite site, and two statements of one rule is
 * what this codebase spends its comments avoiding.
 *
 * The second half is the two fields. `Continuation::entry` and `Continuation::destination` name
 * blocks, and a rewrite that deleted one of them would leave a region naming a block that is not
 * one. **No block named by either field is ever merged or spliced away** - `isAnchor` below - which
 * is the decision that keeps those fields read-only in this directory. Updating them through the
 * editor was the alternative and it is worse: `entry` is the one point a region may be entered at,
 * so moving it is changing what the region *is*, and there is nothing at a merge site that knows
 * whether that was intended. It also costs nothing measurable, because the two shapes it declines -
 * folding a cleanup arm's first block into the flag test above it, folding a region's rejoin point
 * into the arm that reaches it - are both merges *across* a region boundary that the rule above has
 * already refused.
 *
 * A deleted block is the one exception, and it is not one: an unreachable region is unreachable at
 * its entry, so a live edge can never name a block a sweep took out. See `removeUnreachable`.
 *
 * ## The linear rule, and why an unreachable block owes nothing
 *
 * Deleting a block deletes the instructions in it, and some of those consume owners and some are
 * `Drop`s. That is sound for the reason the verifier's own dataflow gives: the linear rule is a
 * statement about the paths through a body, an unreachable block is on no path, and a `Drop` there
 * runs never. An owned value *defined* in a dead block owes a consumption on the paths that reach
 * its definition, and there are none; an owned value defined in a live block and consumed in a dead
 * one is a value whose consumption was never on a path either, so what the erase removes is an
 * obligation that was already discharged everywhere it existed. What would not be sound is deleting
 * a *reachable* block, which is why the sweep is driven by `reachableOf` and never by "this block
 * looks empty".
 *
 * ## What the shipping stage does here and this does not
 *
 * **`reorderBlocksInRpo` has no counterpart.** Over there, `Block::index` *is* the position in
 * `Function::blocks`, the lowering walks that list in order, and a pass that moves an edge can leave
 * a block listed in front of the blocks defining the values it reads - a wrong number with no
 * diagnostic ([[yana-resolve-cfg-invariants]]). Here `Lowering::lowerBody` takes the shared reverse
 * postorder, which phase 1 changed it to for exactly this reason, so moving an edge owes nothing.
 * That is what makes the splice below affordable at all.
 *
 * **`collapseSinglePhis` has no counterpart either**, and its absence is the block-argument
 * representation rather than a decision: a join left with one predecessor still declares its
 * arguments, and what answers them is the merge below, which replaces each argument by the operand
 * the surviving edge hands it. The shipping stage needs a separate pass because a phi is a value in
 * the block and can be collapsed without merging anything; here the two are the same rewrite.
 */

namespace ssa {

namespace {

struct CfgCleaner {
    OptContext& opt;

    /*
     * The queue over blocks, and the bit that keeps one from being on it twice - `simplify`'s
     * shape, over `BlockId` instead of `InstId`.
     *
     * A head index rather than a stack for the same reason: the seeding walk's order is the block
     * table's, so a chain of merges collapses in one sweep rather than one per round. Unlike the
     * instruction queue this one never grows past the table it was sized from - nothing here makes
     * a block - but it is written the same way so that the two read alike.
     */
    Array<U32> queue;
    Array<U8> marked;
    Size head = 0;

    void mark(Size index, U8 value) {
        while(marked.size() <= index) marked.push(0);
        marked[index] = value;
    }

    void push(Block* block) {
        if(!block || block->deleted) return;

        auto index = Size(block->index.index);
        if(index < marked.size() && marked[index]) return;

        mark(index, 1);
        queue.push(U32(index));
    }

    void push(SsaPtr<Block> pointer) {
        if(pointer) push(opt.base[pointer]);
    }

    // Everything that reaches this block and everything it reaches - what a rewrite of its
    // terminator or its instruction list has just changed the shape of.
    void pushNeighbours(Block& block) {
        for(auto pointer: block.incoming(opt.base)) push(pointer);

        if(auto last = block.terminator(opt.base)) {
            forEachEdge(*last, [&](Edge& edge) { push(edge.target); });
        }
    }

    /*
     * Whether a `Continuation` names this block as its `entry` or its `destination` - the file
     * comment's second half, and the whole of what keeps those two fields out of this directory.
     *
     * The body's own region names the entry block, so this also states "the entry block is never
     * merged or spliced away" without a case of its own - which `Editor::deleteBlock` asserts
     * anyway, and asserting is not the same as declining before the edit.
     */
    bool isAnchor(Block& block) {
        for(auto pointer: opt.body->regions.contents(opt.base)) {
            auto region = opt.base[pointer];

            if(region->entry.valid() && region->entry == block.index) return true;
            if(region->destination.valid() && region->destination == block.index) return true;
        }

        return false;
    }

    // Whether this block is the one point its own region may be entered at. An edge from *inside*
    // the region to it is a re-entry, which is what `Continuation::multiplicity` forbids for a
    // cleanup arm - so a redirect that would produce one is declined at the site rather than
    // reported at the checkpoint.
    bool isRegionEntry(Block& block) {
        auto region = opt.body->region(opt.base, block.region);
        return region && region->entry.valid() && region->entry == block.index;
    }

    // Whether two edges hand their target the same operands. `branch %c, X, X` is legal and the two
    // edges are two independent argument lists, so folding it to one edge is exact only where the
    // two agree - see `foldBranch`.
    bool edgesAgree(Edge& a, Edge& b) {
        if(a.arguments.size() != b.arguments.size()) return false;

        for(Size i = 0; i < a.arguments.size(); i++) {
            if(a.arguments.get(opt.base, i).packed != b.arguments.get(opt.base, i).packed) {
                return false;
            }
        }

        return true;
    }

    /*
     * One terminator built again, detached, from another that is still standing - what a merge needs
     * and the editor deliberately does not have.
     *
     * A terminator cannot be *moved*: `moveInst` refuses one because the edges move with it, and
     * `replaceTerminator` takes a `make*` form precisely because the block being written still holds
     * the one being replaced. So the merge copies the operands into a fresh instruction, places that
     * in the predecessor, and erases the original - which leaves both recorded at their shared
     * successors for the length of one call and neither afterwards.
     *
     * The branch case reads **one** argument list because that is the only shape there is:
     * `Editor::makeBranch` builds one list and pushes it onto both edges, so a branch whose two
     * edges disagree is IR nothing in this stage produces. `mergeInto` checks it anyway before it
     * starts editing, since the alternative is discovering it half way through.
     */
    Inst* cloneTerminator(Editor& editor, Inst& from) {
        switch(from.kind) {
            case Inst::Return: {
                auto& written = (InstReturn&)from;
                SmallArray<Use, 4> values;
                for(auto used: written.values.contents(opt.base)) values.push(used);

                return editor.makeReturn(from.source, values.pointer(), values.size());
            }

            case Inst::Jump: {
                auto& written = (InstJump&)from;
                SmallArray<Use, 4> arguments;
                for(auto used: written.to.arguments.contents(opt.base)) arguments.push(used);

                return editor.makeJump(from.source, *opt.base[written.to.target],
                                       arguments.pointer(), arguments.size());
            }

            case Inst::Branch: {
                auto& written = (InstBranch&)from;
                SmallArray<Use, 4> arguments;
                for(auto used: written.whenTrue.arguments.contents(opt.base)) arguments.push(used);

                return editor.makeBranch(from.source, written.condition,
                                         *opt.base[written.whenTrue.target],
                                         *opt.base[written.whenFalse.target],
                                         arguments.pointer(), arguments.size());
            }

            default:
                return editor.makeUnreachable(from.source);
        }
    }

    /*
     * A branch that decides nothing, which is a jump - the first of the three.
     *
     * Two shapes, and the second is not the interesting one. A **constant condition** is what
     * arrives from everywhere else: `while True`, a `match` on a value the caller wrote as a
     * literal, a comparison the folder answered. **Both arms at one block** is what this pass makes
     * itself - the splice below points a predecessor at a target its other arm may already reach -
     * and it has to be folded because the tier below refuses one by name: `validateLowerModule`
     * reports "same target block for all branches".
     *
     * The doubled arm folds only where the two edges hand over the same operands. Two that disagree
     * mean the condition selects between them, which is a `Select` and not a jump, and nothing in
     * this stage builds one - so this declines rather than choosing an arm.
     *
     * `replaceTerminator` is what makes the rest three lines. The surviving edge is in both the old
     * successor multiset and the new one, so it is left exactly as it was, arguments included, and
     * only the untaken arm loses its `incoming` entry. There is nothing at this site that knows what
     * the taken arm was being handed, which is the whole argument for the multiset rule.
     */
    bool foldBranch(Block& block) {
        auto last = block.terminator(opt.base);
        if(!last || last->kind != Inst::Branch) return false;

        auto& branch = (InstBranch&)*last;
        if(!branch.whenTrue.target || !branch.whenFalse.target) return false;

        Edge* taken = nullptr;

        if(branch.whenTrue.target == branch.whenFalse.target) {
            if(!edgesAgree(branch.whenTrue, branch.whenFalse)) return false;

            taken = &branch.whenTrue;
        } else {
            if(!branch.condition.isSet()) return false;

            auto condition = constantValueOf(opt, opt.base[branch.condition.value()]);
            if(!condition) return false;

            taken = condition.unwrap() ? &branch.whenTrue : &branch.whenFalse;
        }

        SmallArray<Use, 4> arguments;
        for(auto used: taken->arguments.contents(opt.base)) arguments.push(used);

        auto target = opt.base[taken->target];
        auto editor = opt.ir();
        editor.replaceTerminator(block, editor.makeJump(last->source, *target, arguments.pointer(),
                                                        arguments.size()));

        push(&block);
        pushNeighbours(block);
        return true;
    }

    /*
     * Whether the block argument at one edge may simply *be* the operand that edge hands it - the
     * precondition `Editor::replaceAllUses` asserts, asked before the merge starts rather than
     * discovered in the middle of one.
     *
     * The conventions agree by construction: `checkEdge` compares an edge's operand against the
     * argument it fills and reports a mismatch, so this is the verifier's own rule read back. What
     * is *not* by construction is the loan: a `Guaranteed` argument and a `Guaranteed` operand may
     * depend on different storage, and a borrow rerouted onto a different body-local loan is one
     * whose uses are measured against endpoints in a different place. The caller's loan - an empty
     * storage set, which never ends inside this body - is the one substitution that cannot move an
     * endpoint past a use, and it is admitted for that reason.
     */
    bool canSubstitute(Value& argument, Value& supplied) {
        if(&argument == &supplied) return false;
        if(argument.convention != supplied.convention) return false;
        if(argument.convention != Convention::Guaranteed) return true;

        if(!supplied.loan) return false;
        if(supplied.loan == argument.loan) return true;

        return opt.base[supplied.loan]->storage.size() == 0;
    }

    /*
     * A block with one way in, folded back into the block it leaves from - the second of the three,
     * and the one that pays.
     *
     * What makes the shape is everything else: folding a branch deletes an arm and leaves the join
     * it fed with one predecessor, and phase 9's inliner will cut a caller's block in two and leave
     * the second half with one way in wherever the callee has one `return`. What leaving it costs is
     * every block-local pass below - `forwardMemory` and the block-local half of `simplify` both
     * stop at a boundary that decides nothing.
     *
     * Five guards, and each is a case this must not touch:
     *
     *  - the predecessor ends in a plain **jump**. A branch cannot absorb anything, since the block
     *    would then hold instructions behind its own terminator;
     *  - the block has **exactly one** `incoming` entry, which - the list holding one per edge -
     *    makes that entry this predecessor and makes the concatenation unconditional rather than a
     *    guess about which way control came. `branch %c, X, X` is two entries and is declined here,
     *    correctly: `foldBranch` above is what turns it into one;
     *  - **the same region**, which is the file comment's rule. A block in a child region folded
     *    into its parent would carry that region's blocks' entry point into the parent, and every
     *    edge into what is left would then be entering a region at a block that is not its `entry`;
     *  - **not a region anchor** - see `isAnchor`;
     *  - and each block argument has to be substitutable by the operand the jump hands it, which is
     *    `canSubstitute`.
     *
     * A **loop header is safe by construction rather than by a check**, which is the shipping
     * stage's observation and holds here for the same reason: a header has a back edge, so it has at
     * least two `incoming` entries, so it is never the block being folded. A block that jumps to
     * itself is the degenerate case of the same thing - its one predecessor is itself - and is
     * declined outright, since there is nothing to fold it into.
     */
    bool mergeInto(Block& into) {
        auto last = into.terminator(opt.base);
        if(!last || last->kind != Inst::Jump) return false;

        auto& jump = (InstJump&)*last;
        if(!jump.to.target) return false;

        auto block = opt.base[jump.to.target];
        if(block == &into || block->deleted) return false;
        if(block->predecessorCount() != 1) return false;
        if(block->region != into.region) return false;
        if(isAnchor(*block)) return false;

        auto end = block->terminator(opt.base);
        if(!end) return false;

        // The one thing `cloneTerminator` cannot copy - see the note there.
        if(end->kind == Inst::Branch) {
            auto& branch = (InstBranch&)*end;
            if(!edgesAgree(branch.whenTrue, branch.whenFalse)) return false;
        }

        // The operands read out before anything moves: `removeBlockArgument` takes each of them off
        // this jump as it goes, so the list this walks is the one it is emptying.
        SmallArray<Use, 4> operands;
        for(auto used: jump.to.arguments.contents(opt.base)) operands.push(used);

        for(Size i = 0; i < operands.size(); i++) {
            if(!operands[i].isSet()) return false;

            auto argument = opt.base[block->argumentAt(opt.base, i)];
            if(!canSubstitute(*argument, *opt.base[operands[i].value()])) return false;
        }

        auto editor = opt.ir();

        // From the last, so that removing one does not shift the ones still to come. Each argument's
        // readers are pointed at the operand first, which is what `removeBlockArgument` requires.
        for(Size i = operands.size(); i-- > 0;) {
            auto argument = opt.base[block->argumentAt(opt.base, i)];
            editor.replaceAllUses(*argument, *opt.base[operands[i].value()]);
            editor.removeBlockArgument(*block, i);
        }

        // The instructions, in order, immediately in front of the jump that is about to go. By
        // `moveInst`, which is an unlink and a relink: re-adding one would register every operand a
        // second time, which reads afterwards as a value with more readers than it has.
        for(;;) {
            if(!block->instructionCount()) break;

            auto inst = opt.base[block->instructionAt(opt.base, 0)];
            if(isTerminator(inst->kind)) break;

            editor.moveInst(*inst, into, into.instructionCount() - 1);
        }

        // And the way out. The copy is placed first and the original erased after it, so the shared
        // successors carry both records for the length of one call - `replaceTerminator` drops the
        // jump into `block`, and the erase drops what `block` itself owned.
        editor.replaceTerminator(into, cloneTerminator(editor, *end));
        editor.eraseInst(*end);
        editor.deleteBlock(*block);

        push(&into);
        pushNeighbours(into);
        return true;
    }

    /*
     * A block holding nothing but a jump, spliced out by pointing every predecessor at its target -
     * the third of the three, and the one that is restricted rather than general.
     *
     * **No block arguments anywhere**, which is three conditions at once: the block declares none,
     * so every edge into it supplies none and fits its target; the target declares none, so the
     * redirect is the same edge at a new address; and there is nothing to merge, so nothing about
     * the shape has to be reasoned about. That is not a convenience. §44 of `test/bench/findings.md`
     * measured the general form on the shipping stage and it **costs**: an empty block in front of a
     * join is where a set of edge copies is *shared*, so splicing it hands every predecessor its own
     * copy and re-creates the critical edge the block was the split of. Restricted to the case that
     * cannot cost, the shipping stage measured it firing on nothing at all - because `mergeInto` had
     * already folded every such block into its predecessor.
     *
     * Here it fires, and the producer is `threadBranches` below. A threaded edge leaves its
     * predecessor holding a jump to somewhere else and nothing else at all, and that block's own
     * predecessor is a branch - which `mergeInto` declines, since a branch absorbs nothing. So the
     * relay stays unless this removes it, and the whole of what threading bought is then one block
     * of indirection deeper than it was.
     *
     * The region rule is the same rule in its third spelling: the target is in this block's region,
     * and so is every predecessor. The second half is not a separate fact - a predecessor in another
     * region could only reach a block that is its own region's `destination` or its child's `entry`,
     * and `isAnchor` has already declined both - but it is checked rather than argued, because the
     * cost is a comparison and the alternative is an argument that a later kind of region has to be
     * re-checked against.
     *
     * A target that is its own region's `entry` is declined outright: every predecessor here is
     * inside that region, so the redirect would be a re-entry, which is what
     * `Continuation::multiplicity` forbids of a cleanup arm.
     */
    bool spliceEmpty(Block& block) {
        if(block.argumentCount() || block.instructionCount() != 1) return false;
        if(!block.predecessorCount() || isAnchor(block)) return false;

        auto last = block.terminator(opt.base);
        if(!last || last->kind != Inst::Jump) return false;

        auto& jump = (InstJump&)*last;
        if(!jump.to.target) return false;

        auto target = opt.base[jump.to.target];
        if(target == &block || target->deleted) return false;
        if(target->argumentCount() || target->region != block.region) return false;
        if(isRegionEntry(*target)) return false;

        for(auto pointer: block.incoming(opt.base)) {
            if(opt.base[pointer]->region != block.region) return false;
        }

        /*
         * Every predecessor pointed at the target instead, taken from the front rather than walked.
         *
         * A redirect removes the entry it acted on - a predecessor whose two arms both land here is
         * two entries and leaves as two - so this reads the list it is emptying. What is tested is
         * the *arm*: a predecessor names this block from at least one of its edges by construction,
         * and stopping where it does not leaves a consistent body (what has moved reaches the target
         * directly, what has not still arrives here) where trusting the invariant leaves a compiler
         * that spins with nothing to say.
         */
        auto editor = opt.ir();
        auto moved = false;

        while(block.predecessorCount()) {
            auto from = opt.base[block.predecessorAt(opt.base, 0)];
            auto end = from->terminator(opt.base);
            if(!end) break;

            Size arm = 0;
            auto found = Size(~Size(0));
            forEachEdge(*end, [&](Edge& edge) {
                if(found == ~Size(0) && edge.target && opt.base[edge.target] == &block) found = arm;
                arm++;
            });

            if(found == ~Size(0)) break;

            editor.redirectEdge(*end, found, *target);

            // A predecessor whose other arm already reached the target now names it twice, which the
            // tier below refuses - so it goes back on the queue for `foldBranch`.
            push(from);
            moved = true;
        }

        if(!moved) return false;

        push(target);
        return true;
    }

    /*
     * The blocks nothing reaches, taken out together with the edges they owned into the ones that
     * survive - the file comment's linear-rule note is the argument that this is sound.
     *
     * Three phases, and the order is the whole of it. `Editor::deleteBlock` asserts a block has no
     * `incoming`, and a dead block is very often the predecessor of another dead block - a whole
     * stranded loop, say - so the edges have to go first. The terminators go first for a second
     * reason as well: an edge into a *live* block carries operands, and those uses have to stop
     * counting before the values they name can be erased.
     *
     * Then the instructions, to a fixed point rather than block by block, because a value defined in
     * one dead block is read in another and `eraseInst` refuses an instruction whose results still
     * have readers. A fixed point rather than a topological walk: the order exists - what is dead
     * now was ordinary SSA before the branch that stranded it - so a sweep that removes at least one
     * instruction per round terminates, and writing the order out would be reconstructing a dominance
     * relation that no longer has a meaning over these blocks.
     *
     * Nothing *live* can read a value defined here, which is what makes the whole phase local: a
     * definition has to dominate its readers, and an unreachable block dominates nothing reachable.
     * The one way a live block could name one is over an edge, and those went in the first phase.
     */
    bool removeUnreachable() {
        auto reachable = reachableOf(opt);

        SmallArray<Block*, 16> dead;
        for(auto pointer: opt.body->blocks.contents(opt.base)) {
            auto block = opt.base[pointer];
            if(block->deleted || reachable[block->index]) continue;

            dead.push(block);
        }

        if(!dead.size()) return false;

        auto editor = opt.ir();

        for(auto block: dead) {
            if(auto last = block->terminator(opt.base)) editor.eraseInst(*last);
        }

        for(auto progress = true; progress;) {
            progress = false;

            for(auto block: dead) {
                for(Size i = block->instructionCount(); i-- > 0;) {
                    auto inst = opt.base[block->instructionAt(opt.base, i)];

                    auto unread = true;
                    for(auto resultPointer: inst->results.contents(opt.base)) {
                        if(opt.base[resultPointer]->useCount()) unread = false;
                    }

                    if(!unread) continue;

                    editor.eraseInst(*inst);
                    progress = true;
                }
            }
        }

        for(auto block: dead) editor.deleteBlock(*block);
        return true;
    }

    void runQueue() {
        queue.clear();
        head = 0;
        for(Size i = 0; i < marked.size(); i++) marked[i] = 0;

        for(auto pointer: opt.body->blocks.contents(opt.base)) push(opt.base[pointer]);

        while(head < queue.size()) {
            auto index = queue[head++];
            mark(index, 0);

            auto block = opt.body->block(opt.base, BlockId(index));
            if(block->deleted) continue;

            if(foldBranch(*block)) continue;
            if(mergeInto(*block)) continue;
            spliceEmpty(*block);
        }
    }

    /*
     * The queue and the sweep, alternated until the sweep finds nothing.
     *
     * Two loops rather than one because they enable each other in one direction only: the queue
     * strands blocks, and the sweep - by removing a dead block's edges - leaves the live joins it
     * fed with one predecessor, which is a merge the queue had already declined. Each round of the
     * outer loop deletes at least one block, so it is bounded by the block count and in practice
     * runs twice.
     */
    void run() {
        for(;;) {
            runQueue();
            if(!removeUnreachable()) break;
        }
    }
};

/*
 * One join that a predecessor has already decided, and the edge sent straight past it.
 *
 * The shape is short-circuit boolean control used as a *value*: `let ok = (a && b) || c` converts to
 * a chain of arms that hand a `Bool` to a join, and the join branches on it. On the edge from the
 * arm that wrote `False` the answer is known where the edge leaves, so that predecessor can be
 * pointed at the branch's false arm and never reach the join at all.
 *
 * **A constant on the edge, and nothing else.** The shipping stage's `threadBooleanBranches` also
 * rewrites a predecessor that supplies a *computed* boolean - the jump becomes a copy of the join's
 * branch - and has a second shape for a predecessor whose own branch already proved the value. Both
 * are declined here and the reason is the same one: `Editor::makeBranch` builds **one** argument
 * list and pushes it onto both edges, which is this tier's rule that a value live before a branch is
 * live on both sides of it, so a branch cannot be given two arms of different shapes and a
 * predecessor ending in one cannot have a single arm redirected to a block declaring different
 * arguments. What is left is the constant case, which needs no branch at the predecessor at all.
 *
 * ## The three things that make a redirect legal, and they are the three rules of this tier
 *
 * **The arm's operands have to be computable where the edge leaves.** They are, and the reason is
 * dominance rather than a check: the join holds its branch and nothing else, so the only value
 * defined in it is its one block argument, and that argument's single reader is the condition. Every
 * other operand on an arm's edge is defined outside the join and therefore dominates it - and a
 * definition that dominates a block dominates every predecessor of that block, because a path to the
 * predecessor extends to the join and must pass through the definition. So "the arm's arguments are
 * available at the predecessor" is a consequence of the two shape conditions above rather than a
 * fourth one.
 *
 * **The region rule.** The predecessor, the join and both arms are in one `Continuation`, which is
 * `cleanCfg`'s rule in its fourth spelling; and no arm is its own region's `entry`, since every
 * redirected predecessor is inside that region and an edge from inside to the entry is the re-entry
 * `Continuation::multiplicity` forbids.
 *
 * **The linear rule, and this is the decision.** A join with a **tracked** block argument is not
 * threaded, and neither is one whose arms hand a tracked value over an edge. The first is the
 * blunter of the two and costs nothing: the argument here is the branch condition, and a `Bool` is
 * `Trivial`. The second is the real choice. Duplicating the handover site - the owner is consumed on
 * the predecessor's new edge and still consumed on the join's remaining ones - is sound exactly when
 * the two sites are on disjoint paths, which is a question about the paths through the join rather
 * than about the edge being moved, and there is nothing at this site that answers it. So it is
 * declined, and what that costs is measurable if it ever matters: the arms of a boolean join carry
 * the values the expression around it was in the middle of, and those are the operands of an
 * arithmetic or a comparison.
 *
 * The join's block argument is **not** removed when the last constant edge goes. Either some
 * predecessor still supplies it, in which case it is needed, or none does - and then the join is
 * unreachable, and **this pass deletes it rather than leaving it for the next round's sweep**.
 *
 * That last clause is phase 6's doing and it is worth stating, because the first version left the
 * stranded join standing on exactly the argument above. `verifyBody` checks an unreachable block
 * structurally and dominance is one of the things it checks, so a join left holding `branch %arg`
 * with no way in is reported - the block has no position in the order, so nothing dominates it,
 * including its own argument. It never arose while `forwardMemory` did not exist: a `let` put a
 * store and a load in the join, `thread` requires the join to hold its branch and nothing else, and
 * the joins that did qualify kept a predecessor. Forwarding the load is what makes the whole of an
 * `is` test's join threadable, and `Is.yana`'s `present` is where both of its predecessors go. A
 * pass owes the verifier IR it accepts at its own checkpoint, so the deletion belongs here.
 */
struct BranchThreader {
    OptContext& opt;

    bool isRegionEntry(Block& block) {
        auto region = opt.body->region(opt.base, block.region);
        return region && region->entry.valid() && region->entry == block.index;
    }

    // Whether an arm may be the new target: same region, not a re-entry, and handing over nothing
    // this pass is unwilling to duplicate the handover of - see the note above.
    bool armIsThreadable(Block& join, Edge& arm) {
        if(!arm.target) return false;

        auto target = opt.base[arm.target];
        if(target == &join || target->deleted) return false;
        if(target->region != join.region || isRegionEntry(*target)) return false;

        for(auto used: arm.arguments.contents(opt.base)) {
            if(!used.isSet()) return false;
            if(isTracked(opt.base[used.value()]->convention)) return false;
        }

        return true;
    }

    /*
     * The constant a predecessor hands the join, and nothing where this pass would not move that
     * predecessor at all - the one statement of "this edge is one that goes round", read twice: once
     * by the strand test below and once by the walk that performs the redirect.
     */
    Maybe<U64> movedConstant(Block& join, Block& from) {
        auto end = from.terminator(opt.base);
        if(!end || end->kind != Inst::Jump || from.region != join.region) return Nothing();

        auto supplied = ((InstJump&)*end).to.arguments.get(opt.base, 0);
        if(!supplied.isSet()) return Nothing();

        return constantValueOf(opt, opt.base[supplied.value()]);
    }

    // How many of a block's `incoming` entries name one predecessor - the edges the join's own
    // branch contributes, which go with it if the deletion below fires. One entry per edge, so a
    // branch whose two arms land on one block is counted twice.
    Size edgesFrom(Block& target, Block& from) {
        Size count = 0;
        for(auto pointer: target.incoming(opt.base)) {
            if(opt.base[pointer] == &from) count++;
        }

        return count;
    }

    /*
     * Whether threading this join would leave one of its arms with no way in - declined, rather
     * than followed by a sweep.
     *
     * It can only happen where **every** predecessor goes round, because that is the state the
     * deletion below fires in and the branch goes with the block: an arm no redirected edge selects
     * and nothing else enters is then unreachable, and an unreachable block is checked structurally
     * - nothing dominates it, so every operand it reads is reported at this pass's own checkpoint.
     * That was `let ok = if c then True else True` in front of an `if ok`, where both predecessors
     * hand over the same literal and the arm the other constant would have selected is reached from
     * nowhere else.
     *
     * Declined rather than swept, because what would have to go is the arm *and everything only it
     * reached*, which is `cleanCfg`'s `removeUnreachable` and not a second copy of it here - and
     * this pass runs behind that one rather than inside it.
     *
     * What the refusal costs is exactly the case it names: a diamond whose two sides wrote the same
     * answer, left as a diamond. Nothing else in this stage collapses one either - the condition is
     * a block argument, so `simplify` never sees a constant, and what would see it is the rule that
     * a block argument every edge supplies one number *is* that number. That rule is not here, and
     * it is the one worth adding if the shape ever measures: with it, the join's branch folds and
     * `cleanCfg` deletes the arm, which is the same answer this declines to reach the wrong way
     * round.
     */
    bool wouldStrand(Block& join, InstBranch& branch) {
        Size movable = 0;
        Size toTrue = 0;
        Size toFalse = 0;

        for(Size i = 0; i < join.predecessorCount(); i++) {
            auto constant = movedConstant(join, *opt.base[join.predecessorAt(opt.base, i)]);
            if(!constant) continue;

            movable++;
            if(constant.unwrap()) toTrue++; else toFalse++;
        }

        // A join that keeps a predecessor keeps its branch, and both arms keep the edge it owns.
        if(!movable || movable != join.predecessorCount()) return false;

        auto whenTrue = opt.base[branch.whenTrue.target];
        auto whenFalse = opt.base[branch.whenFalse.target];

        if(!toTrue && whenTrue->predecessorCount() == edgesFrom(*whenTrue, join)) return true;
        if(!toFalse && whenFalse->predecessorCount() == edgesFrom(*whenFalse, join)) return true;

        return false;
    }

    bool thread(Block& join) {
        if(join.argumentCount() != 1 || join.instructionCount() != 1) return false;

        auto last = join.terminator(opt.base);
        if(!last || last->kind != Inst::Branch) return false;

        auto& branch = (InstBranch&)*last;
        if(!branch.condition.isSet()) return false;

        // The condition is the block argument and its only reader. Both halves: an argument with a
        // second reader is one the join is still needed for, and a condition that is not the
        // argument is a join no incoming edge decides.
        auto argument = opt.base[join.argumentAt(opt.base, 0)];
        if(opt.base[branch.condition.value()] != argument) return false;
        if(argument->useCount() != 1 || isTracked(argument->convention)) return false;

        if(!armIsThreadable(join, branch.whenTrue)) return false;
        if(!armIsThreadable(join, branch.whenFalse)) return false;
        if(wouldStrand(join, branch)) return false;

        auto editor = opt.ir();
        auto threaded = false;

        /*
         * Every incoming edge that hands over a constant, sent to the arm that constant selects.
         *
         * The index is not advanced on a rewrite: `replaceTerminator` drops the predecessor's entry
         * from this list, so what was at `i` is now the next one to look at. A predecessor that
         * names the join twice is a `branch`, which is skipped for the reason above, so an entry
         * this walk passes over stays where it is.
         */
        for(Size i = 0; i < join.predecessorCount();) {
            auto from = opt.base[join.predecessorAt(opt.base, i)];

            auto constant = movedConstant(join, *from);
            if(!constant) { i++; continue; }

            auto end = from->terminator(opt.base);
            auto& arm = constant.unwrap() ? branch.whenTrue : branch.whenFalse;

            SmallArray<Use, 4> arguments;
            for(auto used: arm.arguments.contents(opt.base)) arguments.push(used);

            editor.replaceTerminator(*from, editor.makeJump(end->source, *opt.base[arm.target],
                                                            arguments.pointer(), arguments.size()));
            threaded = true;
        }

        /*
         * A join every predecessor has now gone round, taken out here - see the note above.
         *
         * `deleteBlock` is legal in exactly this state and needs nothing arranged: the incoming list
         * is empty because the loop above emptied it, the terminator is the block's only instruction
         * and is erased first, and the argument's only reader was that terminator. An anchor is not
         * excepted, and the sweep's argument is why: a region is entered at its `entry` and left to
         * its `destination`, so a block no live edge names is one no live region names either.
         */
        if(threaded && !join.predecessorCount() && &join != opt.base[opt.body->entry]) {
            editor.deleteBlock(join);
        }

        return threaded;
    }

    void run() {
        // Over a snapshot of the table, because a thread rewrites a *predecessor's* terminator and
        // the walk is over joins - a block visited later may have had its way out changed already,
        // which is the state `thread` reads rather than the one it was seeded with.
        SmallArray<Block*, 32> blocks;
        for(auto pointer: opt.body->blocks.contents(opt.base)) blocks.push(opt.base[pointer]);

        for(auto block: blocks) {
            if(block->deleted) continue;
            thread(*block);
        }
    }
};

} // namespace

void cleanCfg(OptContext& opt) {
    CfgCleaner cleaner { opt };
    cleaner.run();
}

void threadBranches(OptContext& opt) {
    BranchThreader threader { opt };
    threader.run();
}

} // namespace ssa
