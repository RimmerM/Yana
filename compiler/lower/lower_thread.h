#pragma once

#include "lower_inst.h"

/*
 * A branch whose answer the edge into it already settled.
 *
 * `indexOfVectors` in test/bench/programs/VecString.yana is the standing demonstration and the item
 * this was written for. A `return` inside a `for` inside an `iter fn` is four nested `Outcome`s -
 * one per loop the signal has to climb out of - and by the time `splitAggregateSlots` and
 * `promoteStackSlots` have taken those records apart, each level is the same four lines:
 *
 *     b_63 { %tag = phi [b_37, 0], [b_60, 1], [b_80, 1]
 *            %at  = phi [b_37, 0], [b_60, %v_56], [b_80, %v_76]
 *            %c = cmp_eq %tag, 1 ; je %c, b_87, b_88 }
 *
 * Every alternative of the tag is a literal, so *every* edge into the block already knows which way
 * the branch goes - and nothing below the front end was reading that. What it cost is not only the
 * four `cmp $1 ; je` on the way out: the no-match arm of the search loop paid three instructions per
 * iteration writing zeroes into a payload that only the match arm ever reads, because the phi it
 * merges into has to hold *something* on that edge.
 *
 * So the edge is pointed at the successor it had already chosen, and the block in the middle is left
 * with whatever predecessors could not answer - usually none, and then it goes too.
 *
 * ## What it takes to skip a block, and the four preconditions that are the whole of it
 *
 * Threading is edge surgery on a graph whose values are in SSA form, so the hard half is not the
 * edge - it is that a value defined in the block being skipped may be *read below it*, and control
 * arriving at the reader without passing the definition is a program that no longer type-checks. The
 * repair is standard and is the reason this is a pass rather than a rule in `foldFunctionConstants`:
 * a phi is inserted at the join the threaded edge now reaches, and every read the join dominates is
 * pointed at that phi instead.
 *
 * Four things are required of the block in the middle, and each one removes a whole class of repair
 * rather than a case:
 *
 *  - **Nothing in it but phis, one compare and the branch.** An edge that skips the block has to skip
 *    nothing that happens, and the compare is admitted only because it is the branch's own condition
 *    and nothing else reads it. This is what makes the question a shape question.
 *  - **Both successors are reached only from it.** That is what makes the block dominate them, which
 *    is what makes "the reads to repair" exactly "the reads the successor dominates". Without it the
 *    successor may be reachable another way, and a phi inserted there would be answering for edges
 *    that never held the value at all.
 *  - **Neither successor has phis of its own.** A phi's alternatives are allocated with it, so
 *    extending one is a rebuild, and the value each new edge would have to supply is a value defined
 *    in the block being skipped. A successor with one predecessor has no phi worth having anyway -
 *    `removeTrivialPhis` ran during promotion and took them.
 *  - **Every read of every phi is inside the block or under one of the two successors**, a read on a
 *    phi edge being judged where that edge leaves. A read at a join *below* both arms is the one the
 *    repair above cannot reach: it is dominated by the block and by neither successor, so there is no
 *    single place to put the phi that answers it. That is real SSA reconstruction, it is what this
 *    deliberately stops short of, and declining costs nothing the shape above ever wanted.
 *
 * ## Iterated, because one level exposes the next
 *
 * Threading `b_63` makes its successors' join a block whose own tag phi now has literal alternatives,
 * which is this same shape one level out. The four levels of `indexOfVectors` come apart one round
 * at a time, so the pass runs to a fixed point over the function.
 *
 * ## Where it runs
 *
 * After `promoteStackSlots`, which is what turns the records into the phis this reads, and after the
 * folds, so that an alternative that is a literal is written as one. Before `LoopAnalysis` is built,
 * because it changes the block set and every pass below that point indexes by block.
 *
 * A fold runs behind it: a phi left with one alternative is that alternative, and the comparison the
 * next level branches on folds against it.
 */
void threadDecidedBranches(LowerBase base, LowerModule& module, LowerFunction& fun);

/*
 * The constant-condition half of the pass above, on its own, for a caller that runs below it.
 *
 * The fold inside `threadDecidedBranches` only sees the branches that were decided by the time it
 * ran, and the passes behind it decide more - see the note on the definition. Answers whether the
 * block set changed.
 */
bool foldDecidedBranches(LowerBase base, LowerModule& module, LowerFunction& fun);

/*
 * A branch whose answer a test on the way into it already settled, with the edge moved past it.
 *
 * `threadDecidedBranches` reads a literal a phi carries; this reads a comparison a branch above made.
 * The shape is a chain of tests of one value, which is what a guard with tuple patterns becomes:
 * `| {0xE0, _..0xA0} -> 0 | {0xED, 0xA0.._} -> 0 | {0xE0..=0xEF, _} -> 2` tests the lead against
 * `0xE0`, then the second byte, and where the second byte does not match it falls into the next arm's
 * test of the lead - which that edge already answered. `sequenceReach` in lib/Core/Char.native.yana is the
 * function this was written for: without the pass every fall re-tested the lead down the rest of the
 * chain, and the layout put the rare second-byte tests between the common lead tests.
 *
 * A test is read as the interval of values its true arm admits, modulo the width - an equality, a
 * relation, or a relation of the value moved by a literal, which is what a range pattern folds to -
 * and an edge knows the interval of its own arm or the complement of it. The block is skipped when
 * that interval lies inside the test's or outside it.
 *
 * What may be skipped is the same promise as the pass above, stated without phis: nothing in the
 * block but pure arithmetic only the branch reads. An arm with phis of its own is declined, since the
 * front ends never branch straight into a join, and a loop header is never skipped, since the edge
 * would enter the loop below it.
 *
 * The facts are read from the predecessor's own branch and from the branches above it, climbing only
 * through blocks with one way in. No block is copied, so the program never grows.
 *
 * Behind `threadDecidedBranches`, and in front of the loop analysis for the same reason: it changes
 * the block set. Answers whether anything moved.
 */
bool threadImpliedBranches(LowerBase base, LowerModule& module, LowerFunction& fun);

/*
 * A block that ends in a jump to a block nothing else enters, and the two made one.
 *
 * Nothing above builds this shape on purpose; it is what a removed branch leaves behind. A bounds
 * check a dominating one already decided becomes a `jmp` into the arm it always took
 * (`takeDecidedArm` in lower_cse.cpp), and so does one the loop's own test made
 * (`eliminateBoundedChecks`) - and neither of those rewrites is in a position to splice, since each
 * runs under an analysis indexed by the block set. So the pair is left for here.
 *
 * **Why it is worth a pass: the backend's folds are per block.** `foldStoreUpdates` takes a load,
 * an operation on it and the store of the result only when the operation and the store share a
 * block, and the resolver checks `out[i]` once for the read and once for the write in
 * `out[i] = out[i] + x`. The second check is decided, and what is left is exactly
 * `b1 { ...; %v = add %l, %x; jmp b2 }  b2 { store %p, %v }`: the fold fired under
 * `compiler/resolve` (the stage deleted in J173), which checked the place once, and not under this
 * one - on the same program, with otherwise the same IR (§113 of test/bench/findings.md).
 *
 * The successor's phis have one alternative each and are replaced by it; the terminator, and with it
 * every edge the successor had, moves up. A successor that jumps back to the block is left alone,
 * because the merge would make that a jump from the block to itself.
 *
 * Last in the run, behind every pass that removes a branch, and answers whether anything merged.
 */
bool mergeLinearBlocks(LowerBase base, LowerFunction& fun);
