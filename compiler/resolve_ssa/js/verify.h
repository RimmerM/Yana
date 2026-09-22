#pragma once

#include "repr.h"

namespace jsir {

/*
 * The main reason this tier exists - Analysis-IR-JS.md part 4, summary item 8.
 *
 * A missing definition cannot become `null`; a number cannot be used as an object receiver; parts
 * cannot be used as one value without materialization; caller and callee ABIs must agree; and every
 * write must go through a valid place. Each of those is a failure this backend has actually had, and
 * each of them looks like working JavaScript that computes the wrong number.
 *
 * **On in every build**, which is §10.5's acceptance criterion and not the usual assertion-build
 * rule: the final verifier is the compiler's malformed-IR defence rather than development cost, and
 * a shipping build that emitted a body the verifier would have refused is a wrong answer nobody has
 * a reproduction for. It is one linear walk plus one dominator computation over a function's blocks.
 *
 * Reports through `target` and answers false. Every check that fails is reported, not only the
 * first, so that a broken pass is one report away from being understood rather than one per run.
 */
bool verifyBody(JsTarget& target, JsBody& body);

} // namespace jsir
