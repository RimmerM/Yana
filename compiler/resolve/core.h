#pragma once

#include "module.h"

/*
 * The Core module.
 *
 * Core is an ordinary module - it has a name, declarations, classes and instances, and every
 * other module reaches it through the same import machinery it would use for any other. What it
 * does not have is a file: its declarations are parsed from source embedded in the compiler, and
 * its primitive instances are generated directly, because `Int`'s `+` cannot be written in terms
 * of anything more basic.
 *
 * Those generated instances are real functions with real bodies, and they also carry an
 * intrinsic hook, so an ordinary call to `+` expands to the one instruction it contains instead
 * of to a call some later pass would have to inline. Nothing about the call site knows this: it
 * selects a Num instance exactly as it would for a user-defined type.
 */
void defineCore(Program& program);
