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
void writeHover(Net::JsonWriter& json, Session& session, LocationWriter& locations,
                StringId module, U32 offset);
void writeReferences(Net::JsonWriter& json, Session& session, LocationWriter& locations,
                     StringId module, U32 offset, bool includeDeclaration);
void writeSemanticTokens(Net::JsonWriter& json, Session& session, StringId module,
                         StringView text, const LineTable& lines, bool utf16);

/// The hover text for whatever is at a position, as the markdown a client renders. Exposed on its
/// own because the fixtures assert on it directly - see test/lsp.
void describeAt(Session& session, StringId module, U32 offset, StringBuilder& into);

} // namespace lsp
