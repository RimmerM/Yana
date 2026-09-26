#pragma once

#include "lower_inst.h"

/*
 * The functions and globals nothing in a finished program can reach, taken out of the module - J176.
 *
 * `resolve_ssa/opt/reach.cpp` answers the same question over SSA, and its answer is right for the
 * program it is asked about. It is asked before the lower tier, and the lower tier removes
 * references too: a call whose condition `threadDecidedBranches` or `foldDecidedBranches` decided, a
 * teardown `expandTeardownCalls` wrote out in its caller, a constant table `foldConstantLoad` read at
 * compile time, the last check of a counted loop `eliminateBoundedChecks` discharged. And the resolve
 * tier roots what the lowering *will* call without an SSA instruction naming it - the allocator on
 * `Program::usesBoxes` - on a fact nothing clears once the box it was set for went away.
 *
 * Here every call is an instruction, so there is no root but the ones a backend enters from outside:
 *
 *  - `module.entry`, which both backends and both test harnesses find by name;
 *  - a `foreignBoundary` function, which something outside the program may enter;
 *  - the `builtins` and the image anchor, which a backend writes into or measures from without an
 *    instruction naming them.
 *
 * And the edges are the three things that name a function or a global: a `fun` or `global`
 * instruction in a reached body, a relocation in a reached global (a witness table's slot, a string
 * run, a jump table's cell), and a reached function's `prefix`. The x86 backend's own references -
 * a pooled constant, a jump table's `symbol` - are made after this runs, out of what it keeps.
 *
 * A module with no entry is a library, and every declaration of one is a root: nothing is swept.
 *
 * Measured (J176): over the 625 `test/resolve` programs, -4,214 bytes of text (0.9935x total,
 * 0.974x geomean, 113 goldens and every one of them removals only), -1,056 bytes of `test/base`, and
 * `Pipeline` -15 among the bench programs. The LLVM backend was already dropping these, so its bytes
 * do not move. What it cannot take is a function a *live* table names in a slot nothing reads: a
 * descriptor's `teardown()`, an erased iterator's thunks. That is another 4.6 KB over 43 fixtures, 2 KB
 * of it `iter/Witness`, and removing it needs to know which slots an erased body reads, which a
 * relocation walk cannot tell.
 *
 * Behind every function's pipeline, because what it reads is every body's final set of references,
 * and in front of `removeUnusedParameters`, because a dead caller's call is a use that pass would
 * otherwise have to keep a parameter for.
 */
void removeUnreachableDeclarations(LowerBase base, LowerModule& module);
