#include "library.h"
#include "parser.h"

namespace ast {

// One library file, parsed. Null where the library could not read it - which is not the same as the
// module not existing, and is reported by neither: `LibrarySource` has already logged what it could
// not read, and a module with no readable files is a module with no declarations.
static Module* parseLibrarySource(Context& context, StringId id) {
    auto source = context.library.source(context, id);
    if(source.length == 0) return nullptr;

    Lexer lexer(context, context.diagnostics, source, id);
    Parser parser(context, lexer, id);

    return new Module(parser.parseModule());
}

/*
 * Whether a file the library walk turned up is a file of this module.
 *
 * The same three cases the project's own module map decides, and it has to be decided after the
 * parse because it is written in the file. A file directly in the module's directory that said
 * nothing is a file of it; a file in a subdirectory that said nothing belongs to that subdirectory's
 * module, and is left for whoever imports it; `module` alone names the file's own path, which is a
 * module of one file where `Math.yana` and `File.yana` sit and is standing apart from its siblings
 * where `Core/Sort.yana` would; and `module M` joins M, which is what `Core/Float/*.yana` write.
 *
 * The prefix restriction the project side reports on is not checked here. The library's files are
 * this repository's own, so a name that does not fit is a mistake in the library rather than in the
 * program being compiled, and it shows up as the module missing declarations rather than as a report
 * against source the author did not write.
 */
static bool joinsModule(StringId module, LibraryFile file, const Module& ast) {
    switch(ast.membership) {
        // `module` names the file's own path, so it is this module exactly when the two are the same
        // name - which is what `Math.yana` and `File.yana` are, and what `Core/Sort.yana` would not be.
        case Membership::Own: return file.name == module;
        case Membership::Named: return ast.joins == module;
        case Membership::Directory: return file.inDirectory;
    }

    return false;
}

LibraryModules::~LibraryModules() {
    for(auto group: groups) delete group;
    for(auto ast: asts) delete ast;
}

ModuleGroup* LibraryModules::find(Context& context, StringId name) {
    // The cache, and it answers a miss as well as a hit - see the header. A second `import Math` in
    // a program is a second walk of `lib/` only if this forgets what the first one found.
    for(auto& entry: entries) {
        if(entry.name == name) return entry.group;
    }

    auto& entry = *entries.push();
    entry.name = name;

    auto files = context.library.files(context, name);
    if(files.isEmpty()) return nullptr;

    auto group = new ModuleGroup { .name = name, .library = true };
    groups.push(group);

    for(auto file: files) {
        auto ast = parseLibrarySource(context, file.name);
        if(!ast) continue;

        ast->test = file.test;

        asts.push(ast);
        if(joinsModule(name, file, *ast)) group->files.push(ast);
    }

    // A group with nothing in it is nothing to import, and answering one would make an unresolvable
    // import into a module that exists and declares nothing - two failures instead of one.
    if(group->files.isEmpty()) return nullptr;

    entry.group = group;
    return group;
}

} // namespace ast
