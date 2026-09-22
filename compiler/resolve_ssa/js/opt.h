#pragma once

#include "build.h"

/*
 * The JSIR optimizer - Analysis-IR-JS.md part 6, and the one tier that owns the facts target
 * representation creates.
 *
 * One entry point over one converted body, run between the conversion and the AST builder and
 * switchable off whole (`-no-jsir-opt`, §10.2's third configuration). Everything here is a rewrite
 * that leaves the body verifiable: `verifyBody` runs on what this returns in every build, and in an
 * assertion build it runs on what it was *given* as well, so a body this file broke is named here
 * rather than at the emitter.
 *
 * What it must not become is a second ownership model. §2.4 gives this tier "whether a copy may be
 * observed as an alias", and the facts it discharges that with are the resolver's - the shared loan
 * a `JsPassing::shared` position records, and the demand pass's answer that a `Var` is a local whose
 * address never left. Nothing below re-derives ownership by looking at instruction kinds.
 *
 * The counters are `JsTarget::optimizer` and `-mode jsir` prints them; see `JsOptCounts`.
 */
namespace jsir {

// Rewrites `body` in place. Answers false only where it reported, which it never does today - a
// rewrite that cannot be proved is not made - and there is no "could not optimize".
bool optimizeBody(JsTarget& target, JsBody& body);

} // namespace jsir
