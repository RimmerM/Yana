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
 * quotient sequence twice, and this pass does nothing about it: the two are built independently, and
 * noticing that is a question about the finished IR rather than about either rewrite. That was the
 * first concrete thing named for a lower-IR CSE and it is now what collects it - `lower_cse.cpp` runs
 * immediately behind this pass, for exactly that reason.
 *
 * A signed division by -1 *is* rewritten, as `neg x`, and the note that used to stand here said why
 * it could not be: the machine raises on that pair whenever the dividend is the type's lowest value,
 * so a pass that rewrote it would be deciding what the answer is, silently. The language decides it
 * now - the quotient wraps back to the minimum, as all signed overflow does, and `neg` on the
 * minimum wraps to the minimum - so the rewrite states the rule instead of inventing one. See the
 * ruling beside `Div` in resolve/inst.def.
 *
 * A division by zero is still left alone here, for a reason that is not caution: lower_divide.cpp
 * guards it into a select over a constant and the fold behind this pass answers the whole thing, so
 * a rule here would be a second route to an answer that already arrives.
 */

// Run over a function after `foldFunctionConstants`, so that a divisor that only became a literal
// through promotion is one this can see. Emits `Imm`s of its own, which is why the fold and the dead
// constant sweep run again after it.
void strengthReduceFunction(LowerBase base, LowerModule& module, LowerFunction& fun);
