#pragma once

#include "lower_inst.h"

/*
 * The run of passes every producer of this IR owes its output, in the one order that was argued.
 *
 * The sequence used to live at the bottom of `resolve/lower.cpp`, which is where it was written and
 * not where it belongs: the order between its members is argued entirely in terms of the lower IR -
 * what promotion leaves for the forwarding to see, which passes index by block, which of them the
 * loop analysis may be shared across - and none of that mentions a resolve type. Both resolve stages
 * produce this IR, so the second one either called the same nineteen functions in the same order or
 * quietly shipped a worse program; `resolve_ssa` did the second, with one call in it and a comment
 * saying so.
 *
 * The comments moved with the calls. They are the whole value of the sequence - each says why a pass
 * is behind the one above it - and splitting them from the calls would leave two orders to keep in
 * step.
 *
 * `optimize` is `settings.optimizeIr`, and it gates everything except two passes that are not
 * optimizations at all - `makeDivisionTotal`, because the language defines what a division by zero
 * answers and no machine does, and `foldDecidedBranches`, because a library arm gated on a
 * compile-time constant hides code the running target cannot encode behind a branch. The reason
 * each is exempt is written where it is called; both are things without which the compiler produces
 * no program rather than a slower one. That gate is new for the shipping
 * stage - these passes ran there unconditionally, because `optimizeIr` was read only in
 * `opt/opt.cpp` - and it is the point of the move as much as the sharing is: `-no-ir-opt` now means
 * one thing on both stages, and the fixture runner's optimized/unoptimized comparison covers the
 * lower tier instead of stopping above it.
 *
 * `returnPlace` is the hidden result pointer where this function has one, and null where it does
 * not. Only `forwardCopyDestinations` reads it - see lower_forward.h, which is the whole of why the
 * pass has to be told which parameter that is.
 */
void optimizeLowerFunction(LowerBase base, LowerModule& module, LowerFunction& fun,
                           LowerPtr<LowerValue> returnPlace, bool optimize);
