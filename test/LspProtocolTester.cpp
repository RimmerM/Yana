// The protocol half of Implementation-Tooling.md's testing strategy: the lifecycle, driven through
// the real message loop, over a transport that is a pair of buffers rather than a pair of pipes.
//
// Ten cases, not a hundred. What the feature fixtures in `test/lsp/*.yana` assert is what the
// compiler answers; what this asserts is that a client can ask - that the framing survives a
// message split across reads, that `shutdown` is answerable, that an unknown method is refused
// rather than ignored, and that a fixed error is published as an empty list rather than left on
// screen.
//
// URIs are reduced to a file name in the output, since an absolute path is different on every
// machine and none of what is being tested is about the path.
#include <Core.h>
#include <File.h>
#include "../compiler/lsp/server.h"
#include "Net/Stream.h"
#include "Net/File.h"

using namespace Tritium;
using namespace lsp;

/*
 * A transport over two buffers.
 *
 * Two threads write to the output - the worker answers requests while the reader answers
 * `shutdown` - so a scenario that sends everything at once records them in whichever order the
 * scheduler produced. `barrier()` is what makes that deterministic: the input stops there until
 * the server has finished a compile, which is the only point at which it is known to be idle.
 * Waiting on the server rather than on a sleep is what keeps this from being slow and flaky at
 * once.
 */
struct MemoryTransport: Transport {
    Array<Byte> input;
    Array<Byte> output;
    Size cursor = 0;

    Server* server = nullptr;
    Array<Size> barriers;

    Size read(Byte* buffer, Size length) override {
        for(U32 i = 0; i < barriers.size(); i++) {
            if(barriers[i] == cursor) {
                barriers.remove(i);
                server->idle.wait(5000);
                break;
            }
        }

        // A short read, so the framing is exercised across buffer boundaries rather than being
        // handed one contiguous block it can always parse in a single pass.
        if(cursor < input.size()) {
            auto count = min(length, min(Size(48), input.size() - cursor));

            for(U32 i = 0; i < barriers.size(); i++) {
                if(barriers[i] > cursor && barriers[i] - cursor < count) count = barriers[i] - cursor;
            }

            for(Size i = 0; i < count; i++) buffer[i] = input[cursor + i];
            cursor += count;
            return count;
        }

        return 0;
    }

    void write(const Byte* buffer, Size length) override {
        for(Size i = 0; i < length; i++) output.push(buffer[i]);
    }

    void send(StringView body) {
        char header[64];
        auto length = format(toBuffer(header), toString("Content-Length: %@\r\n\r\n"_v), body.length);
        for(Size i = 0; i < length; i++) input.push((Byte)header[i]);
        for(Size i = 0; i < body.length; i++) input.push((Byte)body.ptr[i]);
    }

    /// Nothing after this point is delivered until one compile has finished.
    void barrier() { barriers.push(input.size()); }
};

// The last path segment of every `"uri":"..."` in the output, so the text does not depend on where
// the tree is checked out.
static void writeNormalized(Net::Writer& writer, const Array<Byte>& output) {
    auto marker = "\"uri\":\""_v;
    Size i = 0;

    while(i < output.size()) {
        auto matches = i + marker.length <= output.size();
        for(Size j = 0; matches && j < marker.length; j++) {
            if(output[i + j] != (Byte)marker.ptr[j]) matches = false;
        }

        if(!matches) {
            auto c = char(output[i]);
            // The frame header is not part of what is being asserted, and its length changes with
            // the path it contains. A newline in its place keeps one message per line.
            writer.writeString(StringView { &c, 1 });
            i++;
            continue;
        }

        writer.writeString(marker);
        i += marker.length;

        Size end = i;
        while(end < output.size() && output[end] != '"') end++;

        Size name = i;
        for(Size j = i; j < end; j++) {
            if(output[j] == '/' || output[j] == '\\') name = j + 1;
        }

        for(auto j = name; j < end; j++) {
            auto c = char(output[j]);
            writer.writeString(StringView { &c, 1 });
        }

        i = end;
    }
}

// One scenario: a fresh server, a list of messages, and whatever came back.
template<class Fill>
static void runScenario(Net::Writer& writer, StringView name, Fill&& fill) {
    MemoryTransport transport;
    fill(transport);

    Server server(transport);
    transport.server = &server;

    auto code = server.run();

    writer.writeString("== "_v);
    writer.writeString(name);
    writer.writeString("\n"_v);
    writeNormalized(writer, transport.output);
    writer.writeString("\n"_v);

    char buffer[64];
    auto length = format(toBuffer(buffer), toString("exit %@\n\n"_v), code);
    writer.writeString(StringView { buffer, length });
}

static String initializeMessage(const String& root) {
    return format("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
                  "{\"rootPath\":\"%@\",\"capabilities\":{\"general\":{\"positionEncodings\":[\"utf-8\"]}}}}", root);
}

static String didOpenMessage(const String& path, const String& text) {
    return format("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":"
                  "{\"uri\":\"file://%@\",\"languageId\":\"yana\",\"version\":1,\"text\":\"%@\"}}}", path, text);
}

static String didChangeMessage(const String& path, I32 version, const String& text) {
    return format("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":{\"textDocument\":"
                  "{\"uri\":\"file://%@\",\"version\":%@},\"contentChanges\":[{\"text\":\"%@\"}]}}",
                  path, version, text);
}

static String positionRequest(StringView method, I32 id, const String& path, U32 line, U32 character) {
    return format("{\"jsonrpc\":\"2.0\",\"id\":%@,\"method\":\"%@\",\"params\":{\"textDocument\":"
                  "{\"uri\":\"file://%@\"},\"position\":{\"line\":%@,\"character\":%@}}}",
                  id, method, path, line, character);
}

static void writeScenarios(Net::Writer& writer, const String& root, const String& mainPath) {
    auto clean = String("fn main() -> Int = 40 + 2\\n");
    auto broken = String("fn main() -> Int = missingName(1)\\n");

    runScenario(writer, "lifecycle"_v, [&](MemoryTransport& t) {
        t.send(stringView(initializeMessage(root)));
        t.barrier();
        t.send("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"shutdown\"}"_v);
        t.send("{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}"_v);
    });

    runScenario(writer, "request before initialize"_v, [&](MemoryTransport& t) {
        t.send("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"textDocument/hover\",\"params\":{}}"_v);
    });

    runScenario(writer, "unknown method"_v, [&](MemoryTransport& t) {
        t.send(stringView(initializeMessage(root)));
        t.barrier();
        t.send("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/inlayHint\",\"params\":{}}"_v);
    });

    runScenario(writer, "malformed json"_v, [&](MemoryTransport& t) {
        t.send("{\"jsonrpc\":"_v);
    });

    runScenario(writer, "an error appears"_v, [&](MemoryTransport& t) {
        t.send(stringView(initializeMessage(root)));
        t.send(stringView(didOpenMessage(mainPath, clean)));
        t.barrier();
        t.send(stringView(didChangeMessage(mainPath, 2, broken)));
        t.barrier();
    });

    runScenario(writer, "an error is fixed"_v, [&](MemoryTransport& t) {
        t.send(stringView(initializeMessage(root)));
        t.send(stringView(didOpenMessage(mainPath, broken)));
        t.barrier();
        t.send(stringView(didChangeMessage(mainPath, 2, clean)));
        t.barrier();
    });

    /*
     * The four M6 features, over the wire.
     *
     * What this asserts is not the answers - `test/lsp/*.yana` does that, without the transport in
     * the way - but that a client asking for them gets a well-formed response with the right id.
     * The positions are the two occurrences of `double` on the second line.
     */
    auto twoFunctions = String("fn double(x: Int) -> Int = x + x\\nfn main() -> Int = double(21)\\n");

    runScenario(writer, "definition, hover, references and tokens"_v, [&](MemoryTransport& t) {
        t.send(stringView(initializeMessage(root)));
        t.send(stringView(didOpenMessage(mainPath, twoFunctions)));
        t.barrier();

        t.send(stringView(positionRequest("textDocument/definition"_v, 10, mainPath, 1, 20)));
        t.send(stringView(positionRequest("textDocument/hover"_v, 11, mainPath, 1, 20)));
        t.send(stringView(format("{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"textDocument/references\",\"params\":"
                                 "{\"textDocument\":{\"uri\":\"file://%@\"},\"position\":{\"line\":1,\"character\":20},"
                                 "\"context\":{\"includeDeclaration\":true}}}", mainPath)));
        t.send(stringView(format("{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"textDocument/semanticTokens/full\","
                                 "\"params\":{\"textDocument\":{\"uri\":\"file://%@\"}}}", mainPath)));
        t.barrier();
    });

    // The cancellation is sent before the request it cancels, which is not what a client does and
    // is the only ordering a test can assert on: the point of answering `$/cancelRequest` on the
    // reader thread is that it arrives while the worker is busy, and "while" is not reproducible.
    runScenario(writer, "a cancelled request"_v, [&](MemoryTransport& t) {
        t.send(stringView(initializeMessage(root)));
        t.barrier();
        t.send("{\"jsonrpc\":\"2.0\",\"method\":\"$/cancelRequest\",\"params\":{\"id\":7}}"_v);
        t.send("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"textDocument/definition\",\"params\":{}}"_v);
    });
}

int main(int argc, const char** argv) {
    auto generate = false;
    for(int i = 1; i < argc; i++) {
        if(String(argv[i]) == "generate") generate = true;
    }

    // The fixture project. A directory rather than a temporary one, so that what the server is
    // pointed at is reviewable next to what it answered.
    auto root = String("lsp/project");
    auto mainPath = String("lsp/project/src/Main.yana");
    auto expectPath = String("lsp/protocol.expect");

    if(generate) {
        logInfo("Generating expect file for the protocol test");

        try {
            Net::FileStream file;
            file.open(expectPath, writeAccess(), File::CreateAlways);

            Net::Writer writer(Net::WriteStream(file), 65536);
            writeScenarios(writer, root, mainPath);
            writer.flush();
        } catch(const Net::Exception& e) {
            logError("Cannot create the protocol expect file: %@", e.description);
        }

        return 0;
    }

    print("Running test \"lsp/protocol\"... ");

    Net::Writer writer(65536);
    writeScenarios(writer, root, mainPath);

    auto result = File::openFile(expectPath, readAccess());
    if(result.isErr()) {
        println("cannot open %@: error %@", expectPath, (U32)result.unwrapErr());
        return 1;
    }

    auto file = result.moveUnwrapOk();
    auto size = file.size();
    Ptr<char, HeapDeleter> buffer { (char*)hAlloc(size ? size : 1) };
    if(size) file.read({ (Byte*)buffer.get(), size });

    auto produced = writer.getBuffered();
    if(size == produced.length && compareMem(buffer.get(), produced.ptr, size) == 0) {
        println("Pass.");
        return 0;
    }

    println("Fail. Got:");
    print(StringView { (char*)produced.ptr, produced.length });
    println("\n\nExpected:");
    print(StringView { buffer.get(), size });
    print("\n\n");
    return 1;
}
