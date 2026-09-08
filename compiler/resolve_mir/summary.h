#pragma once

#include "decl.h"

namespace mir {

/*
 * The one interprocedural pass - Analysis-Sharing.md §4, Analysis-IR.md §10.3.
 *
 * Reachability and every published fact in `Summary` are computed here, by one walk over the call
 * graph, and read everywhere else through `Function::summary`. Two relations come out of the walk
 * and they are kept apart: what the program **reaches** - every declaration a body names, by a
 * call, a function value, a witness table or a descriptor - and what a body **invokes** - the
 * callees that run inside its activation. Suspension propagates over the second alone; a body that
 * builds a suspending function value and hands it on has not suspended, and a body compiled once
 * runs the witnesses in the slots it reads, not every slot it was handed. That is the arrangement
 * §4 asks for and the arrangement the current compiler does not have: storage class, mutation
 * demand, argument retention, linear consumption, the instantiation set and suspension all give up
 * at the same boundary - an indirect call, a separately compiled callee - and six passes each with
 * its own lattice and its own conservative fallback is how six of them came to disagree about
 * where that boundary is.
 *
 * **Every fact is computed here, by one walk.** `computeSuspension` and `computePlacement` are a base
 * case and a join over the reversed call graph; `computeArgumentFacts` is the same shape for the
 * four per-position rows, with each call site's arguments resolved to the caller's parameters once
 * so the fixpoint is set algebra rather than a re-analysis - twice over, because `retains` follows
 * what a position reaches as a *value* and `escapes` what it hands over as an *address*, and the two
 * are different sets over the same positions; `computeStack` is a peel rather than a
 * join, because nothing in a recursive cycle ever *becomes* unbounded by a callee changing its
 * answer; and `computeDivergence` recomputes a body's CFG answer when a callee's does change, which
 * is what makes "every path ends in a call that does not come back" transitive.
 *
 * What each fact rests on is `analyze_*.cpp`'s work rather than a second dataflow. Retention and
 * mutation are the loan checker's origin environment read per argument - `Elaboration::rootedIn`,
 * which already says which loans a value may be rooted in - and placement is what
 * `analyze_escape.cpp` decided. **Escape is the same walk with the loads taken out**: a pointer read
 * out of the caller's bytes is not the address of the caller's bytes, and separating the two is what
 * `ssa::Contained` asks - see `Summary::escapes`, which is why the row exists at all.
 *
 * The one row that is a signature rather than a walk is `consumes`, and it says so: `checkLinear`
 * refuses a body that does not discharge an owned parameter on every path, so the declaration *is*
 * the answer.
 *
 * `GenCost` is the exception and is filled by `selectGenForms`, for the pass-order reason its
 * accessor gives.
 *
 * Runs over sealed, verified bodies and writes nothing but `Function::reachable`, `Global::reachable`
 * and `Function::summary` - so a body it read is a body nothing has rewritten.
 */
void computeSummaries(Context& context, Program& program);

} // namespace mir
