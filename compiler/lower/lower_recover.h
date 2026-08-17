#pragma once

#include "lower_inst.h"

/*
 * A load one arm of a guard invalidated, recovered on the arm that did not.
 *
 * `push` is the standing shape and every growable container in the library is written with it:
 *
 *     if length + 1 > capacity: grow(self, length + 1)
 *     self[length] = value
 *
 * The bounds check under the store reads `capacity` a second time, and the guard above it read it
 * first - but between the two stands a call that may have written it, so the CSE retires the first
 * load and the second stands. That is the right answer on the path through the call and the wrong
 * one on the path around it, which is the path taken every time but the first: the value is
 * *partially* redundant, available on one edge into the join and not on the other.
 *
 * `Sieve.yana`'s fill loop is what it costs. Measured on the emitted loop with everything else held
 * identical, over 70M iterations: 33.0 ms as emitted, 29.7 ms with the second read gone.
 *
 * ## What it does, which is a move rather than a copy
 *
 * The obvious rewrite is to clone the load into the arm that clobbered it and merge the two with a
 * phi. This does the same thing from the other end and moves the *existing* load there instead, so
 * nothing is cloned and no instruction has to be reconstructed kind by kind:
 *
 *     guard:  %cap = load %self, 8      guard:  %cap = load %self, 8
 *             je %c, body, grow                 je %c, body, grow
 *     grow:   call grow(%self)   =>     grow:   call grow(%self)
 *             jmp body                          %cap2 = load %self, 8
 *     body:   %cap2 = load %self, 8             jmp body
 *             cmp %len, %cap2           body:   %p = phi [guard, %cap], [grow, %cap2]
 *                                               cmp %len, %p
 *
 * The load runs in exactly the cases it used to - the arm it moved to is the only way into the join
 * that needed it - and the arm that did not need it reads the value the guard already had.
 *
 * ## The shape, and why it is asked of the graph rather than of dominance
 *
 * Four conditions, and together they are one diamond with a single arm:
 *
 *  - the join has exactly two predecessors, and they are different blocks;
 *  - one of them, the *guard*, ends in a conditional branch whose two successors are the join and
 *    the other predecessor;
 *  - the other, the *arm*, has the guard as its only predecessor and reaches the join
 *    unconditionally.
 *
 * That is a stronger question than "which blocks dominate which" and it is asked because of what it
 * then makes free. Everything the join can read is defined in the guard or above it, so it is
 * defined above the arm as well - which is why an instruction may be moved from the join into the
 * arm without checking a single operand. And the guard's own instructions are available on the
 * unclobbered edge by construction, so the phi's other alternative needs no search beyond that
 * block.
 *
 * ## The chain, and the substitution that extends it
 *
 * One load is half the win. What reads it in the join is usually one more instruction that the guard
 * also computed - `and %cap, 0x3fffffff`, a packed capacity's payload - and after the first rewrite
 * its operand is the phi rather than the load, so an ordinary equality test no longer matches it
 * against the guard's copy.
 *
 * So the pass carries a substitution: every phi it inserts is recorded as standing for the guard's
 * alternative, and an operand is resolved through it before being compared. The `and` in the join
 * then reads "the guard's `%cap`" and matches, moves, and gets a phi of its own. The chain extends
 * as far as the guard computed it and stops where it did.
 *
 * ## Where it runs
 *
 * Directly behind `eliminateCommonValues`, which is what leaves the shape: the fully redundant loads
 * are already gone, so what is left in a join is either partially redundant or not redundant at all.
 * It adds phis and moves instructions between existing blocks; it creates and removes none, so the
 * loop structure and dominator tree built around it stay the ones it was handed.
 */
bool recoverPartialLoads(LowerBase base, LowerModule& module, LowerFunction& fun);
