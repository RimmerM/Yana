#pragma once

#include "../convert.h"
#include "manifest.h"

namespace ssa {

/*
 * SSA to the shared lower IR.
 *
 * `compiler/lower` and everything below it are not part of this stage: what is decided here is
 * which `LowerModule` to produce, not what one is. That is the seam Part 11.2 named while
 * `compiler/resolve` (deleted in J173) produced one too - the two front halves met at this type, so
 * a difference between the two pipelines was a difference in what they *decided*, with the whole
 * backend held fixed.
 *
 * Native only. JavaScript is deliberately not a consumer of this stage: `Analysis-IR-JS.md`'s JSIR
 * is built against SSA from the start rather than by teaching this walk a second target, which is
 * how the old JS backend (deleted in J173) acquired representation questions of its own.
 */
Ptr<LowerModule> lowerProgram(Context& context, mir::Program& program, Converted& converted);

} // namespace ssa
