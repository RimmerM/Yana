// Test driver for the editor-facing half of the compiler - Implementation-Tooling.md's testing
// strategy. A fixture is a `.yana` file with cursor markers written in comments:
//
//     fn add(a: Int, b: Int) -> Int = a + b
//     --  ^name       ^type
//
// A marker line is a comment holding one or more `^word`; each `^` names the byte in the line
// above it that is under the cursor. The driver answers each marker and writes the answers to a
// `.expect`, so that a change in what an editor would say is reviewed as a diff rather than
// asserted case by case.
//
// This tests the *features* rather than the transport: the JSON-RPC framing is Tritium's problem
// and has its own protocol test. What is here is what the compiler answers.
#include <Core.h>
#include <File.h>
#include "../compiler/parse/parser.h"
#include "../compiler/compiler/position.h"
#include "../compiler/lsp/feature.h"
#include "Net/Stream.h"
#include "Net/File.h"

using namespace Tritium;

struct TestProvider: SourceProvider {
    StringView source;
    Context* context = nullptr;

    StringView getSource(StringId module) override { return source; }

    const Location* getNode(LocationId id) override {
        return context ? context->getLocation(id) : nullptr;
    }
};

struct Marker {
    String name;
    U32 offset;
    U32 line;
    U32 column;
};

// The markers of one fixture, in the order they are written.
//
// A marker line is one whose first non-space characters are `--` and which holds a `^`. Its own
// text is still part of the module - it is a comment, so the parser skips it - which is what makes
// a fixture a program that compiles rather than a program with an annotation format bolted on.
static void findMarkers(StringView text, const LineTable& lines, Array<Marker>& markers) {
    U32 previousLine = 0;

    for(U32 line = 0; line < lines.lineCount(); line++) {
        auto start = lines.lineStart(line);
        auto end = line + 1 < lines.lineCount() ? lines.lineStart(line + 1) : U32(text.length);
        while(end > start && (text.ptr[end - 1] == '\n' || text.ptr[end - 1] == '\r')) end--;

        auto content = start;
        while(content < end && (text.ptr[content] == ' ' || text.ptr[content] == '\t')) content++;

        auto isComment = content + 1 < end && text.ptr[content] == '-' && text.ptr[content + 1] == '-';
        auto hasCaret = false;
        if(isComment) {
            for(auto i = content; i < end; i++) {
                if(text.ptr[i] == '^') { hasCaret = true; break; }
            }
        }

        if(!isComment || !hasCaret) {
            previousLine = line;
            continue;
        }

        for(auto i = content; i < end; i++) {
            if(text.ptr[i] != '^') continue;

            auto nameStart = i + 1;
            auto nameEnd = nameStart;
            while(nameEnd < end && text.ptr[nameEnd] != ' ' && text.ptr[nameEnd] != '\t') nameEnd++;

            // The caret's column is taken as a character count rather than a byte count, so that a
            // marker lines up under what it points at in an editor even when the line above holds
            // something outside ASCII. That makes this the same arithmetic the server does for a
            // client's position, on the same code, which is the point.
            auto column = i - start;
            auto offset = lines.offsetAt(text, previousLine, column, true);

            markers.push(Marker {
                ownedString(text.ptr + nameStart, nameEnd - nameStart),
                offset,
                previousLine,
                column,
            });
        }
    }
}

// The source a range covers, with newlines and tabs shown, so that a range that swallowed a line
// break is visible in the diff rather than being a mysteriously tall expectation.
static void writeQuoted(Net::Writer& writer, StringView text, U32 start, U32 end) {
    if(end > text.length) end = U32(text.length);
    if(start > end) start = end;

    writer.writeString("\""_v);
    for(auto i = start; i < end; i++) {
        auto c = text.ptr[i];
        if(c == '\n') writer.writeString("\\n"_v);
        else if(c == '\t') writer.writeString("\\t"_v);
        else if(c == '\r') writer.writeString("\\r"_v);
        else writer.writeString(StringView { text.ptr + i, 1 });
    }
    writer.writeString("\""_v);
}

static void writeRange(Net::Writer& writer, Context& context, const LineTable& lines,
                       StringView text, LocationId id) {
    auto location = context.getLocation(id);
    if(!location) {
        writer.writeString("<none>"_v);
        return;
    }

    auto start = location->sourceStart.offset;
    auto end = location->sourceEnd.offset;

    char buffer[256];
    auto length = format(toBuffer(buffer), toString("%@:%@..%@:%@ "_v),
                         lines.lineOf(start) + 1, lines.utf16Column(text, start),
                         lines.lineOf(end) + 1, lines.utf16Column(text, end));
    writer.writeString(StringView { buffer, length });
    writeQuoted(writer, text, start, end);
}

static void writeAnswers(Net::Writer& writer, Context& context, StringView text) {
    LineTable lines;
    lines.build(text);

    Array<Marker> markers;
    findMarkers(text, lines, markers);

    PositionIndex index;
    index.build(context, context.addUnqualifiedName("no_name", 7));

    char buffer[512];
    auto length = format(toBuffer(buffer), toString("-- %@ locations, %@ in source order\n\n"_v),
                         index.entries.size(), index.wasOrdered ? "already"_v : "not"_v);
    writer.writeString(StringView { buffer, length });

    for(auto& marker: markers) {
        length = format(toBuffer(buffer), toString("%@ at %@:%@ (offset %@)\n"_v),
                        marker.name, marker.line + 1, lines.utf16Column(text, marker.offset), marker.offset);
        writer.writeString(StringView { buffer, length });

        writer.writeString("  innermost: "_v);
        writeRange(writer, context, lines, text, index.find(marker.offset));
        writer.writeString("\n"_v);

        Array<LocationId> enclosing;
        index.findEnclosing(marker.offset, enclosing);

        for(auto id: enclosing) {
            writer.writeString("  enclosing: "_v);
            writeRange(writer, context, lines, text, id);
            writer.writeString("\n"_v);
        }

        writer.writeString("\n"_v);
    }
}

static void runFixture(const String& path, StringView content, bool generate) {
    TestProvider provider;
    provider.source = content;

    PrintDiagnostics diagnostics(provider);
    Context context(diagnostics);
    provider.context = &context;

    auto name = context.addUnqualifiedName("no_name", 7);
    Lexer lexer(context, context.diagnostics, content, name);
    Parser parser(context, lexer, name);
    auto ast = parser.parseModule();

    if(generate) {
        logInfo("Generating expect file for test \"%@\"", path);

        try {
            Net::FileStream file;
            file.open(path + ".expect", writeAccess(), File::CreateAlways);

            Net::Writer writer(Net::WriteStream(file), 16384);
            writeAnswers(writer, context, content);
            writer.flush();
        } catch(const Net::Exception& e) {
            logError("Cannot create expect file for \"%@\": %@", path, e.description);
        }

        return;
    }

    print("Running test \"%@\"... ", path);

    Net::Writer writer(16384);
    writeAnswers(writer, context, content);

    auto expectPath = path + String(".expect");
    auto result = File::openFile(expectPath, readAccess());
    if(result.isErr()) {
        println("cannot open %@: error %@", expectPath, (U32)result.unwrapErr());
        return;
    }

    auto file = result.moveUnwrapOk();
    auto size = file.size();
    Ptr<char, HeapDeleter> buffer { (char*)hAlloc(size ? size : 1) };
    if(size) file.read({ (Byte*)buffer.get(), size });

    auto produced = writer.getBuffered();
    if(size == produced.length && compareMem(buffer.get(), produced.ptr, size) == 0) {
        println("Pass.");
    } else {
        println("Fail. Got:");
        print(StringView { (char*)produced.ptr, produced.length });
        println("\n\nExpected:");
        print(StringView { buffer.get(), size });
        print("\n\n");
    }
}


/*
 * The semantic pass.
 *
 * A whole project rather than one file, because the answers worth asserting are the cross-module
 * ones: a definition in another file is what a single-file test cannot get wrong and what a real
 * one does. It goes through `lsp::Session` and the feature functions the server calls, so what is
 * compared here is what an editor would be shown.
 */

// The location of a definition or an occurrence, as `File.yana:line:col..line:col "text"`. A file
// name rather than a path, so the fixture does not depend on where the tree is checked out.
static void writeSymbolLocation(Net::Writer& writer, lsp::Session& session, LocationId id) {
    auto location = session.context ? session.context->getLocation(id) : nullptr;
    if(!location) {
        writer.writeString("<none>"_v);
        return;
    }

    auto path = session.pathOf(location->sourceModule);
    if(path.length == 0) {
        // Core and Native, which are compiled into the compiler and have no file to point at.
        writer.writeString("<builtin>"_v);
        return;
    }

    auto name = path;
    for(Size i = 0; i < path.length; i++) {
        if(path.ptr[i] == '/' || path.ptr[i] == '\\') name = StringView { path.ptr + i + 1, path.length - i - 1 };
    }

    auto text = session.provider.getSource(location->sourceModule);
    LineTable lines;
    lines.build(text);

    auto start = location->sourceStart.offset;
    auto end = location->sourceEnd.offset;

    char buffer[512];
    auto length = format(toBuffer(buffer), toString("%@:%@:%@..%@:%@ "_v), name,
                         lines.lineOf(start) + 1, lines.utf16Column(text, start),
                         lines.lineOf(end) + 1, lines.utf16Column(text, end));
    writer.writeString(StringView { buffer, length });
    writeQuoted(writer, text, start, end);
}

static void writeSemanticAnswers(Net::Writer& writer, lsp::Session& session, StringId module,
                                 StringView path, StringView text) {
    LineTable lines;
    lines.build(text);

    Array<Marker> markers;
    findMarkers(text, lines, markers);

    char buffer[512];
    auto length = format(toBuffer(buffer), toString("== %@\n\n"_v), path);
    writer.writeString(StringView { buffer, length });

    for(auto& marker: markers) {
        length = format(toBuffer(buffer), toString("%@ at %@:%@\n"_v), marker.name, marker.line + 1,
                        lines.utf16Column(text, marker.offset));
        writer.writeString(StringView { buffer, length });

        auto reference = session.referenceAt(module, marker.offset);
        auto symbol = reference ? &reference->target : session.definitionAt(module, marker.offset);

        if(!symbol) {
            writer.writeString("  nothing resolved here\n\n"_v);
            continue;
        }

        writer.writeString("  kind: "_v);
        writer.writeString(symbolKindName(symbol->kind));
        writer.writeString("\n  definition: "_v);
        writeSymbolLocation(writer, session, symbol->definition);
        writer.writeString("\n"_v);

        StringBuilder hover;
        lsp::describeAt(session, module, marker.offset, hover);

        // One line per line of hover, indented, so a multi-line answer is still readable as a diff.
        writer.writeString("  hover: "_v);
        for(Size i = 0; i < hover.size(); i++) {
            if(hover[i] == '\n') writer.writeString("\n         "_v);
            else writer.writeString(StringView { &hover[i], 1 });
        }
        writer.writeString("\n"_v);

        if(session.index) {
            Array<const Reference*> occurrences;
            session.index->findOccurrences(*symbol, occurrences);

            for(auto occurrence: occurrences) {
                writer.writeString("  reference: "_v);
                writeSymbolLocation(writer, session, occurrence->source);
                writer.writeString("\n"_v);
            }
        }

        writer.writeString("\n"_v);
    }

    // The tokens the same file would be coloured with, as the protocol's own delta encoding. Held
    // to a fixture because it is the one answer whose *encoding* can regress silently: a wrong
    // delta shows up as everything after it being coloured one word to the left.
    writer.writeString("-- semantic tokens\n"_v);

    Net::Writer json(4096);
    Net::JsonWriter out(json);
    lsp::writeSemanticTokens(out, session, module, text, lines, true);
    json.flush();

    auto produced = json.getBuffered();
    writer.writeString(StringView { (const char*)produced.ptr, produced.length });
    writer.writeString("\n\n"_v);
}

// Everything the compile reported, which for a project that is mid-edit is most of what an editor
// shows. Held to a fixture for the same reason the answers are: a second diagnostic on one mistake
// is a regression that nothing else in the suite would notice - Implementation-Tooling.md §3.2.
static void writeDiagnostics(Net::Writer& writer, lsp::Session& session) {
    writer.writeString("== diagnostics\n\n"_v);

    if(session.diagnostics.messages.size() == 0) {
        writer.writeString("(none)\n\n"_v);
        return;
    }

    for(auto& message: session.diagnostics.messages) {
        const char* kind;
        switch(message.level) {
            case Diagnostics::ErrorLevel: kind = "error"; break;
            case Diagnostics::WarningLevel: kind = "warning"; break;
            default: kind = "message";
        }

        char buffer[1024];
        Size length;

        if(message.hasLocation) {
            auto path = session.pathOf(message.where.sourceModule);
            auto name = path;
            for(Size i = 0; i < path.length; i++) {
                if(path.ptr[i] == '/' || path.ptr[i] == '\\') name = StringView { path.ptr + i + 1, path.length - i - 1 };
            }

            length = format(toBuffer(buffer), toString("%@ %@:%@:%@: %@\n"_v), kind,
                            name.length ? name : "<builtin>"_v, message.where.sourceStart.line + 1,
                            message.where.sourceStart.column, message.text);
        } else {
            length = format(toBuffer(buffer), toString("%@ <no location>: %@\n"_v), kind, message.text);
        }

        writer.writeString(StringView { buffer, length });
    }

    writer.writeString("\n"_v);
}

static void runSemanticFixture(const String& root, const String& expectPath, bool generate) {
    lsp::Session session;

    auto opened = session.open(stringView(root));
    if(opened.isErr()) {
        println("cannot open %@: %@", root, opened.unwrapErr());
        return;
    }

    session.compile();

    // The files in a fixed order. The module map is filled by walking a directory, and the order a
    // directory is walked in is the file system's business rather than anything to assert on.
    Array<SourceEntry*> entries;
    for(auto& entry: session.moduleMap.entries) entries.push(&entry);

    for(U32 i = 1; i < entries.size(); i++) {
        auto entry = entries[i];
        auto j = i;
        while(j > 0 && entry->path < entries[j - 1]->path) {
            entries[j] = entries[j - 1];
            j--;
        }

        entries[j] = entry;
    }

    Net::Writer memory(65536);
    writeDiagnostics(memory, session);

    for(auto entry: entries) {
        if(!entry->name) continue;

        auto text = session.provider.getSource(entry->name);
        if(text.length == 0) continue;

        writeSemanticAnswers(memory, session, entry->name, entry->path, text);
    }

    if(generate) {
        logInfo("Generating expect file for test \"%@\"", expectPath);

        try {
            Net::FileStream file;
            file.open(expectPath, writeAccess(), File::CreateAlways);

            Net::Writer writer(Net::WriteStream(file), 65536);
            auto produced = memory.getBuffered();
            writer.writeString(StringView { (const char*)produced.ptr, produced.length });
            writer.flush();
        } catch(const Net::Exception& e) {
            logError("Cannot create expect file for \"%@\": %@", expectPath, e.description);
        }

        return;
    }

    print("Running test \"%@\"... ", expectPath);

    auto result = File::openFile(expectPath, readAccess());
    if(result.isErr()) {
        println("cannot open %@: error %@", expectPath, (U32)result.unwrapErr());
        return;
    }

    auto file = result.moveUnwrapOk();
    auto size = file.size();
    Ptr<char, HeapDeleter> buffer { (char*)hAlloc(size ? size : 1) };
    if(size) file.read({ (Byte*)buffer.get(), size });

    auto produced = memory.getBuffered();
    if(size == produced.length && compareMem(buffer.get(), produced.ptr, size) == 0) {
        println("Pass.");
    } else {
        println("Fail. Got:");
        print(StringView { (char*)produced.ptr, produced.length });
        println("\n\nExpected:");
        print(StringView { buffer.get(), size });
        print("\n\n");
    }
}

int main(int argc, const char** argv) {
    auto generate = false;
    for(int i = 1; i < argc; i++) {
        if(String(argv[i]) == "generate") generate = true;
    }

    Array<String> tests;
    listDirectory("lsp", [&](const String& name, bool isDirectory) {
        if(isDirectory || name == "." || name == "..") return;

        if(auto p = findLastChar(stringView(name), '.')) {
            String extension(p + 1, name.text() + name.size() - p - 1);
            if(extension == "yana") tests.push(String("lsp/") + name);
        }
    });

    if(tests.size() == 0) println("no tests found");

    // The whole-project pass, which is where everything past the position index is asserted. Twice:
    // once over a project that compiles, and once over one that is mid-edit, since what the second
    // asserts is that the first's answers survive a broken declaration above them.
    runSemanticFixture(String("lsp/semantic"), String("lsp/semantic.expect"), generate);
    runSemanticFixture(String("lsp/recover"), String("lsp/recover.expect"), generate);

    for(auto& test: tests) {
        auto result = File::openFile(test, readAccess());
        if(result.isErr()) {
            println("cannot open file %@: error %@", test, (U32)result.unwrapErr());
            continue;
        }

        auto file = result.moveUnwrapOk();
        auto size = file.size();
        Ptr<char, HeapDeleter> buffer { (char*)hAlloc(size ? size : 1) };
        if(size) file.read({ (Byte*)buffer.get(), size });

        runFixture(test, { buffer.get(), size }, generate);
    }
}
