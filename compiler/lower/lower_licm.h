#pragma once

#include "lower_inst.h"

/*
 * What a loop recomputes because it was written inside it - §10 item 1 and §50.2 of
 * test/bench/findings.md.
 *
 * Two kinds of thing move, and they are the same rule asked of two different obligations.
 *
 * A read whose address does not change between iterations and whose answer nothing in the loop can
 * change is one read, done once. `hashOf` in test/bench/programs/Text.yana is the shape: the string's
 * base pointer is a field of the parameter, so every iteration of the scan loads it again before
 * indexing off it - `mov (%rdi),%r8` as one of the eight instructions in the corpus's worst `-Os`
 * row.
 *
 * A *computation* whose operands do not change between iterations is one computation, done once.
 * `Matrix.multiply` in test/bench/programs/Matrix.yana is that shape: `out[row * n + column]` and
 * `b[k * n + column]` recompute `row * n` and `k * n` in the innermost loop, where neither `row`, `k`
 * nor `n` changes. §50.2 hand-hoisted both in the source and measured **152.5 -> 150.7 ms, 1.2%** -
 * the one remaining difference between this backend's innermost loop and `-Os`'s that is not a
 * decision already measured and kept.
 *
 * ## Why here and not above the fork
 *
 * `hoistLoopValues` in compiler/opt is the same idea over the resolve IR, and it cannot reach either.
 * The read is `text.bytes.run.items`, whose place is rooted in a `borrow` of the parameter - and the
 * borrow is *re-taken inside the loop*, because that is what the resolver emits for each call that
 * reads through it. So the place's root is a value defined in the loop, the hoister's
 * `operandsOutside` refuses it before any aliasing question is asked, and hoisting the borrow itself
 * is a decision that stage declines to take (see opt_loop.cpp's header, and Analysis-Optimization.md
 * §7): a loan moved out of a loop covers iterations the borrow check never agreed to.
 *
 * `row * n` is not in the resolve IR as a multiplication at all on every target. It is `row * n` on a
 * native one, and on JS the same subscript is a place with an Index projection - the same split
 * lower_induction.h §1 gives for why the induction reduction is a lower pass. What is common to the
 * two shapes is arithmetic over lower values, and that is what exists down here.
 *
 * Down here the borrow has become an address, the address is `%text` itself, and the question is the
 * plain one - is this pointer the same every time round, and does anything in the loop write.
 *
 * ## What "does not change between iterations" is
 *
 * An operand defined outside the loop, which for a value in SSA form is simply where its instruction
 * sits. That is also, and without a second check, a value that is available in the preheader: the
 * preheader's only successor is the header, so anything it does not itself dominate is reached
 * through the header and cannot dominate the header in turn. A definition outside the loop that
 * reaches a use inside it therefore dominates the preheader, and an instruction moved there still has
 * every operand it had.
 *
 * An `Imm` is the exception and is admitted wherever it sits, because a constant has no operands and
 * moving it is free of every question above. It is *moved with* its reader rather than swept out
 * ahead of it: an immediate is usually folded into the encoding of whatever reads it, so relocating
 * one nothing hoisted would be a `mov` in a preheader in exchange for nothing.
 *
 * ## The two things that have to be true, for a load
 *
 * **Nothing in the loop writes storage.** Asked as a property of the whole loop rather than per
 * candidate, and answered by kind: a store, a block copy, a pattern fill, a call and an intrinsic all
 * decline it. That is far cruder than the structural aliasing `opt_place.cpp` performs, and
 * deliberately so - a place is an address by this point, so telling `%v_1 + 12` apart from
 * `(load %v_1) + %i` is pointer disambiguation over values rather than a question about fields, and
 * the escape information that would settle it lives a stage above. What is left is the loops that
 * only read, which is where the item's largest single row was.
 *
 * **The read cannot fault where it is being moved to.** The preheader runs when the loop is entered
 * and the body runs only if the test passes, so a load moved from one to the other happens on a path
 * it did not happen on before - and a `while i < n` with `n` zero is that path. Two answers are
 * accepted, and both are about the *address* rather than about the loop:
 *
 *  - it is an `alloca` and the whole access is inside it, which is storage the frame reserved;
 *  - the same base is already read or written, at an offset reaching at least as far, by an
 *    instruction that dominates the preheader. `hashOf` is this one: the entry block loads the
 *    string's length at offset 12, so the object is at least 16 bytes and the field at offset 0 is
 *    inside it.
 *
 * A dynamic offset is admitted by neither, which is what keeps every element access out of this: an
 * indexed address is not loop-invariant in the first place, and a checked one is only in bounds
 * behind the check.
 *
 * ## And the three that have to be true for a computation
 *
 * **It has to be a computation that can be moved.** `isRepeatable` is the list, and it is exactly the
 * right one for CSE and dead-value removal, both of which ask their question at a point the
 * instruction already runs at. This one moves it to a point it did not, so the four dividing
 * operations come out: the machine raises on a zero divisor and on `INT_MIN / -1`, and a division a
 * loop performs behind a guard on its divisor is a division the preheader would perform in front of
 * one. Nothing else in that list can fault - a shift count is masked, a float operation answers a
 * NaN, and a vector operation is lane-wise arithmetic.
 *
 * **It has to be a computation the loop actually performs.** An address, a comparison and a copy are
 * each free where they stand - a displacement in an addressing mode, a condition code, a coalesced
 * register - and hoisting one buys a live range and sells nothing. See `worthHoisting`, which is the
 * difference between this pass costing `Sort` 5.2 ms and costing it nothing.
 *
 * **And the loop it leaves has to be an innermost one.** Which is the profitability question, and the
 * answer to it is a rule rather than a model: what a hoist buys is one instruction per iteration of
 * the loop it leaves, and what it costs is a value live across every iteration of every loop *inside*
 * that one. Leaving an innermost loop is where the second term is empty and the first is largest.
 * `Matrix.multiply`'s two invariant multiplies are the demonstration of both halves - see
 * `isInnermost`. A value that ends up invariant one level further out still gets there, on the next
 * round, once the loop it landed in is itself the innermost one.
 *
 * ## Where it runs
 *
 * After `eliminateCommonValues`, so that a loop reading one address twice is shown one load rather
 * than two, and before `reduceInductionVariables`, which is where the induction pass expects the
 * loop's invariant parts to have stopped moving. That order matters more for the computations than
 * for the loads: the induction pass reads a stride as a literal and an address as `base + (i << k)`,
 * and an invariant multiply still sitting in the loop body is one more thing between the two.
 */
void hoistLoopInvariants(LowerBase base, LowerModule& module, LowerFunction& fun,
                         const LoopAnalysis& analysis);
