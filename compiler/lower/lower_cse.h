#pragma once

#include "lower_inst.h"

/*
 * Common subexpression elimination over the lower IR - §9.2 of test/bench/findings.md.
 *
 * One computation replaced by an earlier one that dominates it. Nothing here is arithmetic the source
 * wrote twice: it is arithmetic *lowering* wrote twice, which is why this pass is here and not in
 * `compiler/opt`.
 *
 * ## What is redundant at this altitude and not above it
 *
 * `out[row * n + column] = out[row * n + column] + left * b[k * n + column]` is one line of Yana, and
 * the two subscripts on the left of it are one index. Above the fork they already are: `row` and
 * `column` are places, and `eliminateCommonValues` in opt_value.cpp unifies two reads of one place
 * and then the `mul` and the `add` over them. What it cannot unify is what does not exist yet -
 *
 *  - **the address.** A place becomes `add base, offset` here, so `%out.length` read twice is two
 *    `add %frame, 12`s and the shared subexpression is one this IR invented.
 *  - **the promoted local.** `promoteStackSlots` runs immediately above this and turns a stack slot
 *    into a phi, so `row` stops being a load and starts being a value - which is what makes two
 *    `imul %row, %n` in two blocks *comparable* in the first place. Above the fork they were two
 *    loads, and opt_value.cpp deliberately declines to unify a whole scalar local because doing so
 *    costs JS a variable and buys the native target nothing that this pass does not buy it better.
 *  - **the reciprocal.** `strengthReduceFunction` turns `x / 7` into a five-instruction magic
 *    multiply and `x % 7` into that sequence plus two more, so a program asking for both at one
 *    divisor got the quotient twice. lower_strength.h names this as the first thing a lower-IR CSE
 *    would collect; it is collected by running this behind that pass.
 *
 * ## Two kinds it deliberately does not touch
 *
 * **An immediate.** Two `Imm`s holding one number are compared as *operands* - see `sameOperand`,
 * without which nearly nothing here matches, since `immediate()` in resolve/lower_type.cpp builds a
 * fresh one per site - but the instructions themselves are left alone. An immediate is not a value
 * the backend materializes if it can help it: `tryEmbedImm` marks one implicit when *every* one of
 * its readers can encode it inline, so giving one reader that can and one that cannot to a single
 * `Imm` takes the encoding away from both. The same argument retires `Fun`, whose address
 * `tryElideDirectCallee` elides on exactly the same all-or-nothing terms.
 *
 * **A load.** Not because it is unsound to ask, but because the answer here is nearly always no: a
 * place is an address by this point, so "do these two touch the same storage" is pointer
 * disambiguation over `add ptr, imm` rather than the structural question opt_place.cpp answers. The
 * redundant loads §9.2 names - an array's `length` and `items`, read once per bounds check - are
 * removed above the fork, where the projection path is still there to compare. See
 * `eliminateCommonValues` in opt_value.cpp.
 *
 * ## Where it runs
 *
 * After `promoteStackSlots` and the strength reduction, and before `reduceInductionVariables`, which
 * is the "before the loop passes" §9.2 asks for: what the induction pass rewrites is a multiply it
 * has proved is a recurrence, and it should be shown one of those rather than three.
 *
 * ## What it is worth, and where the work went
 *
 * `multiply` in test/bench/programs/Matrix.yana goes from **37 instructions to 29** in its innermost
 * loop, and `row * n` leaves it entirely; that program times **1.052x**. Over the ten programs the
 * corpus is, and the 150 `test/resolve` fixtures that build to executables, it is −2036 bytes.
 *
 * Three quarters of the code below is the decision *not* to unify something, and each of the three
 * rules was a measured regression before it was a rule - `answerableAcrossBlocks`, `costsARegister`
 * and `answerableFrom`, whose reasons are with them. They have one shape in common, and it is the one
 * §9.1 of findings.md states about promotion: recomputing a value costs an instruction, and keeping
 * one alive costs a register, and the second is the larger number more often than it looks.
 */
/*
 * Handed the loop structure and the dominator tree rather than building them - see LoopAnalysis in
 * lower.h, which is the run of five passes this opens.
 *
 * Answers whether it changed the block graph, which is what `takeDecidedArm` does when it proves a
 * bounds check redundant: an edge goes, and with it the abort arm nothing reaches any more. That
 * makes it the one pass in the run that leaves the pair it was given stale, and the answer is what
 * tells the caller to rebuild before handing the same pair to the four behind it.
 */
bool eliminateCommonValues(LowerBase base, LowerModule& module, LowerFunction& fun,
                           const LoopAnalysis& analysis);

/*
 * The counters a loop carries two of, merged into one.
 *
 * The pass above answers a computation from an earlier one that dominates it, and a loop's counter is
 * the one thing it can never reach: the value a header phi holds is not computed anywhere that
 * dominates the header, it is *chosen* there, and two phis choosing the same thing are two
 * computations neither of which came first. So a loop advancing two values in step keeps both, one
 * add each, one register each, for the whole of the loop.
 *
 * `indexOfVectors` in Core - compiler/resolve/core.cpp - is the shape, and it is not an accident of
 * that function. The iteration protocol hands the body a chunk and the offset it came from, and the
 * body keeps a running position of its own; at a byte lane on AVX2 both advance by 32, so the vector
 * search's inner loop is
 *
 *     %at  = phi [pre, 0], [latch, %at2]        add %eax, %r8d
 *     %off = phi [pre, 0], [latch, %off2]       add %edx, %r8d
 *     ...                                       mov %edx, %r9d
 *     %at2  = add %at,  32                      add %r8d, %r9d
 *     %off2 = add %off, 32                      cmp %ecx, %r9d
 *
 * two recurrences with one value between them. Merging them is one add and one register out of the
 * hot loop, and the survivor serves both the addressing and the position the search returns.
 *
 * ## What makes two of them the same
 *
 * They choose R-equal values on every incoming edge, where R is equality plus the assumption that
 * these two phis are themselves equal. That assumption is what the question needs to be answerable at
 * all - `%at2` and `%off2` agree exactly when `%at` and `%off` do - and discharging it is sound for
 * the reason any greatest-fixpoint argument is: the entry edge of a loop names values that are not
 * the phis, so the induction has a base case, and every later iteration follows from the one before.
 * See `Congruence`.
 *
 * Structural rather than syntactic, because the two steps are two instructions in two blocks: the
 * comparison is `sameOperand`, the same one the CSE uses, which is what makes `add %at, 32` in one
 * block and `add %off, 32` in another one computation. It descends through arithmetic only and never
 * through a load - two loads of one address are equal only where nothing wrote between them, which is
 * a fact about a path rather than about the pair.
 *
 * The alternatives are matched by source block rather than by position, and a predecessor named twice
 * is declined outright.
 *
 * ## Where it runs
 *
 * In front of `eliminateCommonValues`, which is the whole reason it is a separate pass rather than a
 * step inside one: what the merge leaves behind is one value where there were two, and everything
 * computed from either of them - a sign extension, an address, a comparison - becomes a pair the CSE
 * can then collect. Behind `threadDecidedBranches` and the folds, so that a phi left with one
 * alternative is already that alternative rather than a join to compare.
 */
void mergeCongruentPhis(LowerBase base, LowerModule& module, LowerFunction& fun);
