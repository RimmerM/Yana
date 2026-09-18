#pragma once

#include "../compiler/context.h"

/*
 * The calling conventions, and the one table that names them.
 *
 * Split out of `lower.h` so that the *resolver* can hold one: `@convention(clobber)` is read off a
 * declaration long before there is a lower IR, and `resolve/module.h` including the whole lower IR
 * to name an eight-valued enum would be a dependency out of all proportion to the fact.
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

    /*
     * The stack switch - Analysis-External-Iterators.md §4.1's `stack.switch`, and the one
     * "convention" here whose callee is not code at all.
     *
     * It is a convention rather than an instruction's own operand table because of what it has to
     * say about registers, which is exactly what a convention says and nothing a form can: **the
     * other context runs here**, so everything the allocator could have handed out is gone across
     * it. A form states that as `clobbers`, and a form's clobbers land *in front of* its operands -
     * an operand read out of its own register has to dodge all of them, which for a set this size
     * leaves nowhere to read one from. A call's clobbers land behind its operands, because the
     * callee has not run when they are read, and that is the shape a switch has.
     *
     * That is also the whole of the efficiency argument against porting Tritium's helper. A
     * `Tritium_suspendFiber` saves seven callee-saved registers and restores seven on every switch
     * whether or not anything was in them; this tells the allocator that nothing survives and lets
     * it spill exactly what is live, which in a small producer loop is one or two words.
     *
     * The arguments and the result are `Complex`'s tables - `(target, save, transfer)` in the first
     * three argument registers, the incoming transfer word in the first result register - so a
     * switch written in the library lands its operands where the sequence already wants them.
     */
    StackSwitch,

    /*
     * An x86 interrupt handler. The processor supplies its frame on the entry stack rather than
     * through a normal call argument list; the native backend therefore gives the handler a pointer
     * to that frame and, for exception handlers, the error-code word. Its return is `iretq`.
     */
    X86Interrupt,

    // Must always be last.
    LastType = X86Interrupt,
};

static constexpr LowerCallType kDefaultCallType = LowerCallType::Complex;

/*
 * How many registers of each bank a convention hands to arguments, and how many to results.
 *
 * Stated here rather than read off the x64 tables because two tiers need the same numbers and only
 * one of them links the backend: `resolve_ssa/lower` decides which by-value records travel as their
 * fields, and that decision is only worth anything if every field it sends lands in a register. The
 * tables in `codegen/x64/constraint.cpp` are checked against this (`checkBudget`), so a table that
 * grows or shrinks without the budget is a failed assertion rather than a record quietly spilling to
 * the stack.
 */
struct RegisterBudget {
    U8 gpr = 0;
    U8 vector = 0;
};

struct ConventionBudget {
    RegisterBudget args;
    RegisterBudget results;
};

/*
 * `complex`'s budget: every caller-saved general register and the twelve low vector registers, for
 * arguments and for results alike.
 *
 * Nine general registers are the whole caller-saved half - `rax` joined the argument list last, where
 * it was the one volatile register nothing could pass a value in. Twelve vector registers rather than
 * sixteen because xmm12-15 are callee-saved, which is `test/bench/callconv/findings.md`'s robust
 * compromise: a callee that needs twelve or fewer pays nothing, and a caller keeps four values
 * across a call without spilling them.
 */
inline ConventionBudget complexBudget() {
    return ConventionBudget { RegisterBudget { 9, 12 }, RegisterBudget { 9, 12 } };
}

// In declaration order, which is what `callTypeForName` indexes by and what the lower-IR printer
// emits. `system` rather than `syscall` because that is the spelling the fixtures already use.
inline StringView nameForCallType(LowerCallType type) {
    switch(type) {
        case LowerCallType::Sysv:        return "sysv"_v;
        case LowerCallType::Win64:       return "win64"_v;
        case LowerCallType::Simple:      return "simple"_v;
        case LowerCallType::Complex:     return "complex"_v;
        case LowerCallType::Clobber:     return "clobber"_v;
        case LowerCallType::Syscall:     return "system"_v;
        case LowerCallType::StackSwitch: return "stackswitch"_v;
        case LowerCallType::X86Interrupt:return "x86_interrupt"_v;
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
 * Whether operand zero of a call under this convention is the thing being called.
 *
 * Two conventions answer no and they answer it for the same reason: there is no address in the
 * operand list to call. A system call names the kernel by a number, and a stack switch names a
 * saved stack pointer - what runs next is whatever that stack's top word says, which is not a
 * callee any tier of this compiler has a name for.
 *
 * A predicate rather than the `== Syscall` test that was written at each of the four sites, because
 * the second convention with the property is what turns a comparison into a category.
 */
inline bool callTypeHasCallee(LowerCallType type) {
    return type != LowerCallType::Syscall && type != LowerCallType::StackSwitch;
}

/*
 * Whether this convention names an ABI defined outside this compiler.
 *
 * True of the three real ABIs and of nothing else, which is a question about the *table* and is all
 * this answers. It is emphatically not "the other side of this boundary is a stranger" - see
 * `LowerFunction::foreignBoundary`, which is that question and is a separate fact, because a Yana
 * function may name System V for reasons that have nothing to do with being reachable from outside.
 */
inline bool conventionIsForeignAbi(LowerCallType type) {
    return type == LowerCallType::Sysv
        || type == LowerCallType::Win64
        || type == LowerCallType::X86Interrupt;
}

/*
 * Whether a *program* may name this convention, which is a smaller set than the eight.
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
 * `clobber` keeps nothing, which is what makes a vector-state reset at entry legal - and the three
 * foreign ABIs, which a `foreign` declaration will need in order to say which one it is. See
 * doc/spec/targets.md, where the absence of that is currently recorded as an open question.
 */
inline bool conventionWritableInSource(LowerCallType type) {
    return type == LowerCallType::Clobber
        || type == LowerCallType::Sysv
        || type == LowerCallType::Win64
        || type == LowerCallType::X86Interrupt;
}
