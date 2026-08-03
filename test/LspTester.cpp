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

static bool runFixture(const String& path, StringView content, bool generate) {
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
            return false;
        }

        return true;
    }

    print("Running test \"%@\"... ", path);

    Net::Writer writer(16384);
    writeAnswers(writer, context, content);

    auto expectPath = path + String(".expect");
    auto result = File::openFile(expectPath, readAccess());
    if(result.isErr()) {
        println("cannot open %@: error %@", expectPath, (U32)result.unwrapErr());
        return false;
    }

    auto file = result.moveUnwrapOk();
    auto size = file.size();
    Ptr<char, HeapDeleter> buffer { (char*)hAlloc(size ? size : 1) };
    if(size) file.read({ (Byte*)buffer.get(), size });

    auto produced = writer.getBuffered();
    if(size == produced.length && compareMem(buffer.get(), produced.ptr, size) == 0) {
        println("Pass.");
        return true;
    }

    println("Fail. Got:");
    print(StringView { (char*)produced.ptr, produced.length });
    println("\n\nExpected:");
    print(StringView { buffer.get(), size });
    print("\n\n");
    return false;
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

/*
 * Signature help, for the markers that ask for it.
 *
 * Only the ones whose name starts with `sig`, because unlike the four answers above it is a question
 * about a *call* rather than about a name: asking it of every marker would fill both existing
 * fixtures with "not in a call" and say nothing. It rides along with the semantic pass rather than
 * the completion one because it reads the ordinary compile - a call being typed still parses as a
 * call, which is what M7's recovery bought.
 */
static void writeSignatureAnswer(Net::Writer& writer, lsp::Session& session, StringId module,
                                 const Marker& marker, StringView text) {
    if(marker.name.size() < 3 || compareMem(marker.name.text(), "sig", 3) != 0) return;

    Net::Writer json(8192);
    Net::JsonWriter out(json);
    lsp::writeSignatureHelp(out, session, module, marker.offset, text);
    json.flush();

    auto produced = json.getBuffered();
    writer.writeString("  signature: "_v);
    writer.writeString(StringView { (const char*)produced.ptr, produced.length });
    writer.writeString("\n"_v);
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
            writer.writeString("  nothing resolved here\n"_v);

            // Before the `continue`, because a signature is a question about a *call* rather than
            // about a name: the position inside `Square {` where an overlay is most wanted is one
            // where nothing resolves, and asking only after something did is what would have hidden
            // the whole feature.
            writeSignatureAnswer(writer, session, module, marker, text);
            writer.writeString("\n"_v);
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

        /*
         * The `explain` section the hover carries under the signature - M9.
         *
         * Held here rather than only in `test/resolve/Explain.yana`'s dump because what that
         * fixture asserts is the *printer* and what this one asserts is the *caller*: which symbol
         * under a cursor produces a section at all. A cursor on a call gets the callee's, and a
         * function with nothing surprising to say gets none, and neither of those is visible from
         * the printer's side.
         */
        StringBuilder explanation;
        if(lsp::explainAt(session, module, marker.offset, explanation)) {
            writer.writeString("  explain:"_v);
            for(Size i = 0; i < explanation.size(); i++) {
                if(explanation[i] == '\n') writer.writeString("\n           "_v);
                else writer.writeString(StringView { &explanation[i], 1 });
            }
            writer.writeString("\n"_v);
        }

        if(session.index) {
            Array<const Reference*> occurrences;
            session.index->findOccurrences(*symbol, occurrences);

            for(auto occurrence: occurrences) {
                writer.writeString("  reference: "_v);
                writeSymbolLocation(writer, session, occurrence->source);
                writer.writeString("\n"_v);
            }
        }

        writeSignatureAnswer(writer, session, module, marker, text);
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

    /*
     * The three whole-file answers - Implementation-Tooling.md §6, and M9's inlay hints.
     *
     * Per file rather than per marker, because none of them is about a position: what a client asks
     * for is everything the file has, and what can go wrong with each is an entry that is missing,
     * duplicated or in the wrong place. That is a diff.
     */
    auto section = [&](StringView name, auto&& write) {
        writer.writeString(name);
        writer.writeString("\n"_v);

        Net::Writer body(8192);
        Net::JsonWriter value(body);
        write(value);
        body.flush();

        auto written = body.getBuffered();
        writer.writeString(StringView { (const char*)written.ptr, written.length });
        writer.writeString("\n\n"_v);
    };

    section("-- inlay hints"_v, [&](Net::JsonWriter& value) {
        lsp::writeInlayHints(value, session, module, text, lines, true, 0, U32(text.length));
    });

    section("-- folding ranges"_v, [&](Net::JsonWriter& value) {
        lsp::writeFoldingRanges(value, text, lines);
    });

    section("-- document symbols"_v, [&](Net::JsonWriter& value) {
        lsp::LocationWriter locations(session, true);
        lsp::writeDocumentSymbols(value, session, locations, module);
    });
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

/*
 * Completion - Implementation-Tooling.md §8.
 *
 * A pass of its own, and a fixture project of its own, because answering one *compiles*: the cursor
 * sentinel goes in during the parse, so every marker is a whole compile with the cursor at that
 * marker and nothing about one marker's parse is visible to another. That is also why it cannot
 * share the semantic pass above, whose answers all come from one compile.
 *
 * What is compared is the protocol's own answer, written through the same function the server
 * writes it with - so the item kinds, the ranking and the detail lines are asserted as a client
 * would receive them rather than as an intermediate the server then reformats.
 */

// The most a marker prints. An unfiltered position offers every visible name in the program, which
// for any program at all is most of Core - so what a fixture can usefully hold is the head of the
// list, which is where the ranking decides what an editor shows first.
static const U32 kMaxCompletionItems = 12;

// Where the string starting at `i` ends, or `length` when it does not. Every scan here needs it:
// an item's `insertText` is a snippet, so it holds the braces and commas that would otherwise look
// like structure - `scale(${1:s}, ${2:by})` is one value and not three.
static Size endOfString(StringView text, Size i) {
    for(i++; i < text.length; i++) {
        if(text.ptr[i] == '\\') { i++; continue; }
        if(text.ptr[i] == '"') return i;
    }

    return text.length;
}

// One `"key": "value"` out of the item object, without a JSON parser: the driver writes the
// response and reads it back, so what is here is a formatting question rather than a parsing one.
static bool readField(StringView object, StringView key, StringView& into) {
    for(Size i = 0; i < object.length; i++) {
        if(object.ptr[i] != '"') continue;

        auto keyEnd = endOfString(object, i);
        auto matches = keyEnd - i - 1 == key.length &&
                       compareMem(object.ptr + i + 1, key.ptr, key.length) == 0;

        // A value is a string too, and skipping it whole is what keeps a key from being found
        // inside one - `"detail":"fn key(...)"` must not answer a search for `key`.
        if(!matches || keyEnd + 1 >= object.length || object.ptr[keyEnd + 1] != ':') {
            i = keyEnd;
            continue;
        }

        auto value = keyEnd + 2;
        if(value >= object.length) return false;

        if(object.ptr[value] != '"') {
            auto end = value;
            while(end < object.length && object.ptr[end] != ',' && object.ptr[end] != '}') end++;

            into = StringView { object.ptr + value, end - value };
            return true;
        }

        auto end = endOfString(object, value);
        into = StringView { object.ptr + value + 1, end - value - 1 };
        return true;
    }

    return false;
}

static void writeCompletionAnswers(Net::Writer& writer, const String& root, StringId module,
                                   StringView path, StringView text) {
    LineTable lines;
    lines.build(text);

    Array<Marker> markers;
    findMarkers(text, lines, markers);

    // A file with no markers contributes nothing. Unlike the semantic pass, which prints a section
    // per module because the tokens of each are part of the answer, this pass has an answer only
    // where a cursor was asked about.
    if(markers.size() == 0) return;

    char buffer[512];
    auto length = format(toBuffer(buffer), toString("== %@\n\n"_v), path);
    writer.writeString(StringView { buffer, length });

    for(auto& marker: markers) {
        length = format(toBuffer(buffer), toString("%@ at %@:%@\n"_v), marker.name, marker.line + 1,
                        lines.utf16Column(text, marker.offset));
        writer.writeString(StringView { buffer, length });

        /*
         * A session for each marker.
         *
         * One session would do - `complete` recompiles - but a fresh one is what makes each answer
         * independent of the order the markers are written in, which is the property a fixture that
         * is edited by hand needs most.
         */
        lsp::Session session;
        auto opened = session.open(stringView(root));
        if(opened.isErr()) {
            writer.writeString("  cannot open the project\n\n"_v);
            continue;
        }

        Net::Writer json(65536);
        Net::JsonWriter out(json);
        lsp::writeCompletion(out, session, module, marker.offset, text, true, true);
        json.flush();

        auto produced = json.getBuffered();
        StringView answer { (const char*)produced.ptr, produced.length };

        StringView incomplete;
        readField(answer, "isIncomplete"_v, incomplete);
        writer.writeString("  filtered: "_v);
        writer.writeString(incomplete.length ? incomplete : "?"_v);
        writer.writeString("\n"_v);

        /*
         * The items, one line each.
         *
         * From inside the `items` array rather than from the start, since the response is itself an
         * object and would otherwise be read as the first item - which it silently was, giving one
         * item per marker whose fields came from several. Object boundaries are found by counting
         * depth and skipping strings whole, because a snippet's `${1:a}` is braces inside a value.
         */
        U32 shown = 0;
        Size i = 0;
        auto any = false;

        for(Size at = 0; at + 8 < answer.length; at++) {
            if(compareMem(answer.ptr + at, "\"items\":", 8) == 0) { i = at + 8; break; }
        }

        while(i < answer.length) {
            if(answer.ptr[i] == '"') { i = endOfString(answer, i) + 1; continue; }
            if(answer.ptr[i] != '{') { i++; continue; }

            Size end = i;
            U32 depth = 0;

            for(; end < answer.length; end++) {
                if(answer.ptr[end] == '"') { end = endOfString(answer, end); continue; }
                if(answer.ptr[end] == '{') depth++;
                else if(answer.ptr[end] == '}' && --depth == 0) break;
            }

            StringView object { answer.ptr + i, (end < answer.length ? end + 1 : answer.length) - i };
            i = end + 1;

            StringView label, kind, detail, sort, insert, format_;
            if(!readField(object, "label"_v, label)) continue;

            any = true;
            if(shown == kMaxCompletionItems) {
                writer.writeString("  ... more items follow\n"_v);
                break;
            }

            shown++;
            readField(object, "kind"_v, kind);
            readField(object, "sortText"_v, sort);
            readField(object, "detail"_v, detail);

            length = format(toBuffer(buffer), toString("  %@ [kind %@, sort %@]"_v), label,
                            kind.length ? kind : "?"_v, sort.length ? sort : "?"_v);
            writer.writeString(StringView { buffer, length });

            // What selecting the item types, where that is not just its name - the brackets are the
            // half of the answer a label does not show.
            if(readField(object, "insertText"_v, insert)) {
                readField(object, "insertTextFormat"_v, format_);
                length = format(toBuffer(buffer), toString(" inserts %@%@"_v),
                                format_ == "2"_v ? "snippet "_v : ""_v, insert);
                writer.writeString(StringView { buffer, length });
            }

            if(detail.length) {
                writer.writeString(" -- "_v);
                writer.writeString(detail);
            }

            writer.writeString("\n"_v);
        }

        if(!any) writer.writeString("  (no items)\n"_v);
        writer.writeString("\n"_v);
    }
}

static bool runCompletionFixture(const String& root, const String& expectPath, bool generate) {
    // One session to find out what the project holds. Every answer below builds its own.
    lsp::Session session;

    auto opened = session.open(stringView(root));
    if(opened.isErr()) {
        println("cannot open %@: %@", root, opened.unwrapErr());
        return false;
    }

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

    session.compile();

    // The text is copied out before any completion compile runs: a compile drops the provider's
    // buffers and reads them again, so a view of one taken beforehand does not survive it.
    struct Fixture {
        StringId module = 0;
        String path;
        String text;
    };

    Array<Fixture> fixtures;
    for(auto entry: entries) {
        if(!entry->name) continue;

        auto text = session.provider.getSource(entry->name);
        if(text.length == 0) continue;

        fixtures.push(Fixture { entry->name, ownedString(entry->path.ptr, entry->path.length),
                                ownedString(text.ptr, text.length) });
    }

    Net::Writer memory(65536);
    for(auto& fixture: fixtures) {
        writeCompletionAnswers(memory, root, fixture.module, stringView(fixture.path),
                               stringView(fixture.text));
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
            return false;
        }

        return true;
    }

    print("Running test \"%@\"... ", expectPath);

    auto result = File::openFile(expectPath, readAccess());
    if(result.isErr()) {
        println("cannot open %@: error %@", expectPath, (U32)result.unwrapErr());
        return false;
    }

    auto file = result.moveUnwrapOk();
    auto size = file.size();
    Ptr<char, HeapDeleter> buffer { (char*)hAlloc(size ? size : 1) };
    if(size) file.read({ (Byte*)buffer.get(), size });

    auto produced = memory.getBuffered();
    if(size == produced.length && compareMem(buffer.get(), produced.ptr, size) == 0) {
        println("Pass.");
        return true;
    }

    println("Fail. Got:");
    print(StringView { (char*)produced.ptr, produced.length });
    println("\n\nExpected:");
    print(StringView { buffer.get(), size });
    print("\n\n");
    return false;
}

/*
 * The completion sweep - the half of §8 a fixture cannot assert.
 *
 * A fixture holds a dozen positions somebody thought of. What it cannot say is that the cursor
 * sentinel is safe at *every* position, and that is the property worth having: a request arrives
 * wherever the caret happens to be - inside a string, in the middle of a type, on a comment, in a
 * declaration head - and a language server that crashes on one of them is a plugin that dies while
 * its user types. This is the same argument M7's truncation sweep made, and the same shape of
 * answer.
 *
 * Opt-in (`YanaLspTest sweep`) rather than part of the run, because a completion request is a whole
 * compile and there is one per byte. What it reports is a count per module: how many offsets were
 * answered, and how many produced items - so a change that quietly stops answering anywhere shows
 * up as a number rather than as silence.
 */
static bool runCompletionSweep(const String& root) {
    lsp::Session probe;

    auto opened = probe.open(stringView(root));
    if(opened.isErr()) {
        println("cannot open %@: %@", root, opened.unwrapErr());
        return false;
    }

    probe.compile();

    struct Fixture {
        StringId module = 0;
        String path;
        String text;
    };

    Array<Fixture> fixtures;
    for(auto& entry: probe.moduleMap.entries) {
        if(!entry.name) continue;

        auto text = probe.provider.getSource(entry.name);
        if(text.length == 0) continue;

        fixtures.push(Fixture { entry.name, ownedString(entry.path.ptr, entry.path.length),
                                ownedString(text.ptr, text.length) });
    }

    for(auto& fixture: fixtures) {
        U32 answered = 0, withItems = 0, largest = 0, signatures = 0;
        auto text = stringView(fixture.text);

        /*
         * Signature help at every position, out of the one compile the probe already did.
         *
         * It reads the ordinary program rather than compiling for itself, so it is thousands of
         * requests for the price of none - and it walks the same text scan the completion sweep
         * cannot reach, which is the half of §6's signature row that is about *positions*.
         */
        for(U32 offset = 0; offset <= text.length; offset++) {
            Net::Writer json(8192);
            Net::JsonWriter out(json);
            lsp::writeSignatureHelp(out, probe, fixture.module, offset, text);
            json.flush();

            if(json.getBuffered().length > 4) signatures++;
        }

        for(U32 offset = 0; offset <= text.length; offset++) {
            lsp::Session session;
            if(session.open(stringView(root)).isErr()) continue;

            CompletionRequest request;
            U32 prefixStart = offset;
            session.complete(fixture.module, offset, request, prefixStart);

            if(request.captured) answered++;
            if(request.items.size()) withItems++;
            if(request.items.size() > largest) largest = U32(request.items.size());
        }

        // The largest answer is worth reporting on its own: it is what the sort and the response
        // are sized by, and it grows with the program rather than with the file.
        println("%@: %@ of %@ offsets answered, %@ with items, largest %@, %@ signatures",
                fixture.path, answered, text.length + 1, withItems, largest, signatures);
    }

    return true;
}

static bool runSemanticFixture(const String& root, const String& expectPath, bool generate) {
    lsp::Session session;

    auto opened = session.open(stringView(root));
    if(opened.isErr()) {
        println("cannot open %@: %@", root, opened.unwrapErr());
        return false;
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
            return false;
        }

        return true;
    }

    print("Running test \"%@\"... ", expectPath);

    auto result = File::openFile(expectPath, readAccess());
    if(result.isErr()) {
        println("cannot open %@: error %@", expectPath, (U32)result.unwrapErr());
        return false;
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
        return false;
    }

    return true;
}

int main(int argc, const char** argv) {
    auto generate = false;
    auto sweep = false;
    Array<String> sweepRoots;

    for(int i = 1; i < argc; i++) {
        if(String(argv[i]) == "generate") generate = true;
        else if(String(argv[i]) == "sweep") sweep = true;
        else if(sweep) sweepRoots.push(String(argv[i]));
    }

    // `sweep` with no roots takes the three fixture projects. A root named on the command line is
    // how the sweep is pointed at a larger program, which is what it is for: the fixtures are small
    // by design and the positions that break a parser are in files nobody wrote for a test.
    if(sweep) {
        if(sweepRoots.size() == 0) {
            sweepRoots.push(String("lsp/complete"));
            sweepRoots.push(String("lsp/recover"));
            sweepRoots.push(String("lsp/semantic"));
        }

        auto swept = true;
        for(auto& root: sweepRoots) swept = runCompletionSweep(root) && swept;
        return swept ? 0 : 1;
    }

    Array<String> tests;
    listDirectory("lsp", [&](const String& name, bool isDirectory) {
        if(isDirectory || name == "." || name == "..") return;

        if(auto p = findLastChar(stringView(name), '.')) {
            String extension(p + 1, name.text() + name.size() - p - 1);
            if(extension == "yana") tests.push(String("lsp/") + name);
        }
    });

    // A driver that finds nothing to run has failed, whatever the fixtures would have said. This
    // one is invoked from the tree it reads relative to, so an empty list means the wrong working
    // directory far more often than it means the fixtures are gone.
    if(tests.size() == 0) {
        println("no tests found");
        return 1;
    }

    auto pass = true;

    // The whole-project pass, which is where everything past the position index is asserted. Twice:
    // once over a project that compiles, and once over one that is mid-edit, since what the second
    // asserts is that the first's answers survive a broken declaration above them.
    pass = runSemanticFixture(String("lsp/semantic"), String("lsp/semantic.expect"), generate) && pass;
    pass = runSemanticFixture(String("lsp/recover"), String("lsp/recover.expect"), generate) && pass;

    // Completion, which is a pass of its own because every marker is a compile of its own - §8.
    pass = runCompletionFixture(String("lsp/complete"), String("lsp/complete.expect"), generate) && pass;

    for(auto& test: tests) {
        auto result = File::openFile(test, readAccess());
        if(result.isErr()) {
            println("cannot open file %@: error %@", test, (U32)result.unwrapErr());
            pass = false;
            continue;
        }

        auto file = result.moveUnwrapOk();
        auto size = file.size();
        Ptr<char, HeapDeleter> buffer { (char*)hAlloc(size ? size : 1) };
        if(size) file.read({ (Byte*)buffer.get(), size });

        pass = runFixture(test, { buffer.get(), size }, generate) && pass;
    }

    // A summary line as well as the exit code. Every fixture here prints its whole answer on a
    // mismatch, so a failure two thousand lines up is not something a person scrolling back finds -
    // and "the goldens are green" was believed about this driver for long enough to let a real
    // regression sit in `recover.expect`, because it exited 0 either way.
    println(pass ? "\nAll LSP tests passed." : "\nSome LSP tests FAILED.");
    return pass ? 0 : 1;
}
