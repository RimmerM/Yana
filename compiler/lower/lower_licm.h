#pragma once

#include "lower_inst.h"

/*
 * The loads a loop repeats - §10 item 1 of test/bench/findings.md.
 *
 * A read whose address does not change between iterations and whose answer nothing in the loop can
 * change is one read, done once. `hashOf` in test/bench/programs/Text.yana is the shape: the string's
 * base pointer is a field of the parameter, so every iteration of the scan loads it again before
 * indexing off it - `mov (%rdi),%r8` as one of the eight instructions in the corpus's worst `-Os`
 * row.
 *
 * ## Why here and not above the fork
 *
 * `hoistLoopValues` in compiler/opt is the same idea over the resolve IR, and it cannot reach this
 * one. The read is `text.bytes.run.items`, whose place is rooted in a `borrow` of the parameter - and
 * the borrow is *re-taken inside the loop*, because that is what the resolver emits for each call
 * that reads through it. So the place's root is a value defined in the loop, the hoister's
 * `operandsOutside` refuses it before any aliasing question is asked, and hoisting the borrow itself
 * is a decision that stage declines to take (see opt_loop.cpp's header, and Analysis-Optimization.md
 * §7): a loan moved out of a loop covers iterations the borrow check never agreed to.
 *
 * Down here the borrow has become an address, the address is `%text` itself, and the question is the
 * plain one - is this pointer the same every time round, and does anything in the loop write.
 *
 * ## The two things that have to be true
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
 * ## Where it runs
 *
 * After `eliminateCommonValues`, so that a loop reading one address twice is shown one load rather
 * than two, and before `reduceInductionVariables`, which is where the induction pass expects the
 * loop's invariant parts to have stopped moving.
 */
void hoistLoopLoads(LowerBase base, LowerModule& module, LowerFunction& fun,
                    const LoopAnalysis& analysis);
