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
 * **The format is the shipping stage's, byte for byte**, and that is the whole point of it: the two
 * resolve stages are meant to lay a native program out identically, and the only way to hold them to
 * that is a fixture whose two goldens can be diffed. Where they differ today is where this stage's
 * `NativeTarget` has a feature switched off - see the three flags there, which are the gap.
 */
void printReprs(Net::Writer& writer, Context& context, mir::Program& program);

} // namespace ssa
