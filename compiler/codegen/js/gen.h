#pragma once

#include "ast.h"
#include "../../resolve/module.h"
#include "Net/Stream.h"

/*
 * The JavaScript exit ramp - Analysis-JS.md part 3.
 *
 * This consumes `resolve/module.h` and nothing below it, which is the one structural decision the
 * whole target depends on (§3.1): `compiler/lower`'s IR is five machine types, allocas and byte
 * offsets, and emitting JS from that means emitting a typed-array heap - which fails the
 * performance contract outright and cannot hand a value to a host API. So the pipeline forks
 * *before* address lowering, and on this side a place stays a `(object, property)` pair all the way
 * to emission.
 *
 * What that buys is §3.3: three of the four ownership instructions cost nothing here. A borrow is
 * the object reference, a move is the object reference with the source statically dead, and a
 * derived reclaim is the host collector's business. Only `InstCopy` and the authored half of
 * `InstDrop` emit anything at all.
 *
 * See README.md in this directory for what is implemented and what is not.
 */
namespace js {

// Builds the JS form of a resolved program. Reports through `context.diagnostics` for anything the
// target has no meaning for, and returns the file it managed to build regardless - a diagnostic
// plus the surrounding code is more use than nothing.
Ptr<File> genProgram(Context& context, Program& program);

void formatFile(Net::Writer& writer, Context& context, File& file, bool minify);

// The two together, for a caller that only wants the text.
void printProgramJs(Net::Writer& writer, Context& context, Program& program, bool minify = false);

} // namespace js
