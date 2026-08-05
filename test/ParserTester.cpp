// Standalone parser test driver.
//
// The full Yana compiler (resolve/lower/codegen) is mid-migration and does not build
// end-to-end today; this driver only exercises the parser stage (lexer -> parser ->
// AST printer), which is self-contained and independently testable. See
// compiler/parse/{lexer,parser,ast,ast_print}.* and the YanaParse CMake target.
#include <Core.h>
#include <File.h>
#include "../compiler/parse/parser.h"
#include "../compiler/parse/ast_print.h"
#include "shard.h"
#include "Net/Stream.h"
#include "Net/File.h"

using namespace Tritium;

struct TestProvider: SourceProvider {
    StringView source;
    Context* context = nullptr;

    StringView getSource(StringId module) override {
        return source;
    }

    const Location* getNode(LocationId id) override {
        return context ? context->getLocation(id) : nullptr;
    }
};

// Collects diagnostics as text instead of printing them, so that a fixture can assert on what
// the parser reports. The message alone is not enough for a negative test to be worth much: a
// diagnostic that points at the wrong place is as broken as a missing one, so the line, column
// and source range it covers are part of the compared text. A node whose location was never
// recorded shows up as `<no location>`.
struct TestDiagnostics: Diagnostics {
    using Diagnostics::Diagnostics;

    Array<String> messages;

    void message(Level level, StringView text, const Location* where) override {
        Diagnostics::message(level, text, where);

        const char* kind;
        switch(level) {
            case ErrorLevel: kind = "error"; break;
            case WarningLevel: kind = "warning"; break;
            case MessageLevel:
            default: kind = "message";
        }

        char buffer[4096];
        Size length;

        if(where) {
            length = format(toBuffer(buffer), toString("%@ %@:%@ [%@..%@]: %@"_v), kind, where->sourceStart.line + 1,
                            where->sourceStart.column, where->sourceStart.offset, where->sourceEnd.offset, text);
        } else {
            length = format(toBuffer(buffer), toString("%@ <no location>: %@"_v), kind, text);
        }

        messages.push(ownedString(buffer, length));
    }
};

// The diagnostics a module produced, followed by its AST. Tests that parse cleanly write no
// diagnostics section at all, so their expect files contain the AST and nothing else.
static void writeModule(Net::Writer& writer, Context& context, TestDiagnostics& diagnostics, ast::Module& ast) {
    for(auto& message: diagnostics.messages) {
        writer.writeString(message);
        writer.writeString("\n"_v);
    }

    if(diagnostics.messages.size()) {
        writer.writeString("\n"_v);
    }

    printModule(writer, context, *ast.region, ast);
}

void parserTest(const String& path, StringView content) {
    print("Running test \"%@\"...", path);

    TestProvider provider;
    provider.source = content;

    TestDiagnostics diagnostics(provider);
    Context context(diagnostics);
    provider.context = &context;

    auto name = context.addUnqualifiedName("no_name", 7);
    Lexer lexer(context, context.diagnostics, content, name);
    Parser parser(context, lexer, name);
    auto ast = parser.parseModule();

    Net::Writer writer(16384);
    writeModule(writer, context, diagnostics, ast);

    auto expectPath = path + String(".expect");
    auto file = tryResultOr(File::openFile(expectPath, readAccess()), {
        logError("cannot open file %@: error %@", expectPath, it.unwrapErr());
        return;
    });

    auto size = file.size();
    Ptr<char, HeapDeleter> buffer { (char*)hAlloc(size) };
    file.read({ (Byte*)buffer.get(), size });

    auto string = writer.getBuffered();
    auto equal = size == string.length && compareMem(buffer.get(), string.ptr, size) == 0;
    if(equal) {
        println("Pass.");
    } else {
        println("Fail. Got:");
        print(StringView { (char*)string.ptr, string.length });
        println("\n\n\nExpected:");
        print(StringView { buffer.get(), size });
        print("\n\n\n");
    }
}

void generateParserTest(const String& path, StringView content) {
    logInfo("Generating expect file for test \"%@\"", path);

    TestProvider provider;
    provider.source = content;

    TestDiagnostics diagnostics(provider);
    Context context(diagnostics);
    provider.context = &context;

    auto name = context.addUnqualifiedName("no_name", 7);
    Lexer lexer(context, context.diagnostics, content, name);
    Parser parser(context, lexer, name);
    auto ast = parser.parseModule();

    try {
        Net::FileStream file;
        file.open(path + ".expect", writeAccess(), File::CreateAlways);

        Net::Writer writer(Net::WriteStream(file), 16384);
        writeModule(writer, context, diagnostics, ast);
    } catch(const Net::Exception& e) {
        logError("Cannot create expect file for \"%@\": %@", path, e.description);
    }
}

/*
 * Every prefix of a fixture, parsed - and nothing compared.
 *
 * A truncated file is a file the editor sees on every keystroke, and what a fixture cannot assert
 * about one is that parsing it *finishes*. Two properties are checked here that nothing else can be:
 * that the lexer always consumes what it reports (the assertion at the end of `Lexer::next`, which
 * a non-advancing scanner trips immediately instead of hanging), and that no parser loop spins on a
 * token it will not eat.
 *
 * This is deliberately not M7's truncation sweep, which appends a declaration after the cut and asks
 * whether it survives. Nothing is appended here: the file *ends* mid-construct, which is the state
 * that found an identifier at the end of the buffer leaving the lexer where it started - and which
 * a sweep that always writes a trailing newline can never produce.
 *
 * Cheap enough to be part of the run: parsing is the only stage involved, so the whole fixture
 * directory is a fraction of a second.
 */
static void truncationTest(const String& path, StringView content) {
    print("Truncations of \"%@\"...", path);

    for(Size length = 0; length <= content.length; length++) {
        TestProvider provider;
        provider.source = StringView { content.ptr, length };

        // Collected rather than printed: a prefix of a program is broken almost by definition, and
        // what is being asserted is that the parser reaches the end of it at all.
        TestDiagnostics diagnostics(provider);
        Context context(diagnostics);
        provider.context = &context;

        auto name = context.addUnqualifiedName("no_name", 7);
        Lexer lexer(context, context.diagnostics, provider.source, name);
        Parser parser(context, lexer, name);
        parser.parseModule();
    }

    println(" %@ parsed.", content.length + 1);
}

/*
 * `shard:i/n` - run the fixtures whose index is `i` modulo `n`, and no others.
 *
 * The truncation test parses every prefix of every fixture, which is most of this driver's time and
 * is quadratic in how long the fixtures are rather than how many there are. Nothing about it is
 * shared between fixtures, so the way to stop paying for it in wall time is to run the fixtures at
 * once rather than to check fewer cut points: the properties it asserts - that the lexer always
 * advances, that no parser loop spins - are about *every* cut, and one that is not tried is one
 * where the parser can still hang.
 *
 * Round-robin rather than contiguous blocks, because a directory listing is alphabetical and the
 * long fixtures do not distribute themselves evenly through one.
 */
void testParser(bool generate, U32 shard, U32 shards) {
    Array<String> tests;

    listDirectory("parser", [&](const String& name, bool isDirectory) {
        if(!isDirectory && name != ".." && name != ".") {
            if(auto p = findLastChar(stringView(name), '.')) {
                String extension(p + 1, name.text() + name.size() - p - 1);
                if(extension == "yana") {
                    tests.push(String("parser/") + name);
                }
            }
        }
    });

    if(tests.size() == 0) {
        println("no tests found");
    }

    if(shards > 1) {
        Array<String> mine;
        for(U32 i = 0; i < tests.size(); i++) {
            if(i % shards == shard) mine.push(tests[i]);
        }

        tests = ::move(mine);
    }

    for(auto& test: tests) {
        auto result = File::openFile(test, readAccess());
        if(result.isErr()) {
            println("cannot open file %@: error %@", test, (U32)result.unwrapErr());
            continue;
        }

        auto file = result.moveUnwrapOk();
        auto size = file.size();

        Ptr<char, HeapDeleter> buffer { (char*)hAlloc(size) };
        file.read({ (Byte*)buffer.get(), size });

        if(generate) {
            generateParserTest(test, { buffer.get(), size });
        } else {
            parserTest(test, { buffer.get(), size });
            truncationTest(test, { buffer.get(), size });
        }
    }
}

/*
 * The same check over a wider corpus - `YanaParseTest sweep`.
 *
 * The fixtures in `parser/` are written to exercise the parser and are therefore small. The ones in
 * `resolve/` are written to exercise everything after it, which makes them the longest and most
 * varied Yana in the tree and the better corpus for a property that is about *positions*. Opt-in
 * rather than part of the run only because of the size: it is the same work, an order of magnitude
 * more of it.
 */
static void truncationSweep() {
    Array<String> files;

    auto collect = [&](const char* directory) {
        listDirectory(String(directory), [&](const String& name, bool isDirectory) {
            if(isDirectory) return;

            if(auto p = findLastChar(stringView(name), '.')) {
                String extension(p + 1, name.text() + name.size() - p - 1);
                if(extension == "yana") files.push(String(directory) + String("/") + name);
            }
        });
    };

    collect("parser");
    collect("resolve");
    collect("lsp/complete/src");
    collect("lsp/semantic/src");
    collect("lsp/recover/src");

    for(auto& file: files) {
        auto result = File::openFile(file, readAccess());
        if(result.isErr()) continue;

        auto opened = result.moveUnwrapOk();
        auto size = opened.size();
        Ptr<char, HeapDeleter> buffer { (char*)hAlloc(size ? size : 1) };
        if(size) opened.read({ (Byte*)buffer.get(), size });

        truncationTest(file, { buffer.get(), size });
    }
}

int main(int argc, const char** argv) {
    bool generateExpects = false;
    bool sweep = false;
    U32 shard = 0;
    U32 shards = 1;

    for(int i = 1; i < argc; i++) {
        auto arg = String(argv[i]);
        if(arg == "generate") generateExpects = true;
        if(arg == "sweep") sweep = true;
        parseShard(arg, shard, shards);
    }

    if(sweep) {
        truncationSweep();
        return 0;
    }

    testParser(generateExpects, shard, shards);
}
