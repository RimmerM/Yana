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
 * this one question, which the resolver asks - which is why it is here, beside the answer for the
 * other place a module can come from, rather than inside the resolver or any one of the three.
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
 * group over the ones that did. It lives here rather than in the resolver because the question is
 * about files and their parse rather than about resolution: an `import` naming something the
 * project's own tree has no file for is answered out of `lib/`, and what is doing the asking has
 * nothing to do with the answer.
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

/*
 * Whether any file of this module declares a `@test` - asked of the parse tree, before anything is
 * resolved.
 *
 * A test module is not reached from the program it tests, because the dependency runs the other
 * way, so under `-test` something has to name the ones that are part of the compilation anyway.
 * This is that test, and it is deliberately the narrowest one that works: a module with no `@test`
 * in it contributes nothing to a suite, and a test module's *helpers* are reached from it by
 * ordinary imports like anything else. The resolver takes its answer as its `testRoots`.
 *
 * Here rather than in the resolver, for the reason `ModuleProvider` is: the driver asks it before
 * anything is resolved, and the question is about the parse. It lived in `compiler/resolve/test.cpp`
 * until J150, when that stage was not the default and every `-test` build called into it for this;
 * the stage itself was deleted in J173.
 */
bool moduleDeclaresTests(Context& context, ModuleGroup& group);

} // namespace ast
