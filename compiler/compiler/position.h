#pragma once

#include "context.h"

/*
 * "What is under the cursor", and the arithmetic an editor needs to ask it.
 *
 * Implementation-Tooling.md §2. Nothing here is a new analysis: `Context::locations` already holds
 * every node's range, in the order the parser created them and indexed by nothing. This turns that
 * into something a position can be looked up in.
 */

/*
 * One module's locations, ordered so that a byte offset can be searched for.
 *
 * Sorted by start ascending and end descending, which puts an enclosing range immediately before
 * everything it encloses. The innermost node at an offset is then the *last* entry at or before it
 * that still contains it, which is a binary search and a short walk back.
 */
struct PositionIndex {
    struct Entry {
        U32 start;
        U32 end;

        // The largest `end` of any entry at or before this one. What bounds the walk: once the
        // best any earlier entry could reach is at or before the offset, no earlier entry can
        // contain it and the walk stops. Without it, an offset in the whitespace between two
        // declarations walks back over every node of every declaration before it.
        U32 maxEnd;

        LocationId location;
    };

    /// Collects every location belonging to `module`. Cheap enough to redo per compile: it is one
    /// pass over an array the parser filled in anyway.
    void build(const Context& context, StringId module);

    /// The innermost recorded node containing `offset`, or kNullLocation. An offset in whitespace
    /// belongs to whatever encloses the whitespace, which is usually a declaration and sometimes
    /// nothing at all.
    LocationId find(U32 offset) const;

    /// Every node containing `offset`, innermost first. What a request needing the *context* of a
    /// position uses - the enclosing call for signature help, the enclosing declaration for
    /// completion - since the innermost node alone rarely answers those.
    void findEnclosing(U32 offset, Array<LocationId>& into) const;

    /// True when the locations arrived already ordered, which they do when nothing but the lexer
    /// created them - it only moves forward. Recorded rather than asserted because a location built
    /// after the fact is legitimate and only costs the sort.
    bool wasOrdered = true;

    StringId module {};
    Array<Entry> entries;
};

/*
 * Where the lines are.
 *
 * The server holds the document text, so this is the only thing needed to turn `Loc::offset` into
 * the (line, character) pair a client speaks - and, going the other way, a client's position into
 * an offset. `Loc::line` and `Loc::column` deliberately take no part: the column the lexer produces
 * counts bytes with tabs expanded to a tab stop, which is not a character count in any encoding a
 * client can name. See §2.1.
 */
struct LineTable {
    /// One entry per line, holding the offset of its first byte. Line 0 starts at 0 always, so the
    /// table is never empty even for empty text.
    Array<U32> lineStarts;

    void build(StringView text);

    /// The 0-based line an offset is on.
    U32 lineOf(U32 offset) const;

    U32 lineCount() const { return lineStarts.size(); }
    U32 lineStart(U32 line) const;

    /// The 0-based UTF-16 code unit index of `offset` within its line, which is what an LSP client
    /// means by `character` unless it negotiated otherwise. Needs the text, because how many code
    /// units a byte range is depends on what is in it.
    U32 utf16Column(StringView text, U32 offset) const;

    /// The byte offset of a client's (line, character). `utf16` selects whether `character` counts
    /// UTF-16 code units or bytes - the two agree on ASCII and on nothing else. Clamped to the line
    /// and to the text, because a client may name a position in a version of the document that this
    /// one no longer matches.
    U32 offsetAt(StringView text, U32 line, U32 character, bool utf16) const;
};
