#pragma once

#include "convert.h"

namespace ssa {

/*
 * The SSA verifier - Analysis-IR.md §7.2 and §12.1.
 *
 * A different set of questions from the MIR verifier's, deliberately kept separate so that a
 * failure is attributable: MIR's are about whether the source was represented, these are about
 * whether the linear SSA invariants hold. An error here is conversion's or an optimizer pass's, and
 * an error there is resolution's.
 *
 * What it checks:
 *  - every use names a value that exists, whose definition **dominates** the use - which is block
 *    dominance *and* instruction position within the defining block, because every block dominates
 *    itself and the block test alone therefore says nothing about two points inside one;
 *  - a value's use list and the instructions that name it agree, in both directions and with
 *    multiplicity;
 *  - an `Owned` value is consumed exactly once **on every reachable path**;
 *  - a `Guaranteed` value is not consumed at all;
 *  - each block ends in exactly one terminator, and each edge's arguments match its target's
 *    parameters in occurrence and convention;
 *  - the entry block has no predecessors.
 *
 * The linear rule is a CFG dataflow and not a whole-body count. That distinction is the whole point:
 * a count is not a weaker form of the path rule, it is a different rule that is wrong in both
 * directions - it rejects an owned value consumed once in each arm of an `if` (the count is two) and
 * accepts one consumed in only one arm (the count is one). There is therefore no version of it that
 * can serve as an interim check before the first `Owned` value exists, which is why the dataflow is
 * here now rather than after.
 *
 * Run after every ownership-sensitive optimizer pass in an assertion build, and once unconditionally
 * before lowering.
 */
bool verifyBody(Context& context, mir::Program& program, SsaBase base, mir::Function& function);
bool verifyProgram(Context& context, mir::Program& program, Converted& converted);

} // namespace ssa
