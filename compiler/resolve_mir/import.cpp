#include "resolve_internal.h"
#include "name.h"
#include "../parse/library.h"
#include "../intrinsic/role.h"

namespace mir {

/*
 * The import graph: which modules a program is made of, and what each of them can see.
 *
 * Two passes and a strict order between them. `discoverModules` walks the imports transitively and
 * creates a `Module` for everything reachable; `linkImports` then fills in the edges. They are
 * separate because a cycle has to have **one** meaning: an import that triggered the resolution of
 * what it named would give a two-module cycle two readings depending on which was compiled first,
 * and Analysis-Modules.md §2.2 is the ruling that it may not. Discovery finds every module before
 * any declaration is read, and the declaration passes then run over the whole program a pass at a
 * time - so the two directions of a cyclic edge see each other.
 */

/*
 * The `ast::ModuleGroup` behind each `mir::Module`, in the order the passes read them.
 *
 * Parallel to `Program::modules` rather than a field on `Module`, and that is the arena rule: a
 * declaration-region node may not hold a pointer into the parse region. The pairing is by index,
 * which discovery establishes by pushing to both at once.
 */
static Module* addModule(Program& program, ast::ModuleGroup& group) {
    auto module = program.addModule(group.name);
    module->fromLibrary = group.library;

    for(auto file: group.files) module->files.push(program.decl, file->name);
    return module;
}

void discoverModules(Context& context, Program& program, ast::ModuleGroup& root,
                     ast::ModuleProvider* provider, Array<ast::ModuleGroup*>& groups,
                     Buffer<ast::ModuleGroup*> testRoots) {
    groups.push(&root);
    addModule(program, root);

    /*
     * The modules that hold tests, seeded before the walk - Design-Test.md §3.4.
     *
     * A test module is not reached from the program it tests, because the dependency runs the other
     * way, so a walk from the root alone would compile the program and none of its tests. Seeded
     * *before* the walk so that they are ordinary members of it: what each of them imports is
     * discovered exactly as the root's imports are, which is why a test module's helpers need no
     * listing of their own.
     *
     * Empty in every build without `-test` - the driver is what fills it, and it fills it with the
     * modules that actually declare a `@test`.
     */
    for(auto group: testRoots) {
        if(group == &root || program.findModule(group->name)) continue;

        groups.push(group);
        addModule(program, *group);
    }

    /*
     * Core, before any import is walked and whether or not anything wrote one.
     *
     * It is the module every other one implicitly imports (`linkImports` below), so it has to be
     * *discovered* even by a program whose source names nothing - `[a]`, `[1, 2, 3]` and a string
     * literal are grammar rather than library, and a compilation that reached Core only when
     * something spelled it would answer for those in one program and not in another.
     *
     * Seeded here rather than pushed as an edge, because discovery is what creates a `Module` and
     * the edges are the next pass's. A root that *is* Core - which is how the library compiles
     * itself - finds itself already present and adds nothing.
     *
     * Silently absent where the library walk has no Core, which is not a broken installation to
     * report here: a compilation given a `provider` supplies its modules from memory and has no
     * library at all, and that is the arrangement every editor request uses.
     *
     * The root is first in the list and the library is read *before* it by every declaration pass;
     * the two are different orders on purpose, and `Resolver::eachFile` says why.
     */
    auto coreName = context.addUnqualifiedName(intrinsic::kCoreModuleName);
    if(auto found = program.findModule(coreName)) {
        program.core = found;
    } else if(auto source = program.libraryModules.find(context, coreName)) {
        groups.push(source);
        program.core = addModule(program, *source) - *program.decl;
    }

    /*
     * Breadth-first over the imports, with the list being appended to as it is walked.
     *
     * An index rather than a worklist beside the result, because the result *is* the worklist: a
     * module discovered twice would be a module resolved twice, and `Program::findModule` below is
     * what makes the second sighting an edge rather than a second declaration list.
     */
    for(Size i = 0; i < groups.size(); i++) {
        for(auto file: groups[i]->files) {
            for(auto imported: file->imports.contents(*context.parseRegion)) {
                if(imported.from == groups[i]->name) continue;
                if(program.findModule(imported.from)) continue;

                /*
                 * The project's own files first, and `lib/` second.
                 *
                 * That order is what lets a program shadow a library module by putting a file of
                 * that name in its own source tree, which is the direction that has to work: a
                 * library is shared and a program is not, so the one that can be changed to resolve
                 * a collision is the program, and it should not have to be changed by *renaming* the
                 * module it wanted.
                 *
                 * Silent on a miss. `linkImports` reports a name nothing answered against the import
                 * that wrote it, which is where it is readable.
                 */
                auto source = provider ? provider->getModule(imported.from) : nullptr;
                if(!source) source = program.libraryModules.find(context, imported.from);
                if(!source) continue;

                groups.push(source);
                auto added = addModule(program, *source);

                /*
                 * Whether the package that answered lets a consumer import this - decided here,
                 * where what answered is known, and reported at the import, where a person can act
                 * on it. See `Module::packagePrivate`.
                 *
                 * Not asked at all when this compilation *is* that package: `base` sees all of
                 * `base`, which is how the library's own modules reach each other and how its tests
                 * run.
                 */
                if(added->fromLibrary && !context.library.isOwnPackage(context, context.settings)) {
                    added->packagePrivate = !context.library.exportsModule(context, imported.from);
                }
            }
        }
    }
}

void linkImports(Context& context, Program& program, Buffer<ast::ModuleGroup*> groups) {
    auto base = *program.decl;

    for(U32 index = 0; index < groups.size(); index++) {
        auto group = groups[index];
        auto pointer = program.modules[index];
        auto& module = *base[pointer];

        /*
         * Core is visible everywhere without being written, and is the one module that does not
         * import itself.
         *
         * One implicit edge rather than several - Analysis-Modules.md §2.4. `[a]`, `[1, 2, 3]` and a
         * string literal are grammar rather than library, so what they mean has to be reachable
         * without being asked for; they are all Core's, so saying that takes one import.
         *
         * `Program::core` is null until something builds Core, which this stage does not do yet - so
         * the edge is dormant rather than absent, and the day Core arrives nothing here changes.
         */
        if(program.core && pointer != program.core) {
            Import core;
            core.module = program.core;
            core.localName = base[program.core]->name;
            core.file = Import::kEveryFile;
            module.imports.push(program.decl, core);
        }

        for(U16 file = 0; file < U16(group->files.size()); file++) {
            for(auto imported: group->files[file]->imports.contents(*context.parseRegion)) {
                /*
                 * A module importing itself asks for nothing: every declaration of the module is
                 * already visible to every file of it. Not a soundness question - a cycle between
                 * two modules has a meaning now, and it is the same meaning whichever of them was
                 * compiled first - so what is left is that the line says nothing.
                 */
                if(imported.from == module.name) {
                    context.diagnostics.error("a module cannot import itself"_v, imported.source);
                    continue;
                }

                auto target = program.findModule(imported.from);

                if(!target) {
                    // Discovery has already looked in the project's files and then in `lib/`, so a
                    // name still unanswered here is one neither had.
                    context.diagnostics.error("cannot find module %@"_v, imported.source,
                                              context.findName(imported.from));
                    continue;
                }

                /*
                 * A module of another package that its manifest does not export.
                 *
                 * Reported here rather than as "cannot find", because the two are different mistakes
                 * with different fixes: a name nothing answers is a typo or a missing dependency,
                 * and this is a name that exists and is not yours.
                 */
                if(base[target]->packagePrivate && !module.fromLibrary) {
                    context.diagnostics.error("%@ is not exported by its package - it exists, but the package's manifest does not list it, so it is that package's own business rather than part of what it offers"_v,
                                              imported.source,
                                              context.findName(imported.from));
                    continue;
                }

                auto localName = imported.localName ? imported.localName : imported.from;

                // Per file, because that is the scope an import is in: two files of one module
                // importing the same thing is two files each naming what it uses.
                auto duplicate = false;
                for(auto& existing: module.imports.contents(base)) {
                    if(!existing.inScope(file)) continue;
                    if(existing.module != target || existing.localName != localName) continue;

                    context.diagnostics.error("duplicate import of %@"_v, imported.source,
                                              context.findName(imported.from));
                    duplicate = true;
                    break;
                }

                if(duplicate) continue;

                Import entry;
                entry.module = target;
                entry.localName = localName;
                entry.qualified = imported.qualified;
                entry.file = file;

                for(auto symbol: imported.include.contents(*context.parseRegion)) {
                    entry.include.push(program.decl, symbol);
                }

                for(auto symbol: imported.exclude.contents(*context.parseRegion)) {
                    entry.exclude.push(program.decl, symbol);
                }

                // An import line is navigable. A module is a file rather than a declaration, so this
                // is a reference whose target is found by path - see `moduleSymbol`.
                recordReference(context, imported.source, moduleSymbol(base, target));

                module.imports.push(program.decl, entry);
            }
        }
    }
}

} // namespace mir
