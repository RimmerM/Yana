#pragma once

#include "../resolve/module.h"
#include "Net/Stream.h"

/*
 * The layout this program's target chose, for every concrete record it contains.
 *
 * A dump rather than a diagnostic, and opt-in per fixture: it is the only way to assert a Repr
 * decision that nothing has consumed yet. A niche the search finds and the access lowering does not
 * use is invisible in emitted code, and a layout nobody can see is a layout nobody can check.
 */
void printReprs(Net::Writer& writer, Context& context, Program& program);
