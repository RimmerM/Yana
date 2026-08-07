#pragma once

#include "lower_inst.h"

/*
 * Strength reduction of a multiply, a division or a remainder by a constant.
 *
 * Every operation here is replaced by one that answers the same number and costs less to compute:
 * `x * 8` by a shift, `x / 8` by a shift, `x % 8` by a mask, and `x / 10` by the reciprocal
 * multiplication a divider is spelled as when there is no divider worth using. Nothing here is
 * arithmetic the source could not have written itself - what it is, is the arithmetic nobody writes
 * because the compiler is supposed to.
 *
 * ## Why this is a lower-IR pass and not a resolve-IR one
 *
 * The question was live, because `compiler/opt` already folds constants over the resolve IR and
 * already knows every operand's width and signedness, so it *could* host this. Three things say it
 * should not, and they are the three that separate a fold from a strength reduction:
 *
 *  1. **A fold states what a value is; this states which of two spellings is cheaper.** The rule
 *     `opt_fold.cpp` is written to - "where the targets could disagree about the operation itself,
 *     it is not folded" - is about the *answer*, and every rewrite here preserves that exactly. What
 *     the targets disagree about is the *cost*, and this pass has to pick a side.
 *
 *  2. **The side it would have to pick is wrong for JS.** A 33-to-53-bit division is an inline
 *     `Math.trunc(a / b)` there and a shift of one is a `$w53i$shr` *call* (`codegen/js/wide.cpp`),
 *     so the shift rewrite turns an inline operation into a call; a `Long` is a BigInt, where a
 *     reciprocal multiplication is arbitrary-precision work in place of one division. So the
 *     "written once, serves both targets" argument that put constant folding above the fork points
 *     the other way here: what JS wants is a different rewrite, decided against `codegen/js`'s own
 *     cost model, not this one moved up.
 *
 *  3. **The reciprocal needs an instruction only a machine has.** `MulHi` is the top half of a
 *     double-width product - the thing every multiplier computes and almost every language throws
 *     away. Putting it in the resolve IR would give the language's own instruction set an operation
 *     that exists because x86 has it.
 *
 * Which leaves the lower IR, where it is shared by both native backends for the same reason
 * `lower_fold.cpp` is: LLVM would do all of this itself, and the x64 backend has nothing that
 * would. Handing LLVM the reduced form costs nothing - the power-of-two rewrites are its own
 * canonical form, and `mulhi` is the shape its DAG combiner already matches.
 *
 * ## What is declined, and why
 *
 * A program that asks for both `x / d` and `x % d` at the same non-power-of-two `d` gets the
 * quotient sequence twice, because the only CSE this compiler has runs over the resolve IR and this
 * is below it. That is not a regression - it was two divisions before, and it is two five-instruction
 * sequences now - but it is the first concrete thing the lower-IR CSE the x64 README's roadmap ends
 * on would collect.
 *
 * A division by zero and a signed division by -1 are both left exactly as they were. The machine is
 * entitled to trap on either - `idiv` raises #DE for the second whenever the dividend is the type's
 * lowest value - and `lower_fold.cpp` already declines to fold the same two pairs for the same
 * reason. A pass that turned `x / -1` into `neg x` would be deciding that question rather than
 * leaving it, and it would be deciding it silently.
 */

// Run over a function after `foldFunctionConstants`, so that a divisor that only became a literal
// through promotion is one this can see. Emits `Imm`s of its own, which is why the fold and the dead
// constant sweep run again after it.
void strengthReduceFunction(LowerBase base, LowerModule& module, LowerFunction& fun);
