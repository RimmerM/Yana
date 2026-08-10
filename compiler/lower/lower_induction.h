#pragma once

#include "lower_inst.h"

/*
 * Strength reduction of an address driven by an induction variable.
 *
 * A loop that walks an array recomputes the address of the element it is on from the index it is on:
 * `base + i*stride`, from scratch, every iteration. What it could do instead is carry the address
 * itself and add the stride to it, which is one instruction where the recomputation is two or more -
 * and none of them on the critical path of the load, since the pointer for the next iteration is
 * built while this one's is being used.
 *
 *     head:                            head:
 *       %i = phi [pre, 0], [body, %i2]   %i = phi [pre, 0], [body, %i2]
 *       ...                              %p = phi [pre, %base], [body, %p2]
 *     body:                            body:
 *       %o = shl %i, 5                   %v = load %p
 *       %q = add %base, %o               %p2 = add %p, 32
 *       %v = load %q                     %i2 = add %i, 1
 *       %i2 = add %i, 1
 *
 * This is `test/bench/findings.md` item 6, and it is the item that file ranks third by value and
 * first by cost. The counter is left exactly where it was: replacing the loop's own test with one
 * against the pointer (linear function test replacement) is a second transform, and `llc -O3` does
 * not perform it on this corpus either - its `sumFields` carries both.
 *
 * ## Why this is a lower-IR pass and not a resolve-IR one
 *
 * The question was live, and more so than it was for `lower_strength.h`: the arithmetic this rewrites
 * *does* exist in the resolve IR. `xs[i]` on a native target resolves - through the `Index` instance,
 * inlined - to `mul %i, strideof T` and `add %items, ...`, and `opt_loop.cpp` already runs a
 * dominator walk and a loop finder over that IR. Three things say it should still not go there:
 *
 *  1. **On JS the shape does not exist at all.** `@platform` selects a different `Index` instance
 *     there, and the same source resolves to `borrow [%items][%index]` - a place with an Index
 *     projection, no multiply and no pointer add anywhere. So a resolve-tier pass would not be
 *     "written once, serving every target"; it would be dead code on one of the three by
 *     construction, and the only linear expression it *could* find there - a host array's
 *     `offset + i` - is one adding a second variable to would make worse rather than better.
 *
 *  2. **Whether the rewrite pays depends on the stride, and in the resolve IR the stride is not a
 *     number.** `strideof T` is a `TypeMetric`, answered by a `ReprTable` - and an address whose
 *     scale is 1, 2, 4 or 8 is *free*, because the SIB byte holds it and `foldAddresses` already
 *     puts it there (findings item 4). Reducing one of those trades an addressing mode that costs
 *     nothing for a live pointer register and an add per iteration, which is a pessimization. So the
 *     pass has to read the stride as a literal, and a `compiler/opt` transform that reads a repr
 *     answer to decide what IR to produce is exactly what `Implementation-Module-Format.md`'s
 *     gate/refinement/transform rule disqualifies from the target-independent tier.
 *
 *  3. **Of the two native backends only one needs it**, which is the same reason `lower_fold.h` and
 *     `lower_strength.h` give: LLVM runs its own loop-strength-reduction pass at `-opt >= 1`. The
 *     form handed to it here is the form it produces itself for these strides - findings §3 quotes
 *     `llc`'s `sumFields` advancing a row pointer by `0x20` - so the shared-lower placement costs it
 *     nothing, and the scale gate below is what keeps the two from disagreeing: for a stride the SIB
 *     byte can encode, neither backend wants this and neither is given it.
 *
 * ## What one has to look like
 *
 * A loop with one preheader and one latch, a phi `%i` in its header advanced by a constant on the
 * latch edge, and an address `add %base, (%i << k)` whose base is loop-invariant.
 *
 * `%i` may be narrower than the address unit, in which case it reaches the shift through a `sext`
 * and the recurrence is that widening rather than the counter. That is only an induction variable
 * where the narrow addition cannot wrap, since `sext(i + 1) == sext(i) + 1` is what the whole
 * rewrite rests on - and the counter's *own* type is no help: `doc/spec/types.md` says integer
 * overflow wraps. What decides it is the loop form. `doc/spec/expressions.md` guarantees that a
 * `for` range counter does not overflow on the way out, and the guard implementing that guarantee -
 * the counter's distance from the far end tested against the stride - is emitted control flow that
 * survives to here, so it is checked rather than believed. See `stepCannotOverflow`.
 *
 * A `while` loop with a counter the program advances itself has no such guard and is declined. So
 * is the fact that would otherwise bound it: a *stored* count is a `@bits(30) U32`, but the length a
 * loop compares against belongs to the borrow, and `Flat.length` is a full `Size` on purpose - see
 * the note above `Count` in resolve/native.cpp for why a borrow's length must not be the limiting
 * width in the language.
 *
 * Everything reading the address has to be inside the loop. The pointer phi holds, after the loop,
 * whatever the last *header entry* put there rather than what the last iteration computed, and those
 * differ whenever the address is built under a branch - so a reader outside is declined rather than
 * reasoned about.
 *
 * Addresses sharing a base and a scale share one pointer, which is what makes a walk over a record's
 * fields cost one add rather than one per field: `xs[i].a` and `xs[i].d` are two `add %items, %o`
 * with the same operands, and both become the same `%p` with the field offsets left as the
 * displacements they already were.
 */

// Run over a function after `strengthReduceFunction` and the fold behind it, so that `mul %i, 32` is
// already `shl %i, 5` and a stride that came from a `strideof` is already a literal. Emits an `Imm`
// of its own for the step, so the fold and the dead constant sweep run again after it.
void reduceInductionVariables(LowerBase base, LowerModule& module, LowerFunction& fun,
                              const LoopAnalysis& analysis);

/*
 * A narrow loop counter, carried at the width its addresses are computed in - §14.5 item 1 of
 * test/bench/findings.md, and the byte loop in `hashOf` that item is written about.
 *
 * An index that is `Int` and an address that is not means a sign extension per subscript, inside the
 * loop, on the critical path of the load. Where the counter provably does not wrap - the same proof
 * the reduction above needs, and for the same reason - that widening can move to the counter itself:
 * the phi, its step and the loop's own test go up a width, and every extension in the body stops
 * existing. Nothing else in the loop changes, so an address that was `base + sext(i)` becomes
 * `base + i` and still folds whole into the access that reads it.
 *
 * The item costed this as a pointer induction variable plus a down-counter, and observed that
 * neither pays alone: reducing a scale the SIB byte already holds trades the extension for an add,
 * and the add only goes away once the index has stopped being the loop's counter as well. Widening
 * is a third answer that needs neither, which is why `isEncodableScale` is unchanged.
 *
 * Behind `reduceInductionVariables`, so that an extension that pass is about to delete is not one
 * this widens a counter for. See `widenLoopCounters`.
 */
void widenInductionVariables(LowerBase base, LowerModule& module, LowerFunction& fun,
                             const LoopAnalysis& analysis);

/*
 * §28 A bounds check the loop's own test has already made.
 *
 * `for x in each(xs)` compiles to a counted loop over `0 ..< length(xs)` with a subscript inside it,
 * and the subscript carries a check of its own - so the hot loop tests the same index against the
 * same length twice, once to decide whether to run the body and once to decide whether to abort:
 *
 *     head:  %i = phi [pre, 0], [latch, %i2]     head:  %i = phi [pre, 0], [latch, %i2]
 *            %c = cmp_ilt %i, %n                        %c = cmp_ilt %i, %n
 *            je %c, body, exit                          je %c, body, exit
 *     body:  %d = cmp_ge %i, %len            ->   body:  %v = load %items + %i*4
 *            je %d, abort, load                          ..
 *     load:  %v = load %items + %i*4
 *
 * What makes this a *proof* rather than the unswitch item 5 of the seventeenth list described is
 * that the two tests read the same length. Duplicating the loop under a guard would cost a copy of
 * the body; establishing that the check cannot fail costs nothing and removes a branch, an
 * abort block and - once nothing else reaches it - the exit sequence at the end of it.
 *
 * Three things have to hold, and each is checked rather than assumed:
 *
 *  - **the index is the loop's own counter**, a header phi starting at a non-negative constant and
 *    advanced by a positive constant on the latch edge, whose addition provably does not wrap. That
 *    last is `stepCannotOverflow`, unchanged and shared with the two passes above: without it the
 *    counter is a sequence that usually ascends rather than one that does, and "it started at zero
 *    and only goes up" says nothing.
 *  - **the header's test bounds it**, `%i <s %B` or `%i <=s %B` on the arm that stays in the loop,
 *    with the other arm leaving. So every block the header dominates inside the loop runs with
 *    `0 <= %i` and `%i <= %B`, and the counter is a header phi, so nothing between the two changed
 *    it.
 *  - **the loop's bound is within the length**, which is `%B <=u %L`. Two shapes answer it. The
 *    same value is the easy one. The other is `%B = sext(trunc(%L))`, which is what `length(xs) ::
 *    Int` is: for an unsigned `%L`, `%L mod 2^32 <=u %L` always, and the sign extension either
 *    reproduces that or produces something negative - in which case the header test is false and
 *    the body never runs.
 *
 * The check is then a branch whose abort arm cannot be taken, and the rewrite is to stop naming it.
 * Only an arm ending in `Unreachable` is removed this way: that is what a check's abort arm is (see
 * §11 of test/bench/findings.md), it has no successors of its own, so dropping it is local, and it
 * is the one shape where being wrong is a program that would have stopped rather than a value.
 *
 * Behind `widenInductionVariables`, and that is load-bearing rather than tidy: before it the counter
 * is an `Int` phi and the check compares `sext(%i)` against a `Size`, which is two more steps of
 * reasoning for the same conclusion. After it the phi is already the width the check reads.
 */
void eliminateBoundedChecks(LowerBase base, LowerModule& module, LowerFunction& fun,
                            const LoopAnalysis& analysis);
