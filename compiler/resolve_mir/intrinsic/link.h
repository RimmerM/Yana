#pragma once

#include "roles.h"
#include "../decl.h"
#include "../../parse/ast.h"

namespace mir {

/*
 * `@builtin(role)` on one declaration, bound to it.
 *
 * `entity` is what this declaration *is*, and `Nothing` says it is a kind that can never claim a
 * role - an `instance`, an `alias`. Passing that rather than leaving those sites out is what makes
 * a misplaced attribute a diagnostic: every declaration form calls this, so `@builtin(drop)` on an
 * `instance` is reported at the attribute rather than silently doing nothing.
 *
 * `declaration` is the claimant's region offset, ignored where `entity` is `Nothing`.
 *
 * Answers the role it bound, so that a caller which has somewhere to put it - a `Function`'s
 * intrinsic operation, a `Class`'s role - can, without reading the attribute a second time.
 */
Maybe<intrinsic::RoleRef> bindRole(Context& context, Program& program, ast::ParseBase parse,
                                   ast::Decl& declaration, Maybe<intrinsic::Entity> entity,
                                   U32 declaration_);

/*
 * The `Known` roles against the declarations that claimed them, and the report for one nothing
 * filled.
 *
 * Run once the classes have their members and the records their constructors, and **before** the
 * instances the compiler generates: `provideInstances` reaches a class's members by position, so a
 * class of the wrong shape has to be refused before anything indexes it. Answers false where it
 * reported, which is what stops the generation.
 */
bool checkDeclaredRoles(Context& context, Program& program);

/*
 * One supplied global, checked and **filled** - `supplied.def`'s `BuiltinKind::Supplied`.
 *
 * Called the moment that global's own initializer is settled rather than in a pass of its own, and
 * the ordering is the whole reason it is a function: `pageBytes` is a constant a *later* constant
 * may read - `let mask = pageBytes - 1` - and a supply step that ran after every initializer would
 * hand that reader the zero the declaration is required to write. So the value is the target's from
 * the moment the declaration exists, which is what "an ordinary immutable global with a constant
 * initializer" has to mean if anything is to be allowed to depend on it.
 *
 * Answers false where it reported. `checkRoles` carries that answer forward, so a library whose
 * supplied global is the wrong shape still stops the compilation at the same point it used to.
 */
bool bindSuppliedGlobal(Context& context, Program& program, DeclPtr<Global> global);

/*
 * Every check that needs the finished signatures - an intrinsic's shape (`shape.h`).
 *
 * Run once, after the declaration passes and after initializers. Answers false where it reported,
 * which is what stops a body being resolved against a library the compiler cannot use - including
 * for a supplied global `bindSuppliedGlobal` above already refused.
 */
bool checkRoles(Context& context, Program& program);

} // namespace mir
