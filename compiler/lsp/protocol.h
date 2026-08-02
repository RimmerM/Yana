#pragma once

#include "json.h"
#include "Net/Stream.h"
#include "Net/Codec/Json.h"

namespace lsp {

/*
 * The bytes in and out.
 *
 * An interface rather than a call to `read` because the protocol test drives the same server over a
 * pair of pipes, and because §4.1's `-socket <port>` mode is the same loop over a different pair of
 * file descriptors.
 */
struct Transport {
    virtual ~Transport() = default;

    /// Reads up to `length` bytes, blocking until at least one arrives. Returns 0 at end of stream.
    virtual Size read(Byte* buffer, Size length) = 0;
    virtual void write(const Byte* buffer, Size length) = 0;
};

/// Standard input and standard output, which is how an editor launches a language server.
///
/// `output` is a descriptor rather than 1, because main takes the real standard output away before
/// anything can print to it - see the comment there. `input` is 0 and stays 0.
struct StdioTransport: Transport {
    int input = 0;
    int output = 1;

    Size read(Byte* buffer, Size length) override;
    void write(const Byte* buffer, Size length) override;
};

/*
 * `Content-Length` framing.
 *
 * The reader holds whatever arrived past the end of the message it returned, because a single read
 * routinely spans a message boundary - an editor writes a burst of notifications as one write.
 */
struct FrameReader {
    explicit FrameReader(Transport& transport): transport(transport) {}

    /// Reads one complete message body into `into`. False at end of stream, or when the header is
    /// not something this can frame - which is not recoverable, since the stream position is then
    /// unknown.
    bool read(Array<Byte>& into);

    Transport& transport;
    Array<Byte> pending;
};

/// Writes one framed message. Every writer goes through here and the caller holds a lock across it:
/// the reader thread answers `shutdown` while the worker may be publishing diagnostics, and two
/// interleaved bodies are a stream neither side can resynchronize.
void writeFrame(Transport& transport, Buffer<const Byte> body);

/*
 * `file:` URIs.
 *
 * The client speaks URIs and the compiler speaks paths, and the conversion is not just a prefix:
 * a path with a space in it arrives percent-encoded, and a Windows path arrives with a leading
 * slash before the drive letter that is not part of it.
 */
String uriToPath(StringView uri);
String pathToUri(StringView path);

/*
 * Response and notification bodies.
 *
 * Each opens a writer over memory, so the caller can frame the finished body - the header needs the
 * length, which is only known once the body exists.
 */
struct MessageWriter {
    MessageWriter(): writer(16384), json(writer) {}

    Net::Writer writer;
    Net::JsonWriter json;

    /// `{"jsonrpc":"2.0","id":<id>,` - leaves the object open for the result.
    void startResponse(const JsonValue* id);

    /// `{"jsonrpc":"2.0","method":<method>,"params":` - leaves the object open for the params.
    void startNotification(StringView method);

    /// Writes an id field for a value that is a number, a string, or absent - which JSON-RPC allows
    /// and clients do use.
    void writeId(const JsonValue* id);

    void endObject() { json.endObject(); }

    Buffer<const Byte> body();
};

/// The subset of JSON-RPC error codes this server can produce.
enum class ErrorCode: I32 {
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    InternalError = -32603,
    ServerNotInitialized = -32002,
    RequestCancelled = -32800,
    ContentModified = -32801,
};

void writeError(Transport& transport, const JsonValue* id, ErrorCode code, StringView message);

} // namespace lsp
