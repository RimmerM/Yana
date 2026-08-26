#pragma once

#include "lower_inst.h"

/*
 * Making integer division total, which is what the language promises and what neither native
 * machine provides.
 *
 * `x / 0` is 0, `x % 0` is `x`, and the overflowing signed pair wraps - the ruling is beside `Div`
 * in resolve/inst.def and the rule is in doc/spec/types.md. x86 raises #DE on both of those
 * divisors, and LLVM calls a division by zero undefined outright, so on both native backends the
 * defined answer has to be *built*. This is where it is built, once, for the reason
 * lower_strength.cpp is here rather than in either backend: two targets want the identical rewrite
 * and neither would produce it on its own.
 *
 * ## A correctness pass, not an optimization
 *
 * It runs in every build. There is no level at which a division may fault, because there is no level
 * at which the language says it may - `-no-checks` removes the *check*, not the answer. That is the
 * whole difference between this file and everything else in this directory, and the reason it sits
 * in the pipeline above the passes that read the loop structure rather than among them.
 *
 * ## The shape
 *
 * Four instructions and no branch, which matters: a branch would split the block, and every pass
 * below this one indexes by block. The divisor is replaced by one that cannot fault and the answer
 * is selected afterwards:
 *
 *     %zero   = cmp_eq %b, 0
 *     %safe   = select %zero, 1, %b        -- signed: select on `%b == 0 || %b == -1`
 *     %q      = idiv %x, %safe             -- 1 and -1 are gone, so this raises on nothing
 *     %result = select %zero, 0, %q        -- `%x` instead of 0 for a remainder
 *
 * Dividing by 1 is what makes the signed half fall out almost free. `x % 1` is 0, which is exactly
 * what `x % -1` has to be, so a signed remainder needs no second select at all; only a signed
 * *quotient* does, since `x / 1` is `x` where `x / -1` is `neg x` - and `neg` on the type's minimum
 * wraps to itself, which is the answer the overflow rule asks for.
 *
 * ## The zero arm, and the one case that does without it
 *
 * `divisorKnownNonZero` is the exception: where the chain of single-entry predecessors above a
 * division holds a branch that already tested this divisor against zero, the zero half is not built
 * at all. That is the shape a checked build puts around *every* division - the branch an inlined
 * `checkCondition` leaves - and the shape a careful program writes for itself:
 *
 *     if d != 0 then x / d else 0
 *
 * Seven instructions become four, and an *unsigned* division becomes none: it faults on nothing but
 * zero, so with zero excluded there is nothing to build and the instruction stands as it was.
 *
 * **This is the one thing here that is not a property of the operands.** Such a division cannot fault
 * where it stands and would above the test, so it carries `LowerInstBinary::kTrustsDivisorTest` and
 * `mayFault` in lower_inst.h refuses to hoist it. Getting that wrong is a hoist into a preheader
 * the test does not reach, which is a SIGFPE and not a wrong answer - so the bit is set in exactly
 * the two places the zero half is skipped, and read only through that predicate.
 *
 * The walk is four steps of immediate predecessors rather than a dominator query, because there is
 * no dominator tree built at this point in the pipeline and both shapes above put the proof in the
 * immediate predecessor. Anything deeper is the range analysis opt_range.cpp says it is not; the
 * resolve-IR half of this question is `foldProvenZeroTests` there, and it is what removes the
 * *check* that would otherwise ask what the guard above it just settled.
 *
 * ## What is left alone
 *
 * A divisor that is a non-zero literal, because it cannot fault and because the constant cases
 * belong to the two passes in front of this one - lower_strength.cpp rewrites `x / -1` as `neg x`
 * and lower_fold.cpp folds a literal pair outright, both of which they were only free to do once the
 * rule above existed. Floats, which divide to an IEEE infinity that needs nothing. And vectors: no
 * backend accepts a packed integer division today, so there is no instruction here to guard.
 */
void makeDivisionTotal(LowerBase base, LowerModule& module, LowerFunction& fun);
