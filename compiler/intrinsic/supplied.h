#pragma once

#include "role.h"
#include "../compiler/settings.h"

namespace intrinsic {

/*
 * Which way the fact travels, which is the whole of what distinguishes the two kinds of role.
 *
 * `Written` is storage the *program* reads and the *compiler* fills: the declaration is `&`, its
 * initializer is the value a build where nothing filled it would see, and what actually lands there
 * is written by the entry sequence at run time. A backend that emits no store for one is not a bug -
 * a global nothing in the program reaches is never emitted at all, and there is then nowhere to
 * store to and nothing that could tell the difference.
 *
 * `Supplied` is the other direction: the declaration is an ordinary immutable global, and the
 * compiler replaces what it holds with a number that depends on the target. The initializer is
 * required to be the zero of its type, because a written number that the compiler overwrites is a
 * line of source that lies to whoever reads it. Everything downstream then treats it as what it is,
 * an immutable global with a constant initializer, which folds into its readers - so the constant
 * costs a load nowhere.
 */
enum class BuiltinKind: U8 {
    Written,
    Supplied,
};

/*
 * What a declaration claiming a role has to be, checked against the type resolve gave it.
 *
 * Deliberately coarse. The point is to catch `@builtin(commandLineValues)` on a `Bool` - which would
 * have the backend store an address into one byte of storage - rather than to restate the library's
 * signatures here, which would put the thing this directory removes back in a different spelling.
 * `%%(%%U8)` and `%%U8` are both addresses as far as a store of a register is concerned, and which
 * one the library wants is the library's to say.
 */
enum class BuiltinShape: U8 {
    Word,    // An integer the width of a machine word: `Size`, `USize`.
    Address, // A pointer of any pointee type.

    /*
     * An enumeration the library declares - a `data` whose constructors all carry nothing.
     *
     * Coarse for this shape's reason too: which enumeration, and which constructor each number is,
     * are the library's to say and are said with `@value`. What the compiler checks is that the
     * declaration is one a number can be stored into at all, and what it stores is the number
     * `builtinValue` answers - so a library that renumbered its constructors would be a library
     * that changed what the role means, which is what `@value` is written for.
     */
    Enum,
};

/*
 * The globals the compiler fills - `supplied.def`.
 *
 * Keeps the name `Builtin` rather than becoming `Supplied`, because it is the name every consumer
 * already uses and the enum travels a long way: resolve fills it, lower carries it, and both native
 * backends read it. Renaming it would have been churn in four directories for no fact gained.
 */
enum class Builtin: U8 {
#define YANA_SUPPLIED_ROLE(role, kind, shape) role,
#include "supplied.def"
#undef YANA_SUPPLIED_ROLE

    Count,
};

constexpr Size kBuiltinCount = Size(Builtin::Count);

/*
 * How many of them are the command line's, which is the one thing about this order another stage
 * relies on: the LLVM backend stores `main`'s parameter *i* into role *i*, so the three have to be
 * the first three and have to be in `argc`, `argv`, `envp` order. Stated here so that a role added
 * to the table cannot quietly become a fourth parameter of `main`.
 */
constexpr Size kCommandLineGlobals = 3;

struct BuiltinDef {
    // The attribute's spelling - the enumerator's own name. See `YANA_ROLE_TEXT`.
    StringView role;

    BuiltinKind kind;
    BuiltinShape shape;
};

extern const BuiltinDef builtinTable[kBuiltinCount];

inline const BuiltinDef& builtinDef(Builtin which) { return builtinTable[Size(which)]; }

// Every supplied role name, comma-separated. Narrower than `roleList()` on purpose: a stage that
// only reads `@builtin` on globals should say what it can actually accept there.
Tritium::String suppliedRoleList();

/*
 * What the compiler supplies for a `Supplied` role on this target, or nothing where this target has
 * no answer - which makes claiming the role on that target an error rather than a silent zero.
 *
 * Nothing for every `Written` role: those are filled at run time, and there is no compile-time value
 * for one to be.
 */
Maybe<U64> builtinValue(const CompileSettings& settings, Builtin which);

} // namespace intrinsic
