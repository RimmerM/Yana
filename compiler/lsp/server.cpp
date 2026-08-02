#include "server.h"

namespace lsp {

// How long the typing has to stop before a compile starts. §5.3: the whole program is re-resolved
// per edit, so the debounce is what keeps that off the keystroke path.
static const Int kDebounceMs = 200;

/*
 * Writing.
 */

void Server::send(MessageWriter& message) {
    MutexLock lock(writeLock);
    writeFrame(transport, message.body());
}

void Server::sendError(const JsonValue* id, ErrorCode code, StringView text) {
    MutexLock lock(writeLock);
    writeError(transport, id, code, text);
}

void Server::showMessage(I32 type, StringView text) {
    MessageWriter out;
    out.startNotification("window/showMessage"_v);
    out.json.field("params"_v).startObject();
    out.json.field("type"_v).value(type);
    out.json.field("message"_v).value(text);
    out.json.endObject();
    out.endObject();

    send(out);
}

void Server::logMessage(I32 type, StringView text) {
    MessageWriter out;
    out.startNotification("window/logMessage"_v);
    out.json.field("params"_v).startObject();
    out.json.field("type"_v).value(type);
    out.json.field("message"_v).value(text);
    out.json.endObject();
    out.endObject();

    send(out);
}

/*
 * The reader thread.
 */

int Server::run() {
    worker.start();

    Array<Byte> body;
    while(frames.read(body)) {
        auto message = new JsonDocument();
        if(!message->parse(Buffer<Byte> { body.size() ? &body[0] : nullptr, body.size() })) {
            sendError(nullptr, ErrorCode::ParseError, stringView(message->error));
            delete message;
            continue;
        }

        auto method = find(message->root, "method"_v);
        auto name = method ? method->asString() : StringView {};
        auto id = find(message->root, "id"_v);

        // The three that must be answerable while a compile is in flight. Everything else is
        // queued, which is also what keeps requests in the order the client sent them.
        if(name == "exit"_v) {
            delete message;
            break;
        }

        if(name == "shutdown"_v) {
            MessageWriter out;
            out.startResponse(id);
            out.json.field("result"_v).null();
            out.endObject();
            send(out);

            {
                MutexLock lock(queueLock);
                shutdownRequested = true;
            }

            delete message;
            continue;
        }

        if(name == "$/cancelRequest"_v) {
            if(auto cancelledId = find(find(message->root, "params"_v), "id"_v)) {
                MutexLock lock(queueLock);
                cancelled.push(cancelledId->asInt(-1));
            }

            delete message;
            continue;
        }

        MutexLock lock(queueLock);
        queue.push(message);
        queued.set();
    }

    {
        MutexLock lock(queueLock);
        stopping = true;
    }

    queued.set();
    worker.wait();

    return shutdownRequested ? 0 : 1;
}

void Worker::run() {
    while(true) {
        JsonDocument* message = nullptr;

        {
            MutexLock lock(server.queueLock);
            if(server.queue.size()) {
                message = server.queue[0];
                server.queue.remove(0);
            } else if(server.stopping) {
                break;
            }
        }

        if(message) {
            server.handle(*message);
            delete message;
            continue;
        }

        if(server.dirty) {
            // A keystroke arriving inside the window sets the signal, so the wait returns early and
            // the window starts again. The compile happens only once the typing has stopped, which
            // is the whole of the debounce.
            if(!server.queued.wait(kDebounceMs)) {
                server.refresh();
                server.dirty = false;
            }
        } else {
            server.queued.wait(Int(Signal::WaitForever));
        }
    }
}

bool Server::isCancelled(const JsonValue* id) {
    if(!id || id->kind != JsonValue::Number) return false;

    MutexLock lock(queueLock);
    for(U32 i = 0; i < cancelled.size(); i++) {
        if(cancelled[i] == I64(id->number)) {
            cancelled.remove(i);
            return true;
        }
    }

    return false;
}

/*
 * Dispatch, on the worker thread.
 */

void Server::handle(JsonDocument& message) {
    auto method = find(message.root, "method"_v);
    if(!method) return; // A response to something this server sent; nothing asks for any yet.

    auto name = method->asString();
    auto id = find(message.root, "id"_v);

    if(id) {
        handleRequest(*message.root, name, id);
    } else {
        handleNotification(*message.root, name);
    }
}

void Server::handleRequest(const JsonValue& message, StringView method, const JsonValue* id) {
    if(isCancelled(id)) {
        sendError(id, ErrorCode::RequestCancelled, "cancelled"_v);
        return;
    }

    if(method == "initialize"_v) {
        onInitialize(message, id);
        return;
    }

    if(!initialized) {
        sendError(id, ErrorCode::ServerNotInitialized, "the server has not been initialized"_v);
        return;
    }

    /*
     * Completion compiles for itself - the cursor sentinel is a parse-time decision - so it is
     * dispatched ahead of the refresh below rather than made to pay for a compile it is about to
     * replace. What it leaves behind is what `staleProgram()` answers for.
     *
     * The one thing it still needs a compile for is the *position*: a request names a document and
     * a line, and turning that into a module and a byte offset goes through the module map and the
     * context the last compile left. Completion as the first request after `didOpen` - a `.` typed
     * inside the debounce, which is exactly when an editor asks - therefore found no context and
     * answered null, having never reached the sentinel at all.
     */
    if(method == "textDocument/completion"_v) {
        if(!session.context) {
            refresh();
            dirty = false;
        }

        onCompletion(message, id);
        return;
    }

    /*
     * The compile has to have happened before any of these can answer.
     *
     * A client asks for hover the moment the mouse stops, which is routinely inside the debounce -
     * so the request runs the pending compile itself rather than answering out of a stale program
     * or out of none. It is the same work the debounce was about to do, done a little early.
     *
     * `staleProgram()` is the same condition arriving from the other direction: a completion request
     * left the session holding a program with a sentinel in it, and one name there resolves to
     * nothing. Recompiling is what a keystroke would have done anyway, and a completion request is
     * followed by more typing.
     */
    if(dirty || session.staleProgram()) {
        refresh();
        dirty = false;
    }

    if(method == "textDocument/definition"_v) onDefinition(message, id);
    else if(method == "textDocument/typeDefinition"_v) onTypeDefinition(message, id);
    else if(method == "textDocument/declaration"_v) onDefinition(message, id);
    else if(method == "textDocument/hover"_v) onHover(message, id);
    else if(method == "textDocument/references"_v) onReferences(message, id);
    else if(method == "textDocument/semanticTokens/full"_v) onSemanticTokens(message, id);
    else if(method == "textDocument/signatureHelp"_v) onSignatureHelp(message, id);
    else if(method == "textDocument/inlayHint"_v) onInlayHint(message, id);
    else if(method == "textDocument/documentHighlight"_v) onDocumentHighlight(message, id);
    else if(method == "textDocument/documentSymbol"_v) onDocumentSymbol(message, id);
    else if(method == "textDocument/foldingRange"_v) onFoldingRange(message, id);
    else {
        // Everything else is a later milestone. Answering "method not found" is what tells the
        // client to stop asking, which is better than a silence it waits out.
        sendError(id, ErrorCode::MethodNotFound, method);
    }
}

void Server::handleNotification(const JsonValue& message, StringView method) {
    auto params = message.find("params"_v);
    if(!params) return;

    if(method == "textDocument/didOpen"_v) onDidOpen(*params);
    else if(method == "textDocument/didChange"_v) onDidChange(*params);
    else if(method == "textDocument/didClose"_v) onDidClose(*params);
    else if(method == "textDocument/didSave"_v) onDidSave(*params);
}

/*
 * Lifecycle.
 */

void Server::onInitialize(const JsonValue& message, const JsonValue* id) {
    auto params = message.find("params"_v);

    // Byte offsets are what the compiler has, and asking for them first is what avoids converting
    // every position twice - see §2.1. A client that does not offer utf-8 gets utf-16, which is
    // the specification's default and what the conversion in LineTable exists for.
    utf16Positions = true;
    if(auto encodings = find(find(params, "capabilities"_v), "general"_v)) {
        if(auto list = encodings->find("positionEncodings"_v)) {
            for(auto value = list->first; value; value = value->next) {
                if(value->asString() == "utf-8"_v) utf16Positions = false;
            }
        }
    }

    // Whether an item that takes arguments may insert them with the caret in the first one - §8.
    // False unless the client says otherwise, which is the specification's default and the safe way
    // round: a client that got a snippet it cannot expand would show the placeholder syntax as text.
    completionSnippets = false;
    if(auto completion = find(find(find(params, "capabilities"_v), "textDocument"_v), "completion"_v)) {
        if(auto item = completion->find("completionItem"_v)) {
            completionSnippets = find(item, "snippetSupport"_v)->asBool(false);
        }
    }

    MessageWriter out;
    out.startResponse(id);
    out.json.field("result"_v).startObject();

    out.json.field("capabilities"_v).startObject();
    out.json.field("positionEncoding"_v).value(utf16Positions ? "utf-16"_v : "utf-8"_v);

    out.json.field("textDocumentSync"_v).startObject();
    out.json.field("openClose"_v).value(true);
    out.json.field("change"_v).value(U32(2)); // Incremental.
    out.json.field("save"_v).startObject();
    out.json.field("includeText"_v).value(false);
    out.json.endObject();
    out.json.endObject();

    // M6. Four features out of one side table - Implementation-Tooling.md §1.
    out.json.field("definitionProvider"_v).value(true);
    out.json.field("declarationProvider"_v).value(true);
    out.json.field("typeDefinitionProvider"_v).value(true);
    out.json.field("hoverProvider"_v).value(true);
    out.json.field("referencesProvider"_v).value(true);

    // §6's remaining editor-facing rows, none of which needed anything the compile does not
    // already leave behind. The inlay hints are M9's other half - `explain` above a declaration,
    // and the type of a binding nobody wrote one for.
    out.json.field("inlayHintProvider"_v).value(true);
    out.json.field("documentHighlightProvider"_v).value(true);
    out.json.field("documentSymbolProvider"_v).value(true);
    out.json.field("foldingRangeProvider"_v).value(true);

    // M8 - Implementation-Tooling.md §8. `.` is the one trigger character: every other position
    // starts with an identifier character, which a client asks about on its own.
    out.json.field("completionProvider"_v).startObject();
    out.json.field("triggerCharacters"_v).startArray();
    out.json.arrayField().value("."_v);
    out.json.endArray();
    out.json.endObject();

    // The brackets a call is written with are what open and separate its arguments, so they are
    // what should bring the signature back - and `)` is what should take it away, which a client
    // does for itself once it knows the call ended. `{` because a record is constructed with one
    // and its fields are its arguments.
    out.json.field("signatureHelpProvider"_v).startObject();
    out.json.field("triggerCharacters"_v).startArray();
    out.json.arrayField().value("("_v);
    out.json.arrayField().value("{"_v);
    out.json.arrayField().value(","_v);
    out.json.endArray();
    out.json.field("retriggerCharacters"_v).startArray();
    out.json.arrayField().value(","_v);
    out.json.endArray();
    out.json.endObject();

    out.json.field("semanticTokensProvider"_v).startObject();
    out.json.field("legend"_v).startObject();
    writeTokenLegend(out.json);
    out.json.endObject();

    // Whole-file only. A delta needs the previous answer kept per document, and the file being
    // edited is the one file whose tokens are cheapest to rebuild - they come off an array that
    // already exists.
    out.json.field("full"_v).value(true);
    out.json.endObject();

    out.json.endObject(); // capabilities

    out.json.field("serverInfo"_v).startObject();
    out.json.field("name"_v).value("yana-lsp"_v);
    out.json.field("version"_v).value("0.1"_v);
    out.json.endObject();

    out.json.endObject(); // result
    out.endObject();
    send(out);

    initialized = true;

    // The project root, in the three places a client may put it. `workspaceFolders` is the current
    // one; `rootUri` and `rootPath` are deprecated and still what several clients send.
    String root;
    if(auto folders = find(params, "workspaceFolders"_v)) {
        if(auto first = folders->at(0)) {
            if(auto uri = first->find("uri"_v)) root = uriToPath(uri->asString());
        }
    }

    if(root == "") {
        if(auto uri = find(params, "rootUri"_v)) {
            if(uri->kind == JsonValue::String) root = uriToPath(uri->string);
        }
    }

    if(root == "") {
        if(auto path = find(params, "rootPath"_v)) {
            if(path->kind == JsonValue::String) root = ownedString(path->string.ptr, path->string.length);
        }
    }

    auto opened = session.open(stringView(root));
    if(opened.isErr()) {
        // Named rather than logged. §10: half of all "the plugin does not work" reports for an
        // LSP-backed plugin are a server that failed silently, and this is the failure that
        // happens - a project that has no yana.toml yet.
        showMessage(1, stringView(format("Yana: %@", opened.unwrapErr())));
        return;
    }

    logMessage(3, stringView(format("Yana: %@ modules from %@", session.moduleMap.entries.size(),
                                    session.projectPath)));

    // What was negotiated, said out loud. Both of these change what an answer looks like rather
    // than whether there is one - an item that inserts its brackets needs `snippetSupport`, and
    // every range in every answer is counted in the encoding - so a client that declined one is
    // indistinguishable from a server bug unless the log says which happened.
    logMessage(3, stringView(format("Yana: positions in %@, completion snippets %@",
                                    utf16Positions ? "utf-16" : "utf-8",
                                    completionSnippets ? "on" : "off")));
    dirty = true;
}

/*
 * Documents.
 */

static StringView documentUri(const JsonValue& params) {
    if(auto document = params.find("textDocument"_v)) {
        if(auto uri = document->find("uri"_v)) return uri->asString();
    }

    return {};
}

void Server::onDidOpen(const JsonValue& params) {
    auto document = params.find("textDocument"_v);
    if(!document) return;

    auto uri = document->find("uri"_v);
    auto text = document->find("text"_v);
    if(!uri || !text) return;

    auto path = uriToPath(uri->asString());
    auto version = I32(find(document, "version"_v)->asInt(0));

    auto& opened = documents.open(uri->asString(), path, ownedString(text->string.ptr, text->string.length), version);

    // A file the map has never heard of is one that was created since the map was built. Rescanning
    // is cheap next to resolving, and the alternative is an editor that shows nothing for a new
    // file until the server is restarted.
    if(!session.provider.setDocument(stringView(opened.path), opened.text, version)) {
        if(session.isOpen()) {
            auto rescanned = session.rescan();
            if(rescanned.isOk()) session.provider.setDocument(stringView(opened.path), opened.text, version);
        }
    }

    dirty = true;
}

void Server::onDidChange(const JsonValue& params) {
    auto uri = documentUri(params);
    auto document = documents.find(uri);
    if(!document) return;

    if(auto version = find(params.find("textDocument"_v), "version"_v)) {
        document->version = I32(version->asInt(document->version));
    }

    if(auto changes = params.find("contentChanges"_v)) {
        for(auto change = changes->first; change; change = change->next) {
            auto text = change->find("text"_v);
            if(!text) continue;

            auto range = change->find("range"_v);
            if(!range) {
                // No range means the whole document, which is what a client sends when it declined
                // incremental sync - and what every client sends for the first change after a
                // resynchronization.
                document->setText(ownedString(text->string.ptr, text->string.length));
                continue;
            }

            auto start = range->find("start"_v);
            auto end = range->find("end"_v);
            if(!start || !end) continue;

            document->applyChange(
                U32(find(start, "line"_v)->asInt(0)), U32(find(start, "character"_v)->asInt(0)),
                U32(find(end, "line"_v)->asInt(0)), U32(find(end, "character"_v)->asInt(0)),
                text->asString(), utf16Positions
            );
        }
    }

    session.provider.setDocument(stringView(document->path), document->text, document->version);
    dirty = true;
}

void Server::onDidClose(const JsonValue& params) {
    auto uri = documentUri(params);
    if(auto document = documents.find(uri)) {
        session.provider.clearDocument(stringView(document->path));
    }

    documents.close(uri);
    dirty = true;
}

void Server::onDidSave(const JsonValue&) {
    // The buffer and the file agree again. Nothing changes for this server, since the overlay is
    // what it reads either way - but anything generated beside the source may have, so it compiles.
    dirty = true;
}


/*
 * The request map - Implementation-Tooling.md §6, and the features of §1.
 */

bool Server::resolvePosition(const JsonValue& params, StringId& module, U32& offset, Document*& document) {
    module = 0;
    offset = 0;
    document = nullptr;

    if(!session.isOpen() || !session.context) return false;

    auto uri = documentUri(params);
    if(uri.length == 0) return false;

    auto path = uriToPath(uri);
    auto entry = session.findEntry(stringView(path));
    if(!entry || !entry->name) return false;

    module = entry->name;

    auto position = params.find("position"_v);
    if(!position) return false;

    auto line = U32(find(position, "line"_v)->asInt(0));
    auto character = U32(find(position, "character"_v)->asInt(0));

    /*
     * The open document's own line table when there is one, and the compiled text otherwise.
     *
     * They are the same text - the overlay is what the compile read - but only the document has a
     * table already built, and going through the provider for a file that is open would build a
     * second one per keystroke.
     */
    document = documents.find(uri);
    if(document) {
        offset = document->offsetAt(line, character, utf16Positions);
        return true;
    }

    auto text = session.provider.getSource(module);
    LineTable lines;
    lines.build(text);
    offset = lines.offsetAt(text, line, character, utf16Positions);
    return true;
}

void Server::onDefinition(const JsonValue& message, const JsonValue* id) {
    auto params = message.find("params"_v);

    StringId module = 0;
    U32 offset = 0;
    Document* document = nullptr;

    MessageWriter out;
    out.startResponse(id);
    out.json.field("result"_v);

    if(!params || !resolvePosition(*params, module, offset, document)) {
        out.json.null();
    } else {
        LocationWriter locations(session, utf16Positions);
        writeDefinition(out.json, session, locations, module, offset);
    }

    out.endObject();
    send(out);
}

void Server::onHover(const JsonValue& message, const JsonValue* id) {
    auto params = message.find("params"_v);

    StringId module = 0;
    U32 offset = 0;
    Document* document = nullptr;

    MessageWriter out;
    out.startResponse(id);
    out.json.field("result"_v);

    if(!params || !resolvePosition(*params, module, offset, document)) {
        out.json.null();
    } else {
        LocationWriter locations(session, utf16Positions);
        writeHover(out.json, session, locations, module, offset);
    }

    out.endObject();
    send(out);
}

void Server::onReferences(const JsonValue& message, const JsonValue* id) {
    auto params = message.find("params"_v);

    StringId module = 0;
    U32 offset = 0;
    Document* document = nullptr;

    auto includeDeclaration = true;
    if(auto context = find(params, "context"_v)) {
        includeDeclaration = find(context, "includeDeclaration"_v)->asBool(true);
    }

    MessageWriter out;
    out.startResponse(id);
    out.json.field("result"_v);

    if(!params || !resolvePosition(*params, module, offset, document)) {
        out.json.startArray().endArray();
    } else {
        LocationWriter locations(session, utf16Positions);
        writeReferences(out.json, session, locations, module, offset, includeDeclaration);
    }

    out.endObject();
    send(out);
}

void Server::onCompletion(const JsonValue& message, const JsonValue* id) {
    auto params = message.find("params"_v);

    StringId module = 0;
    U32 offset = 0;
    Document* document = nullptr;

    MessageWriter out;
    out.startResponse(id);
    out.json.field("result"_v);

    if(!params || !resolvePosition(*params, module, offset, document)) {
        out.json.null();
    } else {
        // The open document's text, or nothing - which makes writeCompletion read the file the
        // compile it is about to run loaded, since a view of that one taken now would not survive it.
        writeCompletion(out.json, session, module, offset,
                        document ? stringView(document->text) : StringView {}, completionSnippets,
                        utf16Positions);
    }

    out.endObject();
    send(out);
}

void Server::onSignatureHelp(const JsonValue& message, const JsonValue* id) {
    auto params = message.find("params"_v);

    StringId module = 0;
    U32 offset = 0;
    Document* document = nullptr;

    MessageWriter out;
    out.startResponse(id);
    out.json.field("result"_v);

    if(!params || !resolvePosition(*params, module, offset, document)) {
        out.json.null();
    } else if(document) {
        writeSignatureHelp(out.json, session, module, offset, stringView(document->text));
    } else {
        writeSignatureHelp(out.json, session, module, offset, session.provider.getSource(module));
    }

    out.endObject();
    send(out);
}

/*
 * A whole-file request's document, which is the open buffer's text where there is one and the
 * compiled file's otherwise. The line table comes with it: a request that answers in ranges needs
 * one, and an open document already has it built.
 */
Server::FileRequest Server::resolveFile(const JsonValue& params) {
    FileRequest request;

    auto uri = documentUri(params);
    auto path = uriToPath(uri);
    auto entry = session.isOpen() ? session.findEntry(stringView(path)) : nullptr;
    if(!entry || !entry->name) return request;

    request.module = entry->name;
    request.found = true;

    if(auto document = documents.find(uri)) {
        request.text = stringView(document->text);
        request.lines.build(request.text);
    } else {
        request.text = session.provider.getSource(entry->name);
        request.lines.build(request.text);
    }

    return request;
}

void Server::onSemanticTokens(const JsonValue& message, const JsonValue* id) {
    auto params = message.find("params"_v);

    MessageWriter out;
    out.startResponse(id);
    out.json.field("result"_v);

    auto file = params ? resolveFile(*params) : FileRequest {};

    if(!file.found || !session.context) {
        out.json.null();
    } else {
        writeSemanticTokens(out.json, session, file.module, file.text, file.lines, utf16Positions);
    }

    out.endObject();
    send(out);
}

void Server::onTypeDefinition(const JsonValue& message, const JsonValue* id) {
    auto params = message.find("params"_v);

    StringId module = 0;
    U32 offset = 0;
    Document* document = nullptr;

    MessageWriter out;
    out.startResponse(id);
    out.json.field("result"_v);

    if(!params || !resolvePosition(*params, module, offset, document)) {
        out.json.null();
    } else {
        LocationWriter locations(session, utf16Positions);
        writeTypeDefinition(out.json, session, locations, module, offset);
    }

    out.endObject();
    send(out);
}

void Server::onDocumentHighlight(const JsonValue& message, const JsonValue* id) {
    auto params = message.find("params"_v);

    StringId module = 0;
    U32 offset = 0;
    Document* document = nullptr;

    MessageWriter out;
    out.startResponse(id);
    out.json.field("result"_v);

    auto file = params ? resolveFile(*params) : FileRequest {};

    if(!params || !file.found || !resolvePosition(*params, module, offset, document)) {
        out.json.startArray().endArray();
    } else {
        writeDocumentHighlights(out.json, session, module, offset, file.text, file.lines, utf16Positions);
    }

    out.endObject();
    send(out);
}

void Server::onDocumentSymbol(const JsonValue& message, const JsonValue* id) {
    auto params = message.find("params"_v);

    MessageWriter out;
    out.startResponse(id);
    out.json.field("result"_v);

    auto file = params ? resolveFile(*params) : FileRequest {};

    if(!file.found || !session.context) {
        out.json.startArray().endArray();
    } else {
        LocationWriter locations(session, utf16Positions);
        writeDocumentSymbols(out.json, session, locations, file.module);
    }

    out.endObject();
    send(out);
}

void Server::onFoldingRange(const JsonValue& message, const JsonValue* id) {
    auto params = message.find("params"_v);

    MessageWriter out;
    out.startResponse(id);
    out.json.field("result"_v);

    auto file = params ? resolveFile(*params) : FileRequest {};

    if(!file.found) {
        out.json.startArray().endArray();
    } else {
        // The one answer here that needs no compile at all - folding is the document's own
        // indentation, which is exactly why it keeps working while the file does not parse.
        writeFoldingRanges(out.json, file.text, file.lines);
    }

    out.endObject();
    send(out);
}

void Server::onInlayHint(const JsonValue& message, const JsonValue* id) {
    auto params = message.find("params"_v);

    MessageWriter out;
    out.startResponse(id);
    out.json.field("result"_v);

    auto file = params ? resolveFile(*params) : FileRequest {};

    if(!file.found || !session.context) {
        out.json.startArray().endArray();
        out.endObject();
        send(out);
        return;
    }

    // The visible range, which is what the client asks about rather than the whole file. Absent
    // from a client that asks for everything, and the whole document is then the range.
    U32 from = 0;
    U32 to = U32(file.text.length);

    if(auto range = params->find("range"_v)) {
        auto start = range->find("start"_v);
        auto end = range->find("end"_v);

        if(start && end) {
            from = file.lines.offsetAt(file.text, U32(find(start, "line"_v)->asInt(0)),
                                       U32(find(start, "character"_v)->asInt(0)), utf16Positions);
            to = file.lines.offsetAt(file.text, U32(find(end, "line"_v)->asInt(0)),
                                     U32(find(end, "character"_v)->asInt(0)), utf16Positions);
        }
    }

    writeInlayHints(out.json, session, file.module, file.text, file.lines, utf16Positions, from, to);
    out.endObject();
    send(out);
}

/*
 * Diagnostics.
 */

void Server::refresh() {
    if(!session.isOpen()) return;

    session.compile();
    publishDiagnostics();
    idle.set();
}

// Everything one file needs to turn byte offsets into client positions. Built per publish rather
// than kept, because the text a compile ran against is the text this has to agree with.
struct FileDiagnostics {
    StringId module = 0;
    String uri;
    StringView text;
    LineTable lines;
    Array<const Diagnostic*> messages;
};

void Server::publishDiagnostics() {
    Array<FileDiagnostics> files;
    Array<const Diagnostic*> unplaceable;

    for(auto& message: session.diagnostics.messages) {
        if(!message.hasLocation || message.where.sourceModule == 0) {
            unplaceable.push(&message);
            continue;
        }

        auto entry = session.moduleMap.find(message.where.sourceModule);
        if(!entry) {
            // A location in Core or Native, which are compiled into the compiler and have no file
            // to point at. A diagnostic there is a compiler bug rather than a program error, and
            // attaching it to one of the user's files would say the opposite.
            unplaceable.push(&message);
            continue;
        }

        FileDiagnostics* file = nullptr;
        for(auto& candidate: files) {
            if(candidate.module == message.where.sourceModule) { file = &candidate; break; }
        }

        if(!file) {
            files.push(FileDiagnostics {});
            file = &files[files.size() - 1];
            file->module = message.where.sourceModule;
            file->uri = pathToUri(entry->path);
            file->text = session.provider.getSource(message.where.sourceModule);
            file->lines.build(file->text);
        }

        file->messages.push(&message);
    }

    // A client keeps what it was last told about a URI, so a file whose last error was just fixed
    // has to be published as empty. This is the whole reason the previous set is remembered.
    for(auto& previous: publishedUris) {
        auto stillReported = false;
        for(auto& file: files) {
            if(file.uri == previous) { stillReported = true; break; }
        }

        if(stillReported) continue;

        MessageWriter out;
        out.startNotification("textDocument/publishDiagnostics"_v);
        out.json.field("params"_v).startObject();
        out.json.field("uri"_v).value(previous);
        out.json.field("diagnostics"_v).startArray().endArray();
        out.json.endObject();
        out.endObject();
        send(out);
    }

    publishedUris.clear();

    for(auto& file: files) {
        MessageWriter out;
        out.startNotification("textDocument/publishDiagnostics"_v);
        out.json.field("params"_v).startObject();
        out.json.field("uri"_v).value(file.uri);
        out.json.field("diagnostics"_v).startArray();

        for(auto message: file.messages) {
            auto startLine = file.lines.lineOf(message->where.sourceStart.offset);
            auto endLine = file.lines.lineOf(message->where.sourceEnd.offset);

            auto column = [&](U32 line, U32 offset) {
                return utf16Positions
                    ? file.lines.utf16Column(file.text, offset)
                    : offset - file.lines.lineStart(line);
            };

            out.json.arrayField().startObject();
            out.json.field("range"_v).startObject();
            out.json.field("start"_v).startObject();
            out.json.field("line"_v).value(startLine);
            out.json.field("character"_v).value(column(startLine, message->where.sourceStart.offset));
            out.json.endObject();
            out.json.field("end"_v).startObject();
            out.json.field("line"_v).value(endLine);
            out.json.field("character"_v).value(column(endLine, message->where.sourceEnd.offset));
            out.json.endObject();
            out.json.endObject();

            I32 severity = 1;
            if(message->level == Diagnostics::WarningLevel) severity = 2;
            else if(message->level == Diagnostics::MessageLevel) severity = 3;

            out.json.field("severity"_v).value(severity);
            out.json.field("source"_v).value("yana"_v);
            out.json.field("message"_v).value(message->text);
            out.json.endObject();
        }

        out.json.endArray();
        out.json.endObject();
        out.endObject();
        send(out);

        publishedUris.push(file.uri);
    }

    for(auto message: unplaceable) {
        showMessage(message->level == Diagnostics::ErrorLevel ? 1 : 2, stringView(message->text));
    }
}

} // namespace lsp
