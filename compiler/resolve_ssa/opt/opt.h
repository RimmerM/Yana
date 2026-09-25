#pragma once

#include "../convert.h"

namespace ssa {

/*
 * The optimizer over ownership SSA - see README.md in this directory, which is the argument for
 * every pass that is here and for the eight old ones that are not.
 *
 * It runs between conversion and lowering, on a program whose ownership questions have all been
 * decided: MIR settled what the source meant, the conversion spent that into explicit instructions,
 * and what is left is a graph with one invariant on it. That invariant - the linear rule the SSA
 * verifier checks - is what makes this the low-risk tier to rewrite ownership-bearing IR in: a pass
 * that breaks it is *reported*, at the pass, rather than discovered as a double free.
 *
 * Below it is `lower/`, and the run of shared passes over the lower IR that every producer of that
 * IR owes its output (`lower/lower_pipeline.h`). The two are not alternatives. This one has what the
 * lower tier has thrown away - logical types and their refinements, conventions and loans, call
 * signatures, aggregate structure and continuation regions - and the sequence down there has the
 * machine's arithmetic and the frame. A question is answered at the tier that can state it.
 *
 * `settings.optimizeIr` (`-no-ir-opt`) switches the whole of it off.
 *
 * Answers whether the program still verifies. False is an internal defect - a pass produced IR the
 * tier's own rules reject - and it has already been reported by the time this returns.
 */
bool optimizeProgram(Context& context, mir::Program& program, Converted& converted);

} // namespace ssa
