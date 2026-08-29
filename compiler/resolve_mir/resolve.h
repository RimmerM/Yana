#pragma once

#include "decl.h"
#include "../parse/ast.h"

namespace mir {

/*
 * Source to MIR.
 *
 * This is the front half of the rewritten stage: name resolution, typing, and MIR construction, in
 * that order and with the ownership contract of every occurrence decided before a node naming it is
 * built. Overload selection has to finish before a call's operand modes are recorded (§4), which is
 * why resolution and MIR construction are one pass rather than two.
 *
 * ## What this milestone covers
 *
 * Part 11.4's first slice, and no more: a module of `let` constants and functions whose bodies read
 * one. There is no prelude - `Core` is not built and not needed, because the width names are builtin
 * (see `Types::findBuiltin`) and Core's `Int`/`Long` are aliases over them. Anything else in a source
 * file is reported as unsupported by name, rather than half-resolved into a body the later stages
 * would have to guess at.
 *
 * Reporting rather than asserting is deliberate for exactly this reason: the new stage is chosen by
 * a flag over a corpus written for the old one, so meeting a construction it does not implement is
 * an ordinary event and has to say which construction it was.
 */
Ptr<Program> resolveProgram(Context& context, ast::ModuleGroup& root);
Ptr<Program> resolveProgram(Context& context, ast::Module& root);

} // namespace mir
