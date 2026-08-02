#pragma once

#include "document.h"
#include "protocol.h"
#include "session.h"

namespace lsp {

/*
 * The four features one structure buys - Implementation-Tooling.md M6.
 *
 * Definition, hover, references and semantic tokens are all the same two lookups: a position
 * becomes a node through the position index (§2), and a node becomes a symbol through the semantic
 * index (§1). What differs between them is only what is written out afterwards, which is why they
 * are one file.
 *
 * Nothing here goes through JSON-RPC to be tested: `test/lsp` calls these, and the transport has
 * its own protocol test. That split is deliberate - the framing is Tritium's problem and the
 * answers are the compiler's.
 */

/*
 * Turning a location into something a client can point at.
 *
 * A location names a module and two byte offsets; a client wants a URI and two (line, character)
 * pairs. The conversion needs the module's *text*, and a request routinely answers with a location
 * in a file the client never opened - jumping to a definition in another module is the whole point -
 * so this holds one line table per file it has been asked about rather than reading the open
 * document's.
 */
struct LocationWriter {
    explicit LocationWriter(Session& session, bool utf16): session(session), utf16(utf16) {}

    /// Writes `{"uri":..,"range":{..}}` for one location, or nothing at all when the location is in
    /// a module with no file - Core and Native, which are compiled into the compiler. False then,
    /// so the caller can leave the entry out rather than emit a broken one.
    bool writeLocation(Net::JsonWriter& json, LocationId id);

    /// The range half alone, for the requests that carry a URI of their own.
    bool writeRange(Net::JsonWriter& json, LocationId id);

    /// The whole file as one range, for a symbol whose "definition" is a module rather than a
    /// declaration - which is what an `import` names.
    bool writeModuleLocation(Net::JsonWriter& json, StringId module);

    Session& session;
    bool utf16 = true;

private:
    struct FileLines {
        StringId module = 0;
        StringView text;
        String uri;
        LineTable lines;
    };

    FileLines* linesOf(StringId module);
    Array<Ptr<FileLines>> files;
};

/*
 * The semantic token legend, in the order the protocol numbers it.
 *
 * §11's table asks for distinctions the standard type list has no name for. Where the standard list
 * has a slot that means the same thing it is used - a constructor is an `enumMember`, a class
 * function is a `method` - and the three that are genuinely Yana's are modifiers rather than types,
 * because they say something *about* a binding rather than replacing what it is.
 */
enum class TokenType: U32 {
    Namespace,
    Type,
    Class,
    TypeParameter,
    Parameter,
    Variable,
    Property,
    EnumMember,
    Function,
    Method,

    Count,
};

enum class TokenModifier: U32 {
    Declaration = 1 << 0,
    Definition = 1 << 1,
    Readonly = 1 << 2,
    Static = 1 << 3,

    // Design.md's ownership conventions, which are written at the binding and are the point of
    // writing them - `&x` and `->x` should be visible without reading the declaration again.
    Borrowed = 1 << 4,
    Sunk = 1 << 5,

    // §11's `heapPlaced`: a binding whose `Local::storage` came out heap rather than stack. The
    // `explain` cliff, shown inline, and off by default in any sane theme.
    HeapPlaced = 1 << 6,
};

/// The two legend arrays, written into the `initialize` response.
void writeTokenLegend(Net::JsonWriter& json);

/*
 * The handlers. Each writes the `result` field's value and nothing else; the caller owns the
 * envelope, since a response and a `$/progress` notification carry the same body.
 */
void writeDefinition(Net::JsonWriter& json, Session& session, LocationWriter& locations,
                     StringId module, U32 offset);
void writeTypeDefinition(Net::JsonWriter& json, Session& session, LocationWriter& locations,
                         StringId module, U32 offset);
void writeHover(Net::JsonWriter& json, Session& session, LocationWriter& locations,
                StringId module, U32 offset);
void writeReferences(Net::JsonWriter& json, Session& session, LocationWriter& locations,
                     StringId module, U32 offset, bool includeDeclaration);
void writeSemanticTokens(Net::JsonWriter& json, Session& session, StringId module,
                         StringView text, const LineTable& lines, bool utf16);

/*
 * Inlay hints - §6's row, and the half of M9 that is not hover.
 *
 * `from` and `to` are the byte range the client asked about, which is the visible part of the
 * document plus a margin. Everything outside it is left out rather than sent and discarded.
 */
void writeInlayHints(Net::JsonWriter& json, Session& session, StringId module, StringView text,
                     const LineTable& lines, bool utf16, U32 from, U32 to);

/*
 * Document highlights - §6's row: the `references` answer restricted to one file.
 *
 * A separate request from `references` because it is asked constantly - a client sends one whenever
 * the caret stops moving - and answers with ranges in the current document only, so it needs no
 * URIs and no line table for any other file.
 */
void writeDocumentHighlights(Net::JsonWriter& json, Session& session, StringId module, U32 offset,
                             StringView text, const LineTable& lines, bool utf16);

/*
 * Document symbols - §6's row, out of the module's own declaration tables.
 *
 * The flat form rather than the hierarchical one: Yana's declarations do not nest, apart from a
 * class's and an instance's functions, and those are named after what declares them anyway.
 */
void writeDocumentSymbols(Net::JsonWriter& json, Session& session, LocationWriter& locations,
                          StringId module);

/*
 * Folding ranges - §6's row.
 *
 * From the document's own indentation rather than from the parser's `IndentLevel` stack, which is
 * what §6 proposed. The reason is the one §8 gives for the cursor sentinel: this has to answer on
 * text that is not a program, and a file mid-edit is exactly when its blocks are being written. The
 * rule is the lexer's own - a line is a block header when the line under it is indented further -
 * so the two agree wherever the file parses.
 */
void writeFoldingRanges(Net::JsonWriter& json, StringView text, const LineTable& lines);

/*
 * Completion - Implementation-Tooling.md §8.
 *
 * Unlike the four above, this one *compiles*: the cursor sentinel is a parse-time decision, so
 * answering means re-resolving the program with the cursor set. The session is left stale
 * afterwards - `Session::staleProgram()` - and the caller has to recompile before answering
 * anything else out of it.
 *
 * `text` is the document as the client has it, which is what the partial name under the cursor is
 * read out of. The compiler never sees that text; it reports where the name starts and the server
 * holds the buffer.
 *
 * `snippets` is the client's own `snippetSupport`, which decides whether an item that takes
 * arguments can insert them with the caret in the first one, and `utf16` the negotiated position
 * encoding - an item carries the range it replaces, so it counts characters the way the client does.
 */
void writeCompletion(Net::JsonWriter& json, Session& session, StringId module, U32 offset,
                     StringView text, bool snippets, bool utf16);

/*
 * Signature help - Implementation-Tooling.md §6's `signatureHelp` row.
 *
 * "The overload set at the enclosing call's LocationId, plus which argument the cursor is in", and
 * both halves come from things that already exist: the position index says which call node the
 * cursor is in, and `findFunction`/`findClassFunctions` are the same lookup a call site makes.
 *
 * Unlike completion this reads the ordinary compile - a call being typed still parses as a call,
 * which is what M7's recovery bought - so it costs a lookup rather than a compile.
 */
void writeSignatureHelp(Net::JsonWriter& json, Session& session, StringId module, U32 offset,
                        StringView text);

/// The item kind the protocol numbers a symbol as. Exposed for the fixtures, which assert on the
/// number a client would sort and icon by.
U32 completionItemKind(Symbol::Kind kind);

/// The hover text for whatever is at a position, as the markdown a client renders. Exposed on its
/// own because the fixtures assert on it directly - see test/lsp.
void describeAt(Session& session, StringId module, U32 offset, StringBuilder& into);

/// The `explain` section a hover carries under the signature, or false and nothing written where
/// every answer is the boring one - Implementation-Tooling.md M9. Exposed for the same reason.
bool explainAt(Session& session, StringId module, U32 offset, StringBuilder& into);

} // namespace lsp
