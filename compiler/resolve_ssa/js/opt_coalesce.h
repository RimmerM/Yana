#pragma once

#include "cfg.h"

/*
 * §6.8's block-argument coalescing - the *analysis* half.
 *
 * The emitter gives every join its own argument, so a value reaching a `return` through three
 * nested joins is three variables and two copies. Naming two arguments the same variable makes the
 * edge copy a self-assignment that `emitEdge` writes nothing for: `Base.js` went from 3,327 `var p;`
 * declarations to 427 and from 1,079 copies to 661, which is -28,492 bytes.
 *
 * What this answers is which arguments *may* share, and it answers it with a class per argument.
 * What the emitter does with that is mint one `var` per class, which is still `structure.cpp`'s -
 * see the note at the top of `opt_coalesce.cpp` for why the line is drawn there rather than around
 * the whole thing. `JsCoalesced` and its scratch are `module.h`'s, beside the other per-function
 * side tables.
 */
namespace jsir {

// Computed per body, into `JsTarget::coalesced`. The emitter calls it once, before it names
// anything, and reads the classes back by value id.
const JsCoalesced& jsCoalesceArguments(JsTarget& target, JsBody& body);

} // namespace jsir
