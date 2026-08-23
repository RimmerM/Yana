/*
 * The import graph.
 *
 * What a module can see. P0 item 4 in Analysis-Status.md asked that a cycle here have an
 * order-independent meaning rather than whichever one the first-visited module gave it, and
 * rejecting the cycle was the answer while an import triggered its target's resolution. It no
 * longer does - Analysis-Modules.md §2.2 - so the answer is now that the passes give it one
 * meaning, and this file only links.
 */

#include "module_internal.h"
#include "analyze.h"
#include "const.h"
#include "core.h"
#include "expr.h"
#include "generic.h"
#include "host.h"
#include "index.h"
#include "name.h"
#include "native.h"
#include "verify.h"
#include "witness.h"
#include "../parse/ast.h"

/*
 * What one module can see.
 *
 * Every module of the program has been discovered and interned by the time this runs - see
 * `discoverModules` - so this only links, and the depth-first resolution that used to happen here is
 * gone with it. That is the whole of Analysis-Modules.md §2.2: an import no longer *triggers* the
 * resolution of what it names, so a cycle in the graph is no longer a program with two readings.
 *
 * The imports of every file land in one list, each tagged with the file that wrote it - §2.1.2's
 * "an import is written in a file and scoped to that file". See Import::file and Module::activeFile.
 */
void resolveImports(Module& module) {
    /*
     * Core is visible everywhere without being written, and is the one module that does not import
     * itself.
     *
     * One implicit edge rather than three - Analysis-Modules.md §2.4. `[a]`, `[1, 2, 3]` and a
     * string literal are grammar rather than library, so what they mean has to be reachable without
     * being asked for; they are all Core's now, so saying that takes one import. The three it
     * replaces had an ordering constraint between them that no longer exists: each was null until
     * its module had been built, which is what kept the modules the later ones imported from being
     * handed an import back.
     */
    if(module.program.core && &module != module.program.core) {
        auto& core = *module.imports.push();
        core.module = module.program.core;
        core.localName = module.program.core->name;
        core.file = Import::kEveryFile;
    }

    for(U16 index = 0; index < module.files.size(); index++) {
        auto file = module.files[index];

        for(auto imported: file->imports.contents(module.parse)) {
            /*
             * A module importing itself is still a mistake, and is no longer a soundness one.
             *
             * It used to sit beside the diagnostic for a cycle, which is deleted: a cycle between
             * two modules now has a meaning, and it is the same meaning whichever of them was
             * compiled first. What is left here is that importing yourself asks for nothing - every
             * declaration of the module is already visible to every file of it.
             */
            if(imported.from == module.name) {
                module.context.diagnostics.error("a module cannot import itself"_v, imported.source);
                continue;
            }

            auto target = module.program.findModule(imported.from);

            if(!target) {
                // Discovery has already looked in the project's files and then in `lib/`, so a name
                // still unanswered here is one neither had. Reported against the import that named
                // it, which is where it is readable.
                module.context.diagnostics.error("cannot find module %@"_v, imported.source,
                                                 module.context.findName(imported.from));
                continue;
            }

            // Per file, because that is the scope an import is in: two files of one module importing
            // the same thing is two files each naming what it uses, which is the point of §2.1.2.
            auto duplicate = false;
            for(auto& existing: module.imports) {
                if(!existing.inScope(index)) continue;

                if(existing.module == target && existing.localName == (imported.localName ? imported.localName : imported.from)) {
                    module.context.diagnostics.error("duplicate import of %@"_v, imported.source,
                                                     module.context.findName(imported.from));
                    duplicate = true;
                }
            }

            if(duplicate) continue;

            auto& entry = *module.imports.push();
            entry.module = target;
            entry.localName = imported.localName ? imported.localName : imported.from;
            entry.qualified = imported.qualified;
            entry.file = index;

            // §1.2: an import line is navigable. The module has no location of its own - it is a
            // file, not a declaration - so this is a reference whose target is jumped to by *path*
            // rather than by position; the server turns the module name into a URI through the
            // module map.
            recordReference(module.context, imported.source, moduleSymbol(*target));

            for(auto symbol: imported.include.contents(module.parse)) entry.include.push(symbol);
            for(auto symbol: imported.exclude.contents(module.parse)) entry.exclude.push(symbol);
        }
    }
}
