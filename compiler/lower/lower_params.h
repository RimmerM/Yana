#pragma once

#include "lower_inst.h"

/*
 * A parameter nothing in its function reads, taken off the function and off every call to it - J170.
 *
 * The shape it was built for is a teardown. `Reclaim(String)` takes the string it releases, a string
 * crosses a call as its three words - the run, its capacity and placement flag, and its length - and
 * the release reads the first two. So each of `test/base`'s 2,362 calls to it loaded a length for
 * nothing: 9,702 bytes of `mov` into a register the callee never looks at. The convention cannot know
 * which words a body will read, because it is decided from the declaration
 * (`resolve_ssa/lower/convention.cpp`); by the time this runs, every body is final and the question
 * is a use count.
 *
 * Measured, it was more than the moves: `test/base` went 909,960 -> 888,168 bytes (581 functions
 * smaller, 52 larger), with the time inside the control and `test/bench/lexer` inside it too. 2,883
 * arguments went in all - `runSlots(CodeUnit)`'s probe is the second shape, 440 of them.
 *
 * **A module pass behind every function's pipeline**, which is the only point where both halves are
 * known: a call is rewritten in a caller's body and the parameter in the callee's, and each needs the
 * other's final form - a parameter becomes unused only once its own function's folds have run, and a
 * caller's load of the dropped word only goes once the caller is swept again. Iterated, because the
 * sweep can leave a caller's own parameter with nothing reading it but the operand just removed.
 *
 * **Which signatures it may change** is the whole of the safety argument, and the rule is that every
 * way into the function is a call this pass rewrites:
 *
 *  - every `fun` naming it is read only as the callee of a direct call, of its own convention and its
 *    own arity - a function value, a witness slot or a closure is a caller this cannot see;
 *  - no data relocation names it, which is how a descriptor's teardown and a table's method reach one;
 *  - it is not the entry, the allocator or a `foreignBoundary`, which are called from outside;
 *  - and its convention is one this compiler owns (`Complex`, `Simple`) - a System V or interrupt
 *    signature is a promise to somebody else, even where nothing outside calls it today.
 *
 * Declined: a parameter read only by the function's own recursive call is still read. Taking it off
 * means assuming it goes and checking that the recursion agrees, and after this pass `test/base` has
 * no unread parameter left at a direct call for that to find.
 */
void removeUnusedParameters(LowerBase base, LowerModule& module);
