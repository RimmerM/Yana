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
    SourceEntry(StringView path, Identifier id, U32 directoryLength, U32 root, Ptr<char> buffer):
        path(path), id(id), buffer(::move(buffer)), directoryLength(directoryLength), root(root) {}
    SourceEntry(SourceEntry&& other) noexcept = default;

    StringView path;
    Identifier id;
    Ptr<char> buffer;

    // The interned form of `id`, filled once there is a context to intern it into. This names the
    // *file* and not the module it belongs to - see `module` below, and ast::Module::name. It is
    // what a LocationId is quoted through and what a language server turns back into a URI.
    StringId name = StringId();

    /*
     * The module this file belongs to, decided once every file has been parsed - Analysis-Modules.md
     * §2.1. A file that wrote nothing belongs to its directory's module; `module` makes it a module
     * of its own; `module M` joins M.
     *
     * Null until `FileProvider::prepare` has run, because the answer is in the file.
     */
    StringId module = StringId();

    Ptr<char, HeapDeleter> text;
    Size length = 0;

    Ptr<ast::Module> ast;

    // Whether the file's name carried the `test` selector, kept from the walk that read it and
    // copied onto the AST by `FileProvider::parse` - see ast::Module::test.
    bool test = false;

    // How much of `id` names the file's directory: the text up to its last segment. Zero for a file
    // sitting directly in a compile root, whose directory is the root and is named by `root`.
    U32 directoryLength = 0;

    // Which compile root this file was found under - an index into ModuleMap::roots.
    U32 root = 0;
};

/*
 * One directory or file the compiler was pointed at.
 *
 * The name matters because a directory is now a module (§2.1): the files sitting directly in a
 * compile root form one module, and nothing in their paths says what it is called - their
 * path-derived names are relative to the root and so have the root's own name cut off. So the root
 * carries it, and it is the directory's basename.
 */
struct SourceRoot {
    String path;
    String name;

    // The root was named as a file rather than as a directory. Such a file is a module of its own
    // whatever it writes, since it has no directory of the compilation's to belong to.
    bool isFile = false;
};

/*
 * The files of one module - Analysis-Modules.md §2.1.
 *
 * `files` is in path order, which is what makes the declaration passes over a module deterministic.
 * `root` marks the group formed by a compile root's own directory, which is what a program starts
 * from when nothing named a root module.
 */
struct ModuleGroup {
    StringId name;
    String text;

    // Indices into `ModuleMap::entries`. `SmallArray` on the same terms as the two lists of files it
    // ends up building - util/README.md - since a source tree has one of these per directory and
    // most directories hold a handful of files.
    SmallArray<Size, 8> files;
    bool root = false;

    ast::ModuleGroup parsed;
};

struct ModuleMap {
    Array<SourceEntry> entries;
    Array<SourceRoot> roots;

    /*
     * What this compilation is, for the file-name selectors - see `selectsFile` in source.cpp.
     *
     * A pointer and nullable, because a map can be built from a bare root path with no settings
     * anywhere (`buildModuleMap(map, root)`, which the tools use): there is nothing to select
     * against then, and every file is a file of its module - which is what that overload always did.
     */
    const CompileSettings* settings = nullptr;

    // Filled by FileProvider::prepare, which is the first point at which a file's module is known.
    Array<ModuleGroup> groups;

    SourceEntry* find(StringId name);
    SourceEntry* find(const String& identifier);

    ModuleGroup* findGroup(StringId name);
    ModuleGroup* findGroup(const String& identifier);
    ModuleGroup* rootGroup();
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

    /*
     * Interns every mapped file's identifier, parses every mapped file, and groups the files into
     * modules - Analysis-Modules.md §2.1. Runs once the context exists, since all three need one.
     *
     * Parsing is eager here and it has to be, because which module a file belongs to is written in
     * the file. A directory's module is its files minus the ones that opted out, and "opted out" is
     * a declaration - so there is no answering "what is in module `Core`" without having read every
     * file that could be in it. What that costs is that a file nothing imports is now parsed, and
     * reports its syntax errors, where before it was ignored.
     */
    void prepare(Context& context);

    StringView getSource(StringId module) override;
    const Location* getNode(LocationId id) override;
    ast::ModuleGroup* getModule(StringId name) override;

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
