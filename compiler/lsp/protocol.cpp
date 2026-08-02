#include "protocol.h"

#if __WINDOWS__
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#endif

namespace lsp {

/*
 * Transport.
 */

Size StdioTransport::read(Byte* buffer, Size length) {
#if __WINDOWS__
    auto count = ::_read(input, buffer, unsigned(length));
#else
    auto count = ::read(input, buffer, length);
#endif

    return count > 0 ? Size(count) : 0;
}

void StdioTransport::write(const Byte* buffer, Size length) {
    // Written in a loop, because a pipe accepts what fits in it and reports the rest as a short
    // write. A partial message is a stream the client cannot resynchronize, so this is not
    // something a return value can be left to say.
    Size written = 0;
    while(written < length) {
#if __WINDOWS__
        auto count = ::_write(output, buffer + written, unsigned(length - written));
#else
        auto count = ::write(output, buffer + written, length - written);
#endif
        if(count <= 0) return;
        written += Size(count);
    }
}

/*
 * Framing.
 */

static bool startsWith(const Array<Byte>& text, Size start, Size length, StringView prefix) {
    if(length < prefix.length) return false;

    // The header name is compared case-insensitively, because the specification says it is a HTTP
    // header and HTTP header names are.
    for(Size i = 0; i < prefix.length; i++) {
        auto a = text[start + i];
        auto b = (Byte)prefix.ptr[i];
        if(a >= 'A' && a <= 'Z') a = a - 'A' + 'a';
        if(b >= 'A' && b <= 'Z') b = b - 'A' + 'a';
        if(a != b) return false;
    }

    return true;
}

// The offset just past the blank line ending the header block, and the body length it declared.
static bool findHeader(const Array<Byte>& pending, Size& headerEnd, Size& contentLength) {
    Size lineStart = 0;
    auto found = false;
    contentLength = 0;

    for(Size i = 0; i + 1 < pending.size(); i++) {
        if(pending[i] != '\r' || pending[i + 1] != '\n') continue;

        auto length = i - lineStart;
        if(length == 0) {
            headerEnd = i + 2;
            return found;
        }

        if(startsWith(pending, lineStart, length, "content-length:"_v)) {
            auto p = lineStart + sizeof("content-length:") - 1;
            while(p < i && (pending[p] == ' ' || pending[p] == '\t')) p++;

            contentLength = 0;
            while(p < i && pending[p] >= '0' && pending[p] <= '9') {
                contentLength = contentLength * 10 + Size(pending[p] - '0');
                p++;
            }

            found = true;
        }

        lineStart = i + 2;
        i++;
    }

    return false;
}

bool FrameReader::read(Array<Byte>& into) {
    Byte chunk[8192];

    while(true) {
        Size headerEnd = 0;
        Size contentLength = 0;

        if(findHeader(pending, headerEnd, contentLength)) {
            if(pending.size() >= headerEnd + contentLength) {
                into.clear();
                for(Size i = 0; i < contentLength; i++) into.push(pending[headerEnd + i]);

                // Keep what came after this message: one read routinely spans a boundary, since an
                // editor writes a burst of notifications as one write.
                auto rest = pending.size() - (headerEnd + contentLength);
                for(Size i = 0; i < rest; i++) pending[i] = pending[headerEnd + contentLength + i];
                pending.resize(rest);

                return true;
            }
        }

        auto count = transport.read(chunk, sizeof(chunk));
        if(count == 0) return false;

        for(Size i = 0; i < count; i++) pending.push(chunk[i]);
    }
}

void writeFrame(Transport& transport, Buffer<const Byte> body) {
    char header[64];
    auto length = format(toBuffer(header), toString("Content-Length: %@\r\n\r\n"_v), body.length);

    transport.write((const Byte*)header, length);
    transport.write(body.ptr, body.length);
}

/*
 * URIs.
 */

static I32 hexValue(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

String uriToPath(StringView uri) {
    auto start = Size(0);
    if(uri.length >= 7 && compareMem(uri.ptr, "file://", 7) == 0) start = 7;

    StringBuilder path;
    for(auto i = start; i < uri.length; i++) {
        auto c = uri.ptr[i];
        if(c == '%' && i + 2 < uri.length) {
            auto high = hexValue(uri.ptr[i + 1]);
            auto low = hexValue(uri.ptr[i + 2]);
            if(high >= 0 && low >= 0) {
                char decoded = char(high * 16 + low);
                path.append(&decoded, 1);
                i += 2;
                continue;
            }
        }

        path.append(&c, 1);
    }

    // `file:///C:/src/Main.yana` is a Windows path with a slash in front of it that is not part of
    // the path. A leading slash followed by a drive letter is the only way to tell.
    auto text = path.string();
    if(text.size() >= 3 && text.text()[0] == '/' && text.text()[2] == ':') {
        return ownedString(text.text() + 1, text.size() - 1);
    }

    return text;
}

String pathToUri(StringView path) {
    StringBuilder uri;
    uri.append("file://"_v);

    // A Windows path starts at its drive letter, and the authority-less form needs a slash before
    // it. A POSIX path already has one.
    if(path.length >= 2 && path.ptr[1] == ':') uri.append("/"_v);

    for(Size i = 0; i < path.length; i++) {
        auto c = path.ptr[i];
        auto unreserved = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                       || c == '-' || c == '.' || c == '_' || c == '~' || c == '/' || c == ':';

        if(unreserved) {
            uri.append(&c, 1);
        } else {
            char encoded[3] = { '%', 0, 0 };
            const char* digits = "0123456789ABCDEF";
            encoded[1] = digits[(Byte)c >> 4];
            encoded[2] = digits[(Byte)c & 0xf];
            uri.append(encoded, 3);
        }
    }

    return uri.string();
}

/*
 * Messages.
 */

void MessageWriter::writeId(const JsonValue* id) {
    json.field("id"_v);

    if(!id) {
        json.null();
    } else if(id->kind == JsonValue::String) {
        json.value(id->string);
    } else if(id->kind == JsonValue::Number) {
        json.value(I64(id->number));
    } else {
        json.null();
    }
}

void MessageWriter::startResponse(const JsonValue* id) {
    json.startObject();
    json.field("jsonrpc"_v).value("2.0"_v);
    writeId(id);
}

void MessageWriter::startNotification(StringView method) {
    json.startObject();
    json.field("jsonrpc"_v).value("2.0"_v);
    json.field("method"_v).value(method);
}

Buffer<const Byte> MessageWriter::body() {
    auto buffered = writer.getBuffered();
    return { buffered.ptr, buffered.length };
}

void writeError(Transport& transport, const JsonValue* id, ErrorCode code, StringView message) {
    MessageWriter out;
    out.startResponse(id);
    out.json.field("error"_v).startObject();
    out.json.field("code"_v).value(I32(code));
    out.json.field("message"_v).value(message);
    out.json.endObject();
    out.endObject();

    writeFrame(transport, out.body());
}

} // namespace lsp
