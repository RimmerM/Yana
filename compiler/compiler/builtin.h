#pragma once

#include "settings.h"

/*
 * The declarations the compiler itself has something to say about.
 *
 * A handful of the library's globals are not ordinary declarations. The compiler stores into one
 * before the program's first instruction runs, or decides what another holds at all - and either
 * direction needs the compiler and the library to be talking about the same declaration.
 *
 * The way that used to be arranged was a name written out in the backend: `commandLineCount` spelled
 * in two code generators, matching a `pub let` in `lib/Native/Linux.x64.yana` that nothing checked.
 * Renaming the library's global compiled cleanly and produced a program whose arguments were
 * silently zero, on both backends, with no diagnostic anywhere. That is the failure this file
 * exists to make impossible.
 *
 * The link is written in the source instead, as an attribute:
 *
 *     @builtin(commandLineCount)
 *     pub let &commandLineCount = 0 :: Size
 *
 * The argument is the **role**, which is a name in the table below and nothing else. The
 * declaration's own name, its module, its `pub` and its file are the library's business and may all
 * change without the compiler noticing; what the compiler will not do is fail to notice a role that
 * is not in this table, or two declarations claiming one role, or a role claimed by a declaration of
 * the wrong shape. All three are reported where they are written - see `readBuiltinAttribute`.
 *
 * **It lives in the compiler rather than beside a code generator** because it is not a code
 * generator's fact. Resolve reads the attribute, lower carries the answer across, and both native
 * backends consume it: `codegen/x64/emit.cpp` writes the command line into the three globals from
 * the initial stack pointer, and `codegen/llvm/emit.cpp` stores the same three out of `main`'s
 * parameters. A table inside either one of them would be the other one's copy, which is what the
 * first version of this was.
 */

/*
 * Every role, and the order is the only thing about it that matters twice: `Program::builtins` and
 * `LowerModule::builtins` are arrays indexed by it, and the LLVM backend reads the command-line
 * three as `main`'s parameters 0, 1 and 2 - so those three stay in `argc`, `argv`, `envp` order.
 */
enum class Builtin: U8 {
    // What the process was started with - Design-Test.md §11.2's F4. Written by the entry sequence
    // before anything else runs; `lib/Environment` is what reads them afterwards.
    commandLineCount,
    commandLineValues,
    commandLineEnvironment,

    /*
     * The target's mapping granularity, as a compile-time constant.
     *
     * **No library declaration claims this one yet**, and that is deliberate: whether a page is a
     * number the compiler states or one the kernel is asked for at startup is a decision about the
     * library rather than about this table. The role is here because the second kind of link has to
     * be reachable from somewhere to be worth having, and because this is the candidate it was
     * written for - `lib/Native/Linux.x64.yana`'s `pageBytes` and `lib/Native/Heap.native.yana`'s
     * `heapGuardSize` are the same 4096 written twice, and the second one is in a file that is
     * selected on `native` and therefore has no platform to be right about.
     */
    pageBytes,
};

constexpr Size kBuiltinCount = 4;

/*
 * How many of them are the command line's, which is the one thing about this order another stage
 * relies on: the LLVM backend stores `main`'s parameter *i* into role *i*, so the three have to be
 * the first three and have to be in `argc`, `argv`, `envp` order. Stated here so that a role added
 * to the enum cannot quietly become a fourth parameter of `main`.
 */
constexpr Size kCommandLineGlobals = 3;

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
 * Deliberately coarse. The point is to catch `@builtin(commandLineValues)` on a `Bool` - which
 * would have the backend store an address into one byte of storage - rather than to restate the
 * library's signatures here, which would put the thing this file removes back in a different
 * spelling. `Ptr(Ptr(U8))` and `Ptr(U8)` are both addresses as far as a store of a register is
 * concerned, and which one the library wants is the library's to say.
 */
enum class BuiltinShape: U8 {
    Word,    // An integer the width of a machine word: `Size`, `USize`.
    Address, // A pointer of any pointee type.
};

struct BuiltinDef {
    // What `@builtin(...)` names. Not the library's name for the declaration, which is free.
    StringView role;

    BuiltinKind kind;
    BuiltinShape shape;
};

extern const BuiltinDef builtinTable[kBuiltinCount];

inline const BuiltinDef& builtinDef(Builtin which) { return builtinTable[Size(which)]; }

// The role that name spells, or nothing where the table has none - which is what a misspelled
// `@builtin(commandlineCount)` is, and is reported rather than ignored.
Maybe<Builtin> findBuiltin(StringView name);

// Every role name, comma-separated, for the diagnostic that reports an unknown one. Built rather
// than written out, so that adding a row to the table adds it to the message.
Tritium::String builtinRoleList();

/*
 * What the compiler supplies for a `Supplied` role on this target, or nothing where this target has
 * no answer - which makes claiming the role on that target an error rather than a silent zero.
 *
 * Nothing for every `Written` role: those are filled at run time, and there is no compile-time value
 * for one to be.
 */
Maybe<U64> builtinValue(const CompileSettings& settings, Builtin which);
