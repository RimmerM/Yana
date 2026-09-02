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
 * Every check that needs the finished signatures and initializers - an intrinsic's shape
 * (`shape.h`) and a supplied global's storage, type and value.
 *
 * Run once, after the declaration passes and after initializers - a supplied global's check reads
 * the constant its initializer settled on. Answers false where it reported, which is what stops a
 * body being resolved against a library the compiler cannot use.
 */
bool checkRoles(Context& context, Program& program);

} // namespace mir
