#pragma once

#include "index.h"

/*
 * Completion - Implementation-Tooling.md §8.
 *
 * The hardest of the editor features, because it is the only one that has to work on text that is
 * not a program: the name being completed is by definition half-written, and the declaration it is
 * in usually does not parse. The mechanism is a *cursor sentinel* rather than a second code path -
 * the parser writes a distinguished name where the cursor is (`Context::cursor`, §8.2), and the
 * ordinary resolver reaches it in the ordinary way, with the enclosing scope, the expected type and
 * the receiver of a `.` all already in hand.
 *
 * So nothing here re-derives anything. What is under the cursor comes from the parser, what is in
 * scope comes from `ExprResolver::bindings` and `forEachVisible`, and what a candidate *is* comes
 * from `resolve/index.h`'s Symbol - the same structure hover and go-to-definition read, so a
 * completion item and the hover the client shows beside it cannot describe a name differently.
 */

/*
 * One candidate.
 *
 * A Symbol plus the two things ranking needs. `type` is what the name produces where it is used -
 * a function's result, a binding's own type - and is what `expected` is compared against; it is
 * null for the kinds that have no such type, which is what makes those sort after everything else
 * rather than being filtered out.
 */
struct CompletionItem {
    Symbol symbol;
    TypePtr type = nullptr;

    // The import alias this name has to be written under, or zero when it may be written alone.
    // A `qualified` import is the only thing that sets one.
    StringId qualifier = 0;

    /*
     * Which group this item belongs to, lowest first: what the position asked for, then this body's
     * bindings, then this module's declarations, then everything reached through an import.
     *
     * A rank rather than a measure. LSP clients sort by `sortText` and filter by the typed prefix,
     * so what a server owes them is a coarse grouping they will keep, not a similarity score they
     * will immediately re-order. See the `Rank` enum in complete.cpp.
     */
    U8 rank = 0;
};

/*
 * What one completion request asked and what the compile answered.
 *
 * Owned by whoever made the request - a language server, or the test driver - and hung on the
 * Context beside `Context::cursor`, which is the whole of how the resolver knows a compile is
 * answering one. Null in every ordinary compile.
 */
struct CompletionRequest {
    /*
     * Filled by the resolver when it reaches the sentinel.
     */

    bool captured = false;

    // The module and function the cursor is in. `function` is null for a cursor the resolver
    // reached outside any body, which nothing produces yet but which costs nothing to allow.
    Module* module = nullptr;
    ModulePtr<Function> function = nullptr;

    /*
     * The type this position is being asked for, which is what makes the answer ranked rather than
     * alphabetical: `let x: Maybe(Int) = ` should offer `Just` and `Nothing` first. Available for
     * free, because the resolver already computes it to convert against.
     */
    TypePtr expected = nullptr;

    /*
     * The type of the value before the `.`, for §8.1's third kind. Null for a cursor that is not in
     * a field position, which is what tells the two apart: a member list is *only* the members,
     * since a name in field position can be nothing else.
     */
    TypePtr receiver = nullptr;
    bool member = false;

    /*
     * The record or tuple being *written*, for §8.1's fourth kind - `Square {si|`.
     *
     * Told apart from `receiver` because the two ask opposite questions: a member list is what a
     * finished value has, and this is what an unfinished one still needs. `construct` is set for
     * both of its positions and `member` for neither, so a caller can tell a field list from a
     * member list without comparing types.
     */
    TypePtr constructed = nullptr;
    bool construct = false;

    // Set for a cursor in a *pattern*, where `expected` is the pivot being matched and the items
    // are constructors and nothing else - §8.1's fifth kind.
    bool pattern = false;

    Array<CompletionItem> items;
};

/*
 * Records everything visible at the cursor - a name in expression position, a callee, the field of
 * a `.`, and an ascription. The other three positions have their own entry points below, and each
 * of them is a *different set of candidates* rather than a different way of collecting the same
 * one, which is what keeps "what may be written here" in one function per position.
 *
 * `receiver` is the type of what precedes a `.`, and null everywhere else.
 */
void captureCompletion(ExprResolver& resolver, TypePtr expected, TypePtr receiver, bool member);

/*
 * §8.1's fourth kind: the fields of the record or tuple the cursor is inside the braces of.
 *
 * `owner` is the type whose fields these are - the record for `Square {si|`, the tuple for a bare
 * `{si|` with an expected type - and `content` is the tuple holding them, which for a constructor
 * is its payload rather than the record itself.
 *
 * `namesOnly` is the difference between the two positions a cursor can be in there. A cursor in a
 * field *name* (`Square {si|: 3}`) can be nothing but a field, so nothing else is offered; a cursor
 * in a bare argument (`Square {si|`) has not said yet which of the two it is - a positional
 * constructor takes a value there - so the names in scope come too, ranked below the fields.
 */
void captureConstructionCompletion(ExprResolver& resolver, TypePtr owner, TypePtr content, bool namesOnly);

/*
 * A field of an update path - `{v | ori|gin: p}`.
 *
 * The members of what the path has reached, which is `.`'s answer, *and* a field being written,
 * which is a construction's - so it is the two halves of §8.1's third and fourth kinds meeting. The
 * insert text is what the difference is for: a member after a `.` is a projection and a field here
 * is half of a `field: value` pair.
 */
void captureUpdateCompletion(ExprResolver& resolver, TypePtr receiver);

/*
 * §8.1's fifth kind: a constructor in a pattern.
 *
 * `pivot` is the type being matched, and it is what makes the answer ranked: the constructors of
 * the value in hand come first and every other visible one follows. Constructors and nothing else -
 * a pattern position holds a constructor, a literal, or a *new* name, and neither of the other two
 * is something anything here could offer.
 */
void capturePatternCompletion(ExprResolver& resolver, TypePtr pivot);

/// Whether a completion request is being answered and has not been answered yet. The test the
/// construction sites make before looking for a sentinel among their arguments, since walking them
/// is only free when there is no request at all.
bool wantsCompletion(Context& context);

/// Whether a name is the cursor sentinel. The one test the resolver makes; see cursorName().
bool isCursorSentinel(Context& context, StringId name);
