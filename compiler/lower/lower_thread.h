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
