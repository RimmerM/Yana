#pragma once

#include "index.h"

namespace mir {

/*
 * Completion - Implementation-Tooling.md §8, for this stage.
 *
 * The hardest of the editor features, because it is the only one that has to work on text that is not
 * a program: the name being completed is by definition half-written. The mechanism is a *cursor
 * sentinel* rather than a second code path - the parser writes a distinguished name where the cursor
 * is (`Context::cursor`, §8.2), and the ordinary resolver reaches it in the ordinary way, with the
 * enclosing scope, the expected type and the receiver of a `.` already in hand.
 *
 * So nothing here re-derives anything. What is under the cursor comes from the parser, what is in
 * scope from `Resolver::bindings` and the module's imports, and what a candidate *is* from `index.h`'s
 * `Symbol` - the same structure hover reads, so a completion item and the hover beside it cannot
 * describe one name differently. The captures themselves are `Resolver` members, in `complete.cpp`,
 * because the scope they read is the resolver's own.
 */

/*
 * One candidate: a symbol, and the two things ranking needs.
 *
 * `type` is what the name produces where it is used - a function's result, a binding's own type - and
 * is what the position's expected type is compared against. Null for the kinds that produce no value,
 * which sorts them after everything else rather than filtering them out.
 */
struct CompletionItem {
    Symbol symbol;
    DeclPtr<Type> type = nullptr;

    // The import alias this name has to be written under, or zero - a `qualified` import is the only
    // thing that sets one.
    StringId qualifier {};

    /*
     * Which group this item is in, lowest first: what the position asked for, this body's bindings,
     * this module's declarations, and everything reached through an import.
     *
     * A rank rather than a measure. A client sorts by `sortText` and filters by the typed prefix, so
     * what a server owes it is a coarse grouping it will keep.
     */
    U8 rank = 0;

    /*
     * Whether choosing this item writes the *name* half of a `name: value` pair - a parameter of the
     * call the cursor is inside. Per item, because an argument position offers the parameter names
     * and the names in scope together, and only the first of those takes a colon after it.
     */
    bool naming = false;
};

/*
 * What one completion request asked and what the compile answered.
 *
 * Owned by whoever made the request - a language server, or the test driver - and hung on the
 * `Context` beside `Context::cursor`, which is the whole of how the resolver knows a compile is
 * answering one. Null in every ordinary compile.
 */
struct CompletionRequest {
    // Filled when the resolver reaches the sentinel. One answer per compile: a body reached twice
    // would reach the same sentinel twice.
    bool captured = false;

    DeclPtr<mir::Module> module = nullptr;
    DeclPtr<Function> function = nullptr;

    // The type the position is being asked for, which is what makes the answer ranked rather than
    // alphabetical: `let x: Maybe(Int) = ` offers `Just` and `Nothing` first.
    DeclPtr<Type> expected = nullptr;

    // The type before the `.`, for §8.1's third kind. A member list is *only* the members.
    DeclPtr<Type> receiver = nullptr;
    bool member = false;

    // The record or tuple being *written*, for §8.1's fourth kind - and set for an update path too,
    // whose fields are half of a `field: value` pair just as a construction's are.
    DeclPtr<Type> constructed = nullptr;
    bool construct = false;

    // A namespace selection such as `X86.`. Unlike a member selection it has no receiver occurrence;
    // the LSP layer uses this bit to recover the qualifier when the cursor is in the empty gap after
    // the dot.
    bool qualified = false;

    // A cursor in a pattern, where the items are constructors and nothing else - §8.1's fifth kind.
    bool pattern = false;

    Array<CompletionItem> items;
};

/// Whether a completion request is being answered and has not been answered yet.
inline bool wantsCompletion(Context& context) {
    return context.completion && !context.completion->captured;
}

/// Whether a name is the cursor sentinel - the one test the resolver makes; see `cursorName`.
inline bool isCursorSentinel(Context& context, StringId name) {
    return context.completion && name && name == cursorName(context);
}

} // namespace mir
