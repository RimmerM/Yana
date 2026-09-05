#pragma once

#include "../convert.h"
#include "manifest.h"

namespace ssa {

/*
 * SSA to the shared lower IR.
 *
 * `compiler/lower` and everything below it are shared with the current resolver and are not part of
 * the rewrite: what changes is which stage produces a `LowerModule`, not what one is. That is the
 * seam Part 11.2 names - the two front halves meet at this type, so a difference between the two
 * pipelines is a difference in what they *decided*, with the whole backend held fixed.
 *
 * Native only. JavaScript is deliberately not a consumer of this stage: `Analysis-IR-JS.md`'s JSIR
 * is built against SSA from the start rather than by teaching this walk a second target, which is
 * how the current JS backend acquired representation questions of its own.
 */
Ptr<LowerModule> lowerProgram(Context& context, mir::Program& program, Converted& converted);

} // namespace ssa
