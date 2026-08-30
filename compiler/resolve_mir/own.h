#pragma once

#include "decl.h"

namespace mir {

/*
 * Ownership classification - `doc/spec/ownership.md`'s Teardown, and the one thing about a type that
 * a *later declaration* can change.
 *
 * Every other property of a logical type is decided where the type is made: its kind, its width, its
 * fields. Whether it releases something is decided by the whole program, because an
 * `instance Drop(T)` written at the bottom of a file is a statement about a type declared at the
 * top. So it is a pass rather than a query, run once between the declaration walk and the bodies,
 * and everything afterwards reads `ownershipOf` and gets the settled answer.
 *
 * Running it as a query instead is the mistake this shape avoids. A cached query answers differently
 * before and after the instance that changes it, and nothing says which callers ran first - the
 * current compiler needs a fixpoint with an assumption stack for exactly that reason, and it needs
 * one because its classification is asked from inside type construction.
 *
 * Returns false and reports where the member graph has a cycle: a record that contains itself has no
 * layout, and `@box` - the written indirection that breaks one - is not implemented here yet.
 */
bool classifyTypes(Context& context, Program& program);

} // namespace mir
