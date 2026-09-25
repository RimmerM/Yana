#pragma once

#include "repr.h"
#include "Net/Stream.h"

namespace ssa {

/*
 * The layout this program's target chose, for every concrete record it declares.
 *
 * A dump rather than a diagnostic, and opt-in per fixture: it is the only way to assert a layout
 * decision that nothing has consumed yet. A niche the search finds and the access lowering does not
 * spend is invisible in emitted code, and a layout nobody can see is a layout nobody can check.
 *
 * **The format is `compiler/resolve`'s, byte for byte.** While that stage existed (it was deleted
 * in J173) the two were meant to lay a native program out identically, and a fixture whose two
 * goldens could be diffed was how they were held to it. Where they differed was where this stage's
 * `NativeTarget` has a feature switched off - see the three flags there, which were the gap.
 */
void printReprs(Net::Writer& writer, Context& context, mir::Program& program);

} // namespace ssa
