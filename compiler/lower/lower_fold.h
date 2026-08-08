#pragma once

#include "lower_inst.h"

/*
 * Constant folding at the point an instruction is built.
 *
 * The resolve-to-lower translation is where a `@bits` field, a niche tag and a narrow reference stop
 * being descriptions and become masking, shifting and merging - and at a construction site every one
 * of those operands is a literal. `Pair {a: 1000, b: 999999}` lowered to sixteen instructions that
 * computed one number, and nothing downstream took them back: the native path leaves it to LLVM,
 * which does fold it but only after the IR has been written out and read, and the x64 backend has no
 * such pass at all and emitted every one of them.
 *
 * So it is done where the instruction is made. That is the one place with no ordering question in it
 * - an operand is already built when its consumer is, so a chain folds from the bottom up in a single
 * pass - and it is the place every producer of lower IR goes through, which is what makes this a
 * property of the IR rather than of the six files in `resolve/` that happen to emit bit arithmetic.
 *
 * **Everything here is stated at the type's width**, which is the whole of what makes it safe: an
 * `Int32` operand is read as its low 32 bits whatever the `LowerImm` happens to hold above them, and
 * a result is written back the same way. The alternative - folding in 64 bits and trusting the
 * backend to truncate - is right for `and` and wrong for `shr`, `sar` and every division.
 *
 * Two shapes are declined rather than folded, and both for the same reason: the answer would have to
 * agree with whatever the target does, and neither target says. A shift by a distance at or above the
 * type's width is masked by x86 and poison in LLVM; a division by zero traps. Leaving those alone
 * means the folded and unfolded programs cannot disagree about them.
 */

/*
 * The three folds, each answering with the instruction whose result stands in for the one that was
 * about to be built, or null to build it after all.
 *
 * The answer is an instruction rather than a value because that is what the builders return and what
 * every caller of them reads `created().ptr` off. Which is also why a fold that answers with an
 * operand it was given only does so when that operand's instruction produces exactly one value:
 * forwarding the second result of a call would hand the caller the first.
 */
LowerInst* foldUnaryArith(LowerBase base, LowerModule& module, LowerBlock& block,
                          LowerInst::Kind kind, LowerValue* arg, LowerType type, StringId name);

LowerInst* foldCast(LowerBase base, LowerModule& module, LowerBlock& block, LowerValue* arg,
                    LowerType type, bool signedSource, StringId name);

LowerInst* foldBinary(LowerBase base, LowerModule& module, LowerBlock& block, LowerInst::Kind kind,
                      LowerValue* lhs, LowerValue* rhs, LowerType type, StringId name);

/*
 * The same three rules over IR that is already built, which is not the same work twice.
 *
 * A builder can only see what an operand was when the instruction was made, and promotion changes
 * that answer: a local written once and read once is an `alloca`, a `store` of a constant and a
 * `load`, so every operation reading it was built against a load and becomes an operation on a
 * literal only once the slot is in a register. `Pair {a: 1000, b: 999999}` is entirely this shape -
 * the word is assembled out of a load of the storage it is about to become.
 *
 * Which is why this runs *after* promotion rather than instead of the builder's fold. The builder's
 * is what keeps the chains from being created at all, and this is what folds the ones that only
 * became constant later. Iterated, since folding one operation is what makes the next one's operand
 * a literal.
 */
void foldFunctionConstants(LowerBase base, LowerModule& module, LowerFunction& fun);

/*
 * Whether an instruction computes a value out of its operands and does nothing else.
 *
 * Two passes ask it and they ask it for two reasons that happen to have one answer: something that
 * only computes may be answered from an earlier one that dominates it, and it may be dropped when
 * nothing reads it. Written out rather than derived from a range, because the ranges in lower_inst.h
 * group instructions by *shape* - `isBinary` includes the comparison and `FirstUnary` starts at
 * `Set` - and this is a different question.
 *
 * The four dividing operations are in it, and that is worth stating because they can trap. The
 * machine raises on a zero divisor and on `INT_MIN / -1`; neither reader of this list is made unsound
 * by that. Removing the *second* of two identical divisions removes no trap, since the first
 * dominates it and the fault has already happened - and a division nothing reads at all is one this
 * compiler never emits, because a division is only ever written down for its answer.
 */
bool isRepeatable(LowerInst* inst);

/*
 * The computations nothing reads any more, dropped. Answers whether it dropped anything.
 *
 * Two passes leave these behind. A replacement moves the readers of one value onto another, which can
 * leave the *operands* of what it replaced with nothing reading them: `%a = add x, y` feeding only a
 * `%b = mul %a, z` that has just been answered from an earlier one is dead the moment `%b` goes. And
 * the comparison narrowing in `foldFunctionConstants` takes a reader off an addition without removing
 * the addition, which is the same situation arrived at from the other side.
 *
 * Only the kinds above, so that this stays a sweep behind a rewrite rather than a dead-code pass with
 * an opinion about calls, loads and stores. Iterated, because dropping one value is what makes its
 * operands dead in turn.
 */
bool removeDeadValues(LowerBase base, Region<LowerRegion>& arena, LowerFunction& fun);

/*
 * The immediates nothing reads any more, dropped.
 *
 * A fold never removes the operands it consumed, and cannot: the immediate a caller built may be
 * about to be used a second time by an instruction that has not been created yet - `encodeNarrowRef`
 * builds one mask and both trims and complements it - so "no uses yet" is not "no uses". They are
 * swept once the function is finished instead, where the count is final.
 *
 * Only immediates, so that this stays a cleanup after folding rather than a dead-code eliminator with
 * an opinion about calls and loads. Iterated, because dropping one immediate can be what makes the
 * next one dead.
 */
void removeDeadConstants(LowerBase base, Region<LowerRegion>& arena, LowerFunction& fun);
