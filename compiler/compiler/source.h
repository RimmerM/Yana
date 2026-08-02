#pragma once

#include "../parse/ast.h"
#include "../resolve/module.h"
#include "settings.h"

/*
 * One source file, and what has been made of it.
 *
 * The buffer holds the path and the identifier the file's location produced - see mapFile - and the
 * text is the file's contents, kept for as long as anything can still quote it: a diagnostic
 * reported while resolving names in module A can point at a line of module B.
 */
struct SourceEntry {
    SourceEntry(StringView path, Identifier id, Ptr<char> buffer): path(path), id(id), buffer(::move(buffer)) {}
    SourceEntry(SourceEntry&& other) noexcept = default;

    StringView path;
    Identifier id;
    Ptr<char> buffer;

    // The interned form of `id`, filled once there is a context to intern it into. This is what a
    // module is named by everywhere past the file system - an `import` names it, and so does the
    // resolver when it asks for a module it has not seen.
    StringId name = 0;

    Ptr<char, HeapDeleter> text;
    Size length = 0;

    Ptr<ast::Module> ast;
};

struct ModuleMap {
    Array<SourceEntry> entries;

    SourceEntry* find(StringId name);
    SourceEntry* find(const String& identifier);
};

Result<void, String> buildModuleMap(ModuleMap& map, const String& root);
Result<void, String> buildModuleMap(ModuleMap& map, const CompileSettings& settings);

/*
 * What the compiler reads source through.
 *
 * Both halves are asked the same question from opposite ends: the resolver asks for the parsed form
 * of a module an `import` names, and a diagnostic asks for the text of the module a location is in.
 * One object answers both because both answers come from the same file, and reading it twice would
 * be two chances to disagree about what it says.
 */
struct FileProvider: ModuleProvider, SourceProvider {
    FileProvider(ModuleMap& map): moduleMap(map) {}

    // Interns every mapped file's identifier, which is what makes a module findable by the name an
    // `import` uses. Runs once the context exists, since interning needs one.
    void prepare(Context& context);

    StringView getSource(StringId module) override;
    const Location* getNode(LocationId id) override;
    ast::Module* getModule(StringId name) override;

    // Reads and parses one entry, or answers what was parsed before.
    ast::Module* parse(SourceEntry& entry);

    // Drops every parsed AST and every loaded buffer, so that the next compile starts from the
    // files - or, for an overlay provider, from the editor's buffers.
    //
    // A batch compile never calls this. A server does, on every compile, and it has to: an AST
    // holds LocationIds, which index one Context's location array in the order that context
    // created them, so an AST parsed against one context means something else against the next.
    // The text goes with it because an entry holds no record of where its text came from, which
    // makes "reload everything" the only answer that cannot be stale.
    void reset();

    // Puts one entry's text where `parse` will find it. The base reads the file; the overlay
    // provider answers from the editor's unsaved buffer instead - Implementation-Tooling.md §5.1.
    // Returns false when the text cannot be obtained, having reported why.
    virtual bool loadText(SourceEntry& entry);

    ModuleMap& moduleMap;
    Context* context = nullptr;
};
