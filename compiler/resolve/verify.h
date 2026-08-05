#pragma once

#include "module.h"

/*
 * The resolve IR's own consistency check - Analysis-Status.md's first structural improvement.
 *
 * What it is for is the class of bug this IR has produced over and over and that nothing else
 * catches: a transformation that updates one half of a two-sided structure. The IR keeps both
 * directions of every relation it has - an instruction names its operands and every value names its
 * users, a block names its successors and every block names its predecessors, a local names the
 * value that fills it and that value names the slot - and a pass that writes one side leaves an IR
 * that *prints correctly and walks wrongly*. See Implementation-IR.md part 2 for the structures, and
 * `rebuildUses` in compiler/opt/opt.cpp for the repair one pass still relies on rather than
 * maintaining its own lists.
 *
 * The failures it is meant to turn into a message are, in the order they have actually happened:
 *
 *  - a use list naming an instruction that no longer names the value back, which is what makes
 *    "who reads this local" answer wrongly and lets a live allocation be deleted;
 *  - an operand pointing at an instruction that has been removed from its block, which surfaces as
 *    a crash in a backend rather than as anything about the pass responsible;
 *  - a block list out of reverse postorder, which lowering reports as "resolve value was used
 *    before it was lowered" from inside `mappedValue`;
 *  - an edge recorded in two of the three places that hold one - the predecessor's `outgoing`, the
 *    successor's `incoming`, and one alternative per phi in the successor;
 *  - a place whose path does not fit the type it is rooted in, which `walkPlace` answers null for
 *    and whose consumers then read constructor zero of nothing.
 *
 * ## What it is not
 *
 * It is not a type checker for the source language and it never reports about a *program*: every
 * message it produces says the compiler is wrong, not that the input is. So it runs only over a
 * program the resolver accepted - `verifyProgram` returns immediately once anything has been
 * reported - and the driver treats a finding as an internal error.
 *
 * ## Where it runs
 *
 * Four checkpoints, **all of them in assertion builds only** - see the macros at the bottom, which
 * is the whole of how they are reached:
 *
 *  - after every body is resolved, and again after the drop pass, in `resolveProgram`;
 *  - at the top of `optimizeProgram`, which is the last point at which the use lists are as the
 *    resolver and the ownership passes built them - `flattenArguments` immediately below leaves
 *    every list for the `rebuildUses` at the top of `optimizeFunction`, so this is where the
 *    two-sided def-use structure is asked about and everything after is checked against the repair;
 *  - after each pass of the optimizer, which is what makes a finding name the pass that caused it;
 *  - at the end of `optimizeProgram`, immediately in front of both backends.
 *
 * Assertion builds only, on the same terms as `assertTrue` and for a measured reason: the three
 * whole-program checkpoints alone cost a fifth of the release fixture corpus's compile time, because
 * they walk every body of Core, Native, Collections and Text while the optimizer walks only the
 * handful a program reaches. What that buys in a release build is nothing a user can act on - every
 * message here says the compiler is wrong - so it is spent where it is read.
 *
 * ## What it declines to check
 *
 * One thing, and it is a decision rather than an omission: a slot may hold a value several other
 * slots also hold. `inlineCalls` points every slot that named a call at whatever replaced it, on
 * purpose, so the `Local::value` relation is not one-to-one - what is checked instead is the
 * direction that has to hold, that the slot a value names names it back. See verifyLocals.
 */

/*
 * How far through the pipeline the IR being checked is, which decides which invariants hold.
 *
 * The IR is not one shape: `Drop` does not exist until the ownership passes insert it, a storage
 * class is not chosen until escape analysis chooses one, and `ProjectionKind::Unit` is appended by
 * an optimizer pass and by nothing else. A single set of rules would have to be the intersection of
 * all three, which is the weakest of them - so the stage is a parameter and each check states the
 * earliest stage at which it means anything.
 */
enum class VerifyStage: U8 {
    // Bodies are resolved and nothing else has run. No `Drop`, and no storage decisions.
    Resolved,

    // `runProgramOwnership` has settled: drops are inserted, storage is chosen, summaries are final.
    Ownership,

    // Inside or after compiler/opt. Everything above, plus the forms only that stage produces.
    Optimized,
};

/*
 * One function, against the invariants that hold at `stage`.
 *
 * `where` names what has just run, and is the whole of what makes a finding actionable - "after
 * forwardPlaces" is a pass to look at, "the IR is inconsistent" is not. It is printed as written.
 *
 * Answers whether the function is consistent, and reports every problem it finds through the
 * module's own diagnostics rather than stopping at the first: two findings in one function are
 * usually one cause, and seeing both is what identifies it.
 */
bool verifyFunction(Module& module, Function& function, VerifyStage stage, StringView where);

/*
 * Every function of every module, on the same terms.
 *
 * Returns true - "nothing to say" - for a program that already has errors, since an IR built out of
 * a rejected program is expected to be malformed and reporting about it would bury the diagnostic
 * that matters.
 */
bool verifyProgram(Program& program, VerifyStage stage, StringView where);

/*
 * How every checkpoint is reached - a macro, because it must cost nothing in a build without
 * assertions and because the pass name is a literal at each call site.
 *
 * `verifyIr` is one function; `verifyIrProgram` is the whole program, which is what the passes that
 * rewrite every function at once are bracketed by. Both compile to nothing without `_DEBUG` or
 * `DEBUG`, which is the same switch `assertTrue` uses, and everything the pipeline calls goes
 * through one of them - `verifyFunction` and `verifyProgram` are called directly only by a test or a
 * debugging session.
 */
#if defined(_DEBUG) || defined(DEBUG)
#define verifyIr(module, function, stage, where) verifyFunction(module, function, stage, where)
#define verifyIrProgram(program, stage, where) verifyProgram(program, stage, where)
#else
#define verifyIr(module, function, stage, where) ((void)0)
#define verifyIrProgram(program, stage, where) ((void)0)
#endif
