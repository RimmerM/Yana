#pragma once

#include "json.h"
#include "../compiler/position.h"

namespace lsp {

/*
 * One file as the editor has it.
 *
 * The text is here rather than only in the OverlayProvider because this is the copy positions are
 * computed against - the line table has to match the text a client's `character` was counted in,
 * and the provider's copy is one the compiler owns and re-reads.
 */
struct Document {
    String uri;
    String path;
    String text;
    I32 version = 0;

    LineTable lines;

    /// Replaces the whole text.
    void setText(String text);

    /// Applies one incremental change. `utf16` says how the client counts a character; both ends of
    /// the range are clamped, because a client can name a position in a version this is not.
    void applyChange(U32 startLine, U32 startCharacter, U32 endLine, U32 endCharacter,
                     StringView replacement, bool utf16);

    /// The byte offset of a client's position in this document.
    U32 offsetAt(U32 line, U32 character, bool utf16) const;
};

struct DocumentStore {
    Document* find(StringView uri);
    Document& open(StringView uri, String path, String text, I32 version);
    void close(StringView uri);

    Array<Document> documents;
};

} // namespace lsp
