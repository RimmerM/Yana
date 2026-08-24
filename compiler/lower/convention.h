#pragma once

#include "../compiler/context.h"

/*
 * The calling conventions, and the one table that names them.
 *
 * Split out of `lower.h` so that the *resolver* can hold one: `@convention(clobber)` is read off a
 * declaration long before there is a lower IR, and `resolve/module.h` including the whole lower IR
 * to name a six-valued enum would be a dependency out of all proportion to the fact.
 *
 * There is one name table and every reader shares it, which is the point of the split rather than a
 * tidiness argument. The names appear in three places that have to agree: the `@convention`
 * attribute a program writes, the `<sysv>` a lower-IR function is *parsed* with, and the `<sysv>` a
 * lower-IR function is *printed* with. A fixture round-trips through all three, so a table per
 * reader is a table that can drift, and the drift would show up as a golden file that stopped
 * matching itself.
 */
enum class LowerCallType {
    // System V calling convention used on Linux, macOS, etc.
    Sysv,

    // Calling convention used for 64-bit Windows.
    Win64,

    // Calling convention for "simple" functions, which retains most caller registers.
    Simple,

    // Calling convention for "complex" functions, which gives most registers to the callee.
    Complex,

    // Calling convention that clobbers all registers.
    Clobber,

    // Calling convention for system calls.
    Syscall,

    // Must always be last.
    LastType = Syscall,
};

static constexpr LowerCallType kDefaultCallType = LowerCallType::Complex;

// In declaration order, which is what `callTypeForName` indexes by and what the lower-IR printer
// emits. `system` rather than `syscall` because that is the spelling the fixtures already use.
inline StringView nameForCallType(LowerCallType type) {
    switch(type) {
        case LowerCallType::Sysv:    return "sysv"_v;
        case LowerCallType::Win64:   return "win64"_v;
        case LowerCallType::Simple:  return "simple"_v;
        case LowerCallType::Complex: return "complex"_v;
        case LowerCallType::Clobber: return "clobber"_v;
        case LowerCallType::Syscall: return "system"_v;
    }

    return ""_v;
}

// The reverse, by hash, which is how both readers have a name: the lower parser reads an identifier
// token and the attribute reader reads a `@convention(x)` argument, and both arrive interned.
inline Maybe<LowerCallType> callTypeForName(StringId name) {
    for(Size i = 0; i <= (Size)LowerCallType::LastType; i++) {
        auto type = LowerCallType(i);
        if(Context::nameHash(nameForCallType(type)) == name) return Just(type);
    }

    return Nothing();
}

/*
 * `@x86_legacy_sse`, and the one place its name is written.
 *
 * Beside the convention table for the reason that table exists rather than because it is one. It is
 * not a convention and has nothing to do with the enum above; what it shares is the problem, and
 * exactly: the same string is read by three readers that have to agree - the attribute a program
 * writes, the marker a lower-IR function is *parsed* with, and the marker one is *printed* with -
 * and a fixture round-trips through all three, so a spelling per reader is a spelling that can
 * drift into a golden that stops matching itself.
 *
 * The lower IR spells it the same as the source rather than shortening it. It sits inside the same
 * angle brackets as the convention - `f<sysv, x86_legacy_sse>` - and one fact with one name across
 * every level it appears at is worth more there than four saved characters.
 */
inline StringView nameForLegacySse() { return "x86_legacy_sse"_v; }

// And `LowerFunction::foreignBoundary`, whose lower-IR spelling shares the same list - see the note
// on the field. Not an attribute a program writes: nothing in Yana source declares a boundary yet,
// so the only writers are `lowerProgram` and a fixture.
inline StringView nameForForeignBoundary() { return "foreign"_v; }

/*
 * Whether this convention names an ABI defined outside this compiler.
 *
 * True of the two real ABIs and of nothing else, which is a question about the *table* and is all
 * this answers. It is emphatically not "the other side of this boundary is a stranger" - see
 * `LowerFunction::foreignBoundary`, which is that question and is a separate fact, because a Yana
 * function may name System V for reasons that have nothing to do with being reachable from outside.
 */
inline bool conventionIsForeignAbi(LowerCallType type) {
    return type == LowerCallType::Sysv || type == LowerCallType::Win64;
}

/*
 * Whether a *program* may name this convention, which is a smaller set than the six.
 *
 * `system` is never a function's own convention - the kernel is the callee, and the convention
 * describes what a syscall leaves alone rather than how anything this compiler emits is entered.
 * See the note on it in codegen/x64/constraint.cpp.
 *
 * `simple` and `complex` are the compiler's own choice between two internal shapes, and which one a
 * function gets is a decision the backend is entitled to change. A program pinning one would be
 * writing down an implementation detail as though it were an interface, and the two things it might
 * mean by that - "enter this cheaply" and "this is an ABI" - are the two things the other four names
 * already say.
 *
 * So what is left is the one convention with a *semantic* promise the program can rely on -
 * `clobber` keeps nothing, which is what makes a vector-state reset at entry legal - and the two
 * foreign ABIs, which a `foreign` declaration will need in order to say which one it is. See
 * doc/spec/targets.md, where the absence of that is currently recorded as an open question.
 */
inline bool conventionWritableInSource(LowerCallType type) {
    return type == LowerCallType::Clobber
        || type == LowerCallType::Sysv
        || type == LowerCallType::Win64;
}
