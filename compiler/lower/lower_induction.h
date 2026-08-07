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
void reduceInductionVariables(LowerBase base, LowerModule& module, LowerFunction& fun);
