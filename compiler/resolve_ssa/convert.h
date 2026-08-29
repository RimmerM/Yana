#pragma once

#include "build.h"

namespace ssa {

/*
 * MIR to SSA - Analysis-IR.md §4's phase boundary, and §6.7's drop elaboration.
 *
 * This is where the semantic questions stop. After it, no pass asks whether a bare source use was
 * meant to move, whether an aggregate component was handed over, or which predecessor owned a merge
 * input: those have answers in the graph, and the only invariant left is the linear one the SSA
 * verifier checks.
 *
 * Drops are emitted *during* the conversion rather than by a pass that rewrites MIR first, which is
 * §9.2's economy: an elaboration round that inserted instructions into MIR only to copy them
 * immediately is a round that can be skipped.
 *
 * At this milestone every value is trivial, so there is nothing to drop and nothing conditional to
 * flag - the placement rules are stated in the code where they will attach, and the first fixture
 * with an owner is what will exercise them.
 */
struct Converted {
    SsaArena arena { 4 * 1024 * 1024 };
};

// Converts every reachable function of `program`, filling in each one's `ssa` handle. Returns false
// and reports if any body was rejected; the MIR region is *not* released here, because a caller that
// wants to print the MIR of a program it also lowered has to be able to.
bool convertProgram(Context& context, mir::Program& program, Converted& into);

} // namespace ssa
