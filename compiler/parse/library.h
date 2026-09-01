#pragma once

#include "ast.h"

struct Context;

namespace ast {

/*
 * Where a module's files come from when it is not the library's - the project's own module map.
 *
 * An interface rather than the map itself, because the three things that resolve a program hold
 * three different maps: the driver walks a source tree, the language server lays unsaved buffers
 * over one, and a test driver answers out of a fixture directory. What each of them has in common is
 * this one question, and it is asked by both resolvers - which is why it is here, beside the answer
 * for the other place a module can come from, rather than inside either of them.
 */
struct ModuleProvider {
    virtual ~ModuleProvider() = default;
    virtual ModuleGroup* getModule(StringId name) = 0;
};

/*
 * The standard library's modules, parsed on demand.
 *
 * `LibrarySource` in `compiler/compiler` answers where a module's files *are*; this is the step
 * after it - parse each one, ask whether it joined the module the walk was looking for, and lay a
 * group over the ones that did. It lives here rather than in either resolver because both of them
 * need it and neither owns it: an `import` naming something the project's own tree has no file for
 * is answered out of `lib/`, and which stage is doing the asking has nothing to do with the answer.
 *
 * **Owning**, and that is why it is a struct rather than a free function. The ASTs have to outlive
 * every declaration resolved against them - a diagnostic quotes a location in one long after the
 * parse - so somebody has to hold them for the length of the compilation, and a caller that had to
 * remember to would be a caller that leaks the day it forgets.
 *
 * Cached by name, including the *negative* answer: a name the library has no files for is asked
 * about once per import that writes it, and each of those would otherwise be a directory walk.
 */
struct LibraryModules {
    LibraryModules() = default;
    LibraryModules(const LibraryModules&) = delete;
    ~LibraryModules();

    /*
     * The group for one library module, or null where the library has no files for it.
     *
     * Silent on a miss, deliberately, on the same terms `LibrarySource::source` is: a name that is
     * neither a project module nor a library one is an unresolved import, and the import statement
     * is where that is said. The caller that wants "the standard library is missing" said instead
     * knows it is asking for a module every installation has, which this cannot know.
     */
    ModuleGroup* find(Context& context, StringId name);

private:
    // One asked-for module and what the library had for it, `group` null where it had nothing. The
    // asts are held here rather than on the group because a file parsed while looking for `Core` may
    // turn out to have joined something else, and it is still a file this compilation parsed.
    struct Entry {
        StringId name;
        ModuleGroup* group = nullptr;
    };

    Array<Entry> entries;
    Array<Module*> asts;
    Array<ModuleGroup*> groups;
};

} // namespace ast
