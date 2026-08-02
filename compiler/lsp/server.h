#pragma once

#include "protocol.h"
#include "document.h"
#include "feature.h"
#include "session.h"
#include <Thread/Thread.h>
#include <Thread/Mutex.h>
#include <Thread/Signal.h>

namespace lsp {

struct Server;

/*
 * The worker.
 *
 * Owns the `Context`, the `Program` and every document, and runs one compile at a time. The
 * compiler is not thread-safe - the regions, the arenas, the interning tables and the program's
 * scratch pools are all single-owner - and making it so would be a large change to buy nothing
 * here, since there is one program and one question at a time. §4.1.
 */
struct Worker: Thread {
    explicit Worker(Server& server): server(server) {}
    Server& server;

private:
    void run() override;
};

/*
 * The message loop.
 *
 * Two threads. The reader parses frames and answers `shutdown`, `exit` and `$/cancelRequest`
 * itself, because those have to be answerable while a compile is in flight; everything else is
 * queued for the worker in the order it arrived.
 */
struct Server {
    explicit Server(Transport& transport): transport(transport), frames(transport), worker(*this) {}

    /// Runs until the client closes the stream or sends `exit`. The process exit code, which the
    /// specification defines: zero after a `shutdown`, one for an `exit` without one.
    int run();

    /*
     * The reader thread's half.
     */

    Transport& transport;
    FrameReader frames;

    /*
     * The queue between the two.
     */

    Mutex queueLock;
    Signal queued { false, true };
    Array<JsonDocument*> queue;

    /// Ids the client has cancelled. Set by the reader while the worker may be running, which is
    /// the whole reason cancellation is not just a queue message.
    Array<I64> cancelled;

    bool stopping = false;

    /*
     * The worker thread's half. Nothing here is touched by the reader.
     */

    Worker worker;
    Session session;
    DocumentStore documents;

    /// True from the first `didOpen` or `didChange` until the debounce expires and the compile runs.
    bool dirty = false;
    bool initialized = false;
    bool shutdownRequested = false;

    /// Whether the client counts a `character` in UTF-16 code units. Negotiated in `initialize`;
    /// see §2.1 - the server would rather hand out byte offsets, and asks for that first.
    bool utf16Positions = true;

    /// Whether the client expands a completion item's snippet, which decides whether an item that
    /// takes arguments may put the caret in the first of them - §8.3. Negotiated in `initialize`.
    bool completionSnippets = false;

    /// Files a diagnostic was last published for, so the ones that no longer have any can be
    /// cleared. A client keeps what it was last told about a URI until it is told otherwise.
    Array<String> publishedUris;

    /*
     * Writing. Both threads write, so every frame goes out under this.
     */

    Mutex writeLock;
    void send(MessageWriter& message);
    void sendError(const JsonValue* id, ErrorCode code, StringView text);
    void showMessage(I32 type, StringView text);
    void logMessage(I32 type, StringView text);

    /*
     * Handlers, all on the worker thread.
     */

    void handle(JsonDocument& message);
    void handleRequest(const JsonValue& message, StringView method, const JsonValue* id);
    void handleNotification(const JsonValue& message, StringView method);

    void onInitialize(const JsonValue& message, const JsonValue* id);

    /*
     * The four features one structure buys - M6.
     *
     * Each of them is the same two steps in front of a different writer, so what they share is
     * here: the document the client named, the module it belongs to, and the byte offset of the
     * position it asked about. A request for a file the project does not contain answers null
     * rather than an error, because that is a file the user opened and not a mistake the client
     * made.
     */
    bool resolvePosition(const JsonValue& params, StringId& module, U32& offset, Document*& document);

    void onDefinition(const JsonValue& message, const JsonValue* id);
    void onTypeDefinition(const JsonValue& message, const JsonValue* id);
    void onHover(const JsonValue& message, const JsonValue* id);
    void onReferences(const JsonValue& message, const JsonValue* id);
    void onSemanticTokens(const JsonValue& message, const JsonValue* id);
    void onCompletion(const JsonValue& message, const JsonValue* id);
    void onSignatureHelp(const JsonValue& message, const JsonValue* id);

    // §6's remaining editor-facing rows. Each of them reads what a compile already left, which is
    // why they are handlers rather than milestones - see feature.h.
    void onInlayHint(const JsonValue& message, const JsonValue* id);
    void onDocumentHighlight(const JsonValue& message, const JsonValue* id);
    void onDocumentSymbol(const JsonValue& message, const JsonValue* id);
    void onFoldingRange(const JsonValue& message, const JsonValue* id);

    /// The document a whole-file request names, with its text and line table - which is either the
    /// open buffer's or the compiled file's. Null when the file is not part of the project.
    struct FileRequest {
        StringId module = 0;
        StringView text;
        LineTable lines;
        bool found = false;
    };

    FileRequest resolveFile(const JsonValue& params);

    void onDidOpen(const JsonValue& params);
    void onDidChange(const JsonValue& params);
    void onDidClose(const JsonValue& params);
    void onDidSave(const JsonValue& params);

    /// Set every time a compile finishes and its diagnostics have been published. Nothing in the
    /// server waits on it; the protocol test does, so that it can assert on a result rather than on
    /// a sleep.
    Signal idle { false, true };

    /// Recompiles and publishes. The one thing the worker does that no message asked for directly.
    void refresh();
    void publishDiagnostics();

    bool isCancelled(const JsonValue* id);
};

} // namespace lsp
