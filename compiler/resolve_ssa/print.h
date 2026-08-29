#pragma once

#include "convert.h"
#include "Net/Stream.h"

namespace ssa {

/*
 * The SSA dump - `-mode ssa`, and the `.ssa.expect` fixture golden.
 *
 * Every value's convention and every use's kind are on the face of it, for the same reason the MIR
 * dump prints modes: the invariant this tier maintains is about exactly those two things, and a
 * golden that hid them would assert the shape of the graph and nothing about its ownership.
 */
void printProgram(Net::Writer& writer, Context& context, mir::Program& program, Converted& converted);

} // namespace ssa
