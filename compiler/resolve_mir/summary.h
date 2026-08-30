#pragma once

#include "decl.h"

namespace mir {

/*
 * The one interprocedural pass - Analysis-Sharing.md §4, Analysis-IR.md §10.3.
 *
 * Reachability and every published fact in `Summary` are computed here, by one walk over the call
 * graph, and read everywhere else through `Function::summary`. That is the arrangement §4 asks for
 * and the arrangement the current compiler does not have: storage class, mutation demand, argument
 * retention, linear consumption, the instantiation set and suspension all give up at the same
 * boundary - an indirect call, a separately compiled callee - and six passes each with its own
 * lattice and its own conservative fallback is how six of them came to disagree about where that
 * boundary is.
 *
 * Only suspension is computed at this milestone, because it is the only one whose inputs exist. The
 * pass is written as the fixpoint the other five join rather than as a walk that answers one
 * question: what a second fact costs here is a base case and a join, not a second traversal.
 *
 * Runs over sealed, verified bodies and writes nothing but `Function::reachable`, `Global::reachable`
 * and `Function::summary` - so a body it read is a body nothing has rewritten.
 */
void computeSummaries(Context& context, Program& program);

} // namespace mir
