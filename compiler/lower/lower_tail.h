#pragma once

#include "lower_inst.h"

/*
 * A function that calls itself last, turned into a loop - item 1 of the fourteenth list in
 * test/bench/findings.md, standing since the seventh.
 *
 * `total` in test/bench/programs/Tree.yana is `node.value + total(left) + total(right)`, and the
 * right-hand call is the one this reaches: nothing runs after it but the addition that consumes its
 * answer, so the frame it would push is a frame with nothing left to do in it. The call becomes a
 * jump back to the top with the argument replaced, and the addition becomes an accumulator carried
 * round the loop:
 *
 *     %r  = load %node@right                   %node = phi [entry, %arg], [latch, %r]
 *     %v  = call total, %r          ->         %acc  = phi [entry, 0], [latch, %sum]
 *     %s  = add %left, %v                      ...
 *     ret %s                                   %sum = add %acc, %left ; jmp header
 *
 * LLVM's own `-Os` column does this - see §14.4 of findings.md, which sized the item at nearly the
 * whole of `Tree`'s gap - and one call per node rather than two is what the measurement is.
 *
 * ## Why here rather than in compiler/opt
 *
 * §14.4 costed the transform against the *resolve* IR and found two obstacles that are both gone by
 * this stage. The accumulator is a stack slot up there, so threading one through would be a
 * promotion before it was a transform; and `node` is a memory-typed parameter, so the loop would
 * need a phi over a place rather than over a value. Down here `promoteStackSlots` has already made
 * the accumulator an ordinary phi and the parameter is a `Ptr`, so what is left is the textbook
 * rewrite over SSA. This is the same answer §12.6 reached for induction reduction: a transform that
 * reads as a resolve-tier one is a lower-tier one whenever what it needs is registers rather than
 * types.
 *
 * The cost of that placement is that the JS backend does not get it - it reads the resolve IR and
 * never comes through here.
 *
 * ## What a call has to look like
 *
 * The whole of the analysis is "does anything run after this call", asked as a walk from the call's
 * result to a `ret`. Each step is one of two things:
 *
 *  - **an accumulation.** A binary operation over integers, associative *and* commutative, one of
 *    whose operands is the value the walk is carrying. Add, Mul, And, Or and Xor qualify; float
 *    arithmetic does not, because reassociating it changes the answer, and neither does anything
 *    whose operand order matters.
 *  - **a phi in the next block.** Which is how `if c then sum = sum + f(x)` reaches its return, and
 *    the block it is in must hold nothing but phis - an instruction there is work that runs after
 *    the call on this path and before it on every other one.
 *
 * The value being carried must have exactly one reader at every step. A second reader is a use of
 * the call's answer that survives the call, which is precisely what the frame was being kept for.
 *
 * ## What the whole function has to look like
 *
 * **No `alloca` anywhere in it.** A loop iteration reuses the frame the recursion would have made a
 * new one of, so a slot whose address the callee could still be holding is a slot two iterations
 * would share. Nothing after the call can read one *in this frame* - that is what the tail property
 * says - but the callee received the address, and the callee is this same function. Declined
 * outright rather than reasoned about per slot.
 *
 * **One accumulating operation.** Two chains that accumulate differently would need an accumulator
 * each and an identity each; a chain that accumulates nothing at all - a plain tail call - is
 * compatible with any of them, since it leaves the accumulator alone.
 *
 * The accumulator is applied at *every* remaining `ret` in the function, not only at the one the
 * chain reached: `acc` holds what the frames above this one contributed, and every way out of the
 * loop owes it. Its initial value is the operation's identity, which is why the operation has to
 * have one.
 *
 * ## Where it runs
 *
 * After `promoteStackSlots`, which is what makes the accumulator a value in the first place, and
 * before the CSE and the loop passes - the loop this builds is an ordinary loop, and hoisting,
 * induction reduction and rotation should all see it as one.
 */
void eliminateTailRecursion(LowerBase base, LowerModule& module, LowerFunction& fun);
