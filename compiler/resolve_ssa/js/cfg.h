#pragma once

#include "repr.h"

/*
 * `JsCfg` for one body - the struct is `module.h`'s, beside the other per-function side tables, and
 * this is the one function that fills it. See `JsCfg` for what the three walks this replaced were.
 */
namespace jsir {

/*
 * The facts for one body, computed on the first ask and cached until `JsTarget::beginBody`.
 *
 * Every consumer takes it by reference and none of them may keep it past the body: it lives on the
 * target so that the storage is the widest body ever seen rather than one set of arrays per pass
 * per function, which is `compiler/util/README.md`'s rule and §7.4's.
 */
const JsCfg& jsCfgOf(JsTarget& target, JsBody& body);

} // namespace jsir
