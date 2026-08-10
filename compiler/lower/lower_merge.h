#pragma once

#include "lower_inst.h"

/*
 * The same exit, written down several times.
 *
 * Inlining is what produces these. A bounds check's failure arm is a syscall and an `Unreachable` -
 * three machine instructions with no operands anything else can tell apart - and a body with four
 * checked subscripts copied into its caller brings four identical copies of it along. `main` in
 * test/resolve/Adaptor.yana held seven; `resize(Int)` holds three blocks that are nothing but
 * `ret 0`, one per way its allocation can fail.
 *
 * Nothing above here can collapse them. The resolve tier has already run - `endNonReturningBlocks`
 * cut the call that made these arms *reachable* as one function, which is what turned them into
 * bare syscalls in the first place - and by the time the caller holds four copies they are ordinary
 * blocks of ordinary instructions. So the question is asked where the copies are: two blocks that
 * compute the same thing out of the same values and leave the function the same way are one block,
 * whatever their instructions happen to be named.
 *
 * ## And the copies that agree about everything but one value - §32
 *
 * The commoner shape, and the one that made this more than the abort-arm collapse it started as, is
 * a function with several early returns. Every one of them runs the same teardown - the drops and
 * reclaims the locals in scope owe - and then returns a *different* number:
 *
 *     b_26 { call reclaim, %v_16      b_34 { call reclaim, %v_16      b_50 { call reclaim, %v_16
 *            ret -3            }             ret -4            }             ret -6            }
 *
 * Three blocks, one of which is three quarters teardown. `test/resolve/FoldedAddress.yana`'s `main`
 * holds four and they are half its body; a function whose locals are a couple of arrays and whose
 * checks are a dozen holds a dozen copies of a teardown that is a dozen instructions.
 *
 * So the comparison is *modulo the operands the copies disagree on*, and the disagreement becomes a
 * phi in the block they collapse into. Which slot differs is not restricted - it is the returned
 * value in every case that motivated this, but a store of two different constants into the same
 * place merges by exactly the same rule, and nothing here needs to know which it is looking at.
 *
 * **What may be carried, and what may not.** A phi selects between values that already exist on the
 * edges reaching it, which is one requirement and one refusal:
 *
 *  - The two operands must be defined **outside** their own copies. A value the block computes for
 *    itself is one the merge is about to delete, and a phi over two deleted definitions selects
 *    nothing. Where both are outside, availability is free: an exit block is read by nothing, so a
 *    value it uses dominates it, and a definition that dominates a block and is not in it dominates
 *    every predecessor of that block.
 *  - **A call's first operand is refused**, because it is the function being called. Two copies
 *    calling different functions would merge into one *indirect* call, which is a worse program than
 *    the two it replaced rather than a smaller one. This is also what keeps a syscall number out: a
 *    syscall is a `Call` whose convention says so, and its number is that operand.
 *
 * **And it has to pay.** A phi is a copy per edge, so merging is not free the way the identical case
 * is: two blocks that are nothing but `ret 0` and `ret 1` become one `ret`, two jumps and a phi,
 * which is more code than it started with. `worthMerging` is the whole of the policy - the
 * instructions the merge removes against the copies the phis add - and it is what makes the four-way
 * teardown merge above worth taking and the pair of bare returns not.
 *
 * ## Why only a block control never leaves
 *
 * A block with no successors - one ending in `Ret` or `Unreachable` - is the case that needs no phi
 * surgery *downstream* and no dominance question, and both of those are the whole difference between
 * this and a general tail merge.
 *
 * It dominates nothing, so no value it defines can be read anywhere else, so unifying two of them
 * cannot leave a reader holding a definition that no longer runs. And it appears in no phi's source
 * list, so retargeting the edges into it is the whole of the rewrite: nothing downstream has an
 * opinion about which of the copies control arrived through. The phis this pass builds are in the
 * merged block itself, where the edges are the ones it just collected.
 *
 * The general case - two blocks that both `jmp X` - is deliberately left alone. It needs the phis in
 * `X` to agree about the two edges *and* one of the two source entries removed from each of them,
 * which is a rebuild rather than an edit (a phi's alternatives are allocated with it). What it would
 * buy is one jump per merged pair, against the three-to-six instructions a duplicated exit is.
 *
 * ## Where it runs
 *
 * Last, after every fold, every strength reduction and the CSE. Two copies of an exit are only
 * identical once whatever they compute has been folded to the same shape in both, and this removes
 * blocks rather than instructions - so nothing below it gains from running behind it.
 *
 * The x64 backend has a rewind of its own (§7.2.1, `genFunction`) that compares emitted *bytes* and
 * merges a return tail into an earlier one. The two do not overlap: that one is confined to blocks
 * ending in `Ret` under a shared epilogue and cannot touch an abort arm or a pair of copies that
 * differ, and it runs per target where this runs once for every backend that reads this IR.
 */
void mergeDuplicatedExits(LowerBase base, LowerModule& module, LowerFunction& fun);
