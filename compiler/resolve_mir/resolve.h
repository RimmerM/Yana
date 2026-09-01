#pragma once

#include "decl.h"
#include "../parse/ast.h"
#include "../parse/library.h"

namespace mir {

/*
 * Source to MIR.
 *
 * This is the front half of the rewritten stage: name resolution, typing, and MIR construction, in
 * that order and with the ownership contract of every occurrence decided before a node naming it is
 * built. Overload selection has to finish before a call's operand modes are recorded (§4), which is
 * why resolution and MIR construction are one pass rather than two.
 *
 * ## What this covers
 *
 * Part 11.4's slices 1 to 6 and the imports of slice 8 - see `README.md`, which is where the whole
 * of it is written down. There is still **no prelude**: every type name the language has of its own
 * is builtin (see `Types::findBuiltin` and `intrinsic/primitive.def`, which is the roster),
 * `Program::core` is null, and the implicit import of Core that `linkImports` would add therefore
 * contributes nothing. Anything a source file names that this stage does not
 * implement is reported as unsupported by name, rather than half-resolved into a body the later
 * stages would have to guess at.
 *
 * Reporting rather than asserting is deliberate for exactly this reason: the new stage is chosen by
 * a flag over a corpus written for the old one, so meeting a construction it does not implement is
 * an ordinary event and has to say which construction it was.
 */
/*
 * `root` and everything it imports.
 *
 * `provider` is where a module that is not `root` and is not the standard library's comes from - the
 * project's own files. Null is a compilation whose only modules are `root` and whatever `lib/`
 * answers, which is what a single-file fixture is.
 */
Ptr<Program> resolveProgram(Context& context, ast::ModuleGroup& root,
                            ast::ModuleProvider* provider = nullptr);
Ptr<Program> resolveProgram(Context& context, ast::Module& root,
                            ast::ModuleProvider* provider = nullptr);

} // namespace mir
