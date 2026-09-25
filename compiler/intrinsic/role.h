#pragma once

#include <Core.h>

using namespace Tritium;

/*
 * Everything the compiler and the source language have to agree about, in one directory.
 *
 * A compiler that ships a standard library is coupled to it in five distinct ways, and before this
 * existed each of the five was arranged differently and four of them were unchecked. The failure
 * that made the case for `@builtin` on a global - a renamed `commandLineCount` compiling cleanly
 * into a program whose arguments were silently zero - is the same failure the other four could
 * have, and `Analysis-Builtins.md` §2.4 is where that was written down and deferred. This is the
 * closing of it.
 *
 * ## The five directions
 *
 *  1. **The compiler creates it and the source names it.** `I32`, `Bool`, `{}`: types with no
 *     declaration anywhere, answered by name when a signature spells one. `primitive.def`.
 *  2. **The source declares it and the compiler references it.** `class Drop(a)`, `fn
 *     checkCondition`, `data Outcome`. The declaration is complete; what the compiler needs is a
 *     handle on it. `known.def`.
 *  3. **The source declares it and the compiler implements it.** `pub fn *(it: %a) -> a` - a
 *     signature with no body, whose meaning is a node the compiler emits. `op.def`.
 *  4. **The source declares it and the compiler fills it.** A global the entry sequence writes, or
 *     one whose value the target decides. `supplied.def`.
 *  5. **The source declares the class and the compiler supplies instances of it.** `Num(I32)`, the
 *     conversion ladders. `provided.def`.
 *
 * ## The role is the enumerator's own name
 *
 * A declaration says which role it fills by writing `@builtin(role)`, and the spelling the attribute
 * accepts is the **enumerator stringified** - there is no separate name column for it to drift
 * from. Add a row to a `.def` and the attribute accepts it; rename the row and the old spelling
 * stops being accepted, which is a diagnostic at the declaration rather than a silent unfilled role.
 *
 * The first direction is the one exception and deliberately so: nothing claims a primitive, because
 * there is no declaration to write an attribute on. Its names are in `primitive.def` for the same
 * reason the rest are here - they are source-visible strings - but they are looked up as *types* and
 * are not part of the role namespace.
 *
 * ## Why a top-level directory
 *
 * The tables are read by more than one stage. `resolve_mir` binds roles, `lower` carries the
 * supplied globals across, both native backends fill them, and the JavaScript pipeline names the
 * same host operations. A table inside any one of those is the others' copy - which is exactly what
 * `compiler/compiler/builtin.h` was becoming, with two code generators reading a role enum out of
 * the resolver's neighbourhood.
 *
 * What is **not** here is any emitter. What a role expands to is a question about one stage's IR -
 * a MIR node, an SSA value, a JavaScript expression - so each consumer implements the roles it
 * cares about in its own `intrinsic/` subdirectory, indexed by the enumerators below. Those
 * implementations hold no names at all, which is the property this whole arrangement exists to buy.
 */
namespace intrinsic {

// The spelling `@builtin(...)` accepts for a role - the enumerator's own name, and the only place a
// role's text comes from. `sizeof` of a string literal counts the terminator, hence the minus one.
#define YANA_ROLE_TEXT(role) StringView { #role, sizeof(#role) - 1 }

/*
 * The attribute's own name.
 *
 * Here rather than at each reader, because there is more than one: `resolve_mir` binds roles by it
 * and prints it back, and the day this is spelled differently they have to move together. It was
 * written out three times before, which is the same failure in miniature as the one the roles
 * themselves exist to close.
 */
constexpr StringView kBuiltinAttribute = YANA_ROLE_TEXT(builtin);

/*
 * The name a program's entry point is written with.
 *
 * Here for the reason the attribute is: it is a spelling the language fixes, and one written out at
 * each reader drifts. While `compiler/resolve` existed (it was deleted in J173) both resolve stages
 * looked this name up in the main module and each spelled it, so a language that renamed its entry
 * point would have compiled with two stages disagreeing about which function starts the program.
 *
 * **Not a role**, and it could not be one. The other four directions are couplings to a *library*
 * declaration, which is why each is claimed by an attribute the library writes; this is a coupling
 * to a declaration in the user's own program, and there is nothing for `@builtin` to be written on.
 * `doc/spec/modules.md` is where the language states it, and this is where the compiler agrees.
 */
constexpr StringView kEntryPointName = YANA_ROLE_TEXT(main);

/*
 * The module every compilation implicitly imports, and the file a library directory is judged by.
 *
 * Two places agree about this name: the library locator, which tests a candidate directory for
 * `Core/Core.yana`, and the resolver, which seeds the module before it walks a single import. Each
 * used to spell it - and so did `compiler/resolve`, until J173 deleted it - and a library whose
 * root module were renamed would have moved one of them and left the others looking for something
 * that is no longer there.
 *
 * It is the whole of what `LibrarySource::directory` judges a candidate directory by, which is what
 * makes a standard library a *directory* rather than a mode.
 */
constexpr StringView kCoreModuleName = YANA_ROLE_TEXT(Core);

/*
 * The one operator spelling the compiler holds, and it is a lexical fact rather than a role.
 *
 * `-1` is a written negative number and not a negation of `1`: the lexer produces the magnitude
 * alone, and `doc/spec/patterns.md` says the sign "is part of the pattern rather than the operator
 * it is in an expression" - so folding it into the literal is what makes `I64`'s own minimum
 * writable at all, since negating the magnitude after the fact is signed overflow. Every other
 * prefix operator, this one included at a non-literal operand, is a call by name; this is the one
 * place the compiler has to recognize the token before it looks anything up.
 */
constexpr StringView kNegationOperator = StringView { "-", 1 };

/*
 * Which table a role is in, and therefore what claiming it means.
 *
 * Carried on `RoleRef` rather than derived from the enumerator, because the three dense enums below
 * are three independent numberings: `Known(0)` and `Op(0)` are both zero and are different roles.
 * The pair is the identity.
 *
 * Two of the five directions are absent, and for one reason: **a role is a kind only if a
 * declaration can claim it**. `primitive.def` has nothing to write an attribute on, and a
 * `provided.def` row is keyed off the `Known` role naming its class rather than claimed in its own
 * right - so neither is in the role namespace, and `@builtin` cannot name either.
 */
enum class RoleKind: U8 {
    // A declaration the compiler references - it has a body, or is a class, record or constructor
    // the compiler has to be able to name. `known.def`.
    Known,

    // A signature with no body, which the compiler implements. `op.def`.
    Op,

    // A global whose contents the compiler writes or decides. `supplied.def`.
    Supplied,
};

/*
 * One role, as the attribute reader hands it on.
 *
 * The index is into the table `kind` names, so a reader switches once and then subscripts - which is
 * what keeps a per-kind dense enum usable in a `switch` after a lookup that had to span all three.
 */
struct RoleRef {
    RoleKind kind = RoleKind::Known;
    U16 index = 0;

    bool operator == (RoleRef other) const { return kind == other.kind && index == other.index; }
};

/*
 * The role a written name selects, or nothing where no table has it.
 *
 * One namespace across all three role tables, which is not a convenience: `@builtin(copy)` has to
 * mean one thing, and two tables free to use one spelling would make which of them answered depend
 * on the order this searches. `verifyRoleTable` asserts there are no duplicates rather than leaving
 * that to whoever adds the second one.
 */
Maybe<RoleRef> findRole(StringView name);

// Every role name, comma-separated, for the diagnostic that reports an unknown one. Built from the
// tables rather than written out, so a new row is in the message the moment it exists.
Tritium::String roleList();

/*
 * Asserts that no two rows anywhere share a spelling.
 *
 * Called once from the compiler's start-up in an assertion build. It cannot be a `static_assert` -
 * the names are string comparisons across three independently included tables - and it must not be a
 * runtime check in a release build, because a duplicate is a mistake in this directory rather than
 * anything a program can provoke.
 */
void verifyRoleTable();

/*
 * What kind of declaration may claim a role.
 *
 * Checked where the attribute is read, so `@builtin(drop)` on a `fn` is reported at the attribute
 * naming both what it is on and what the role wants - rather than filling a class slot with a
 * function handle and failing somewhere in the ownership walk.
 */
enum class Entity: U8 {
    Class,
    Record,
    Constructor,
    Function,
    Global,
};

StringView entityName(Entity entity);

/*
 * What happens when nothing claims a role.
 *
 * A column rather than a rule each consumer applies, because the three answers are genuinely
 * different and each is right somewhere:
 *
 *  - `Required` - the compiler cannot proceed without it. A library that does not declare it is
 *    broken, and that is said once, at the end of the declaration passes.
 *  - `Optional` - the compiler copes. `checkConditionAt` is only reached under `-check-locations`,
 *    and a role no program's build asks for costs nothing by being absent.
 *
 * There was a third, `Synthesized`, and a standard library is what removed it. It meant "the compiler
 * makes one", and what it was for was a stage that had none: `class Drop(a)` was built in C++ so
 * that a program declaring no instance could still be classified. A library the stage can compile
 * answers the same need with a declaration, so nothing here has to describe a class that exists
 * without one.
 */
enum class Presence: U8 {
    Required,
    Optional,
};

} // namespace intrinsic
