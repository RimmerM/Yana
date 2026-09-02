#pragma once

#include "../region.h"
#include "../../intrinsic/known.h"
#include "../../intrinsic/op.h"
#include "../../intrinsic/supplied.h"
#include "../../intrinsic/provided.h"

namespace mir {

struct Class;
struct Function;
struct RecordType;
struct Global;

/*
 * Binding this stage's declarations to the compiler's roles - `compiler/intrinsic/role.h`.
 *
 * The whole of the source-to-compiler coupling arrives here and leaves as an enumerator. Nothing
 * below this file compares a name against a spelling, asks whether a class is "the drop class", or
 * looks a library function up by text: what a pass holds is a `Known`, an `Op` or a `Builtin`, and
 * `Roles` is what turns one into the declaration that filled it.
 *
 * ## Why the attribute rather than a table of names
 *
 * A role is a *fact about the declaration* - `Truth` is what `if` dispatches through, and that is
 * not a property of the word "Truth". A compiler-side table of library names is that fact written a
 * second time, and the second copy is the one that goes stale: renaming the library's declaration
 * compiles cleanly and unfills the role, which is exactly the failure `@builtin` was introduced for
 * on globals. Written at the declaration, there is one statement and a rename carries it along.
 *
 * It also means the coupling is *visible where it binds*. A reader of `lib/Core` can see which
 * declarations the compiler is fastened to and therefore which ones are not free to change, which a
 * table in another repository directory cannot tell them.
 *
 * ## What is checked, and when
 *
 * Two moments, because the checks need different things:
 *
 *  - **At the declaration**, `bindRole` checks that the role exists, that nothing else claimed it,
 *    and that the declaration is the *kind* the row wants. All three are answerable from the
 *    attribute and the declaration's own form.
 *  - **After the declaration passes**, `checkRoles` checks everything that needs the finished
 *    signature: a class's arity and member count, an intrinsic's shape (`shape.h`), a supplied
 *    global's mutability, type and initializer. And it reports a `Required` role nothing filled.
 *
 * Both are in `link.h` beside this. What is here is the *table*, because `Program` holds one and
 * `decl.h` therefore has to see it - and `link.h` needs `decl.h` in turn.
 */

/*
 * One role, and what filled it.
 *
 * The claimant is a bare region offset rather than a typed handle, because a role's entity kind is
 * the table's answer and storing a `DeclPtr<Class>` beside a `DeclPtr<Function>` beside a
 * `DeclPtr<Global>` would be three fields of which at most one is ever set. The accessors below
 * assert the row's entity and cast, so a typed handle is what a *reader* gets and the storage stays
 * one word.
 */
struct RoleClaim {
    // Zero where nothing claimed the role - offset zero is never a real object, which is the same
    // convention `RegionPtr`'s null uses.
    U32 declaration = 0;

    // Where the attribute was written, for the message that reports a second claimant.
    LocationId source = kNullLocation;

    bool filled() const { return declaration != 0; }
};

struct Roles {
    RoleClaim known[Size(intrinsic::Known::Count)];
    RoleClaim op[Size(intrinsic::Op::Count)];
    RoleClaim supplied[intrinsic::kBuiltinCount];

    /*
     * The class filling a role, or null.
     *
     * Null is the ordinary answer for an `Optional` row in a program whose library does not declare
     * it. A `Required` row is reported by `checkDeclaredRoles` rather than answered null here, so a
     * caller that reaches this after the checks has either a class or a program that already
     * failed.
     */
    DeclPtr<Class> klass(intrinsic::Known role) const;

    DeclPtr<Function> function(intrinsic::Known role) const;
    DeclPtr<RecordType> record(intrinsic::Known role) const;
    DeclPtr<Global> global(intrinsic::Known role) const;

    // The signature implementing an intrinsic operation, or null where nothing claimed it. A role
    // nothing claims is simply not implemented: there is no call to reach, because there is no
    // declaration of that name for one to resolve to.
    DeclPtr<Function> operation(intrinsic::Op role) const;

    // The global filling a supplied role, or null - which is the ordinary case for a program that
    // never asks about its command line.
    DeclPtr<Global> suppliedGlobal(intrinsic::Builtin role) const;

    // The slot a role's claim goes in. Total - every `RoleKind` is one of the three arrays above,
    // which is what `RoleKind` being three and not five is for. It does not report a claim already
    // taken, because the caller has both locations and this has neither.
    RoleClaim& claim(intrinsic::RoleRef role);
};

} // namespace mir
