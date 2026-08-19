/*
 * The import graph.
 *
 * What a module can see, resolved before anything in it is - see P0 item 4 in Analysis-Status.md
 * for why a cycle here has to have an order-independent meaning rather than whichever one the
 * first-visited module gave it.
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

void resolveImports(Module& module, ast::Module& ast, ModuleProvider* provider) {
    // Core is visible everywhere without being written, and is the one module that does not
    // import itself.
    if(module.program.core && &module != module.program.core) {
        auto& core = *module.imports.push();
        core.module = module.program.core;
        core.localName = module.program.core->name;
    }

    // Collections is visible everywhere for the same reason: `[a]` and `[1, 2, 3]` are grammar
    // rather than library, so what they mean has to be reachable without being asked for. It is
    // built after Core and Native, so neither of those is handed one of these.
    if(module.program.collections && &module != module.program.collections) {
        auto& collections = *module.imports.push();
        collections.module = module.program.collections;
        collections.localName = module.program.collections->name;
    }

    // And Text, on exactly the same terms: a string literal is grammar, so `Show`, `print` and the
    // rest of what a string can do have to be reachable without being asked for. It is built last of
    // all, so nothing it imports is handed one of these - which is what keeps the implicit import
    // from turning Collections and NativeText into a cycle. See Program::text.
    if(module.program.text && &module != module.program.text) {
        auto& text = *module.imports.push();
        text.module = module.program.text;
        text.localName = module.program.text->name;
    }

    for(auto imported: ast.imports.contents(module.parse)) {
        if(imported.from == module.name) {
            module.context.diagnostics.error("a module cannot import itself"_v, imported.source);
            continue;
        }

        auto target = module.program.findModule(imported.from);

        if(!target) {
            /*
             * The project's own files first, and `lib/` second - see findLibraryModule.
             *
             * That order is what lets a program shadow a library module by putting a file of that
             * name in its own source tree, which is the direction that has to work: a library is
             * shared and a program is not, so the one that can be changed to resolve a collision is
             * the program, and it should not have to be changed by *renaming* the module it wanted.
             *
             * Nothing is asked of the library for a name the program already answered, so a compile
             * whose imports are all its own never touches the library directory past the seven
             * modules built before any of this ran.
             */
            auto source = provider ? provider->getModule(imported.from) : nullptr;
            if(!source) source = findLibraryModule(module.context, imported.from);

            if(!source) {
                module.context.diagnostics.error("cannot find module %@"_v, imported.source,
                                                 module.context.findName(imported.from));
                continue;
            }

            target = module.program.addModule(imported.from, *source->region);
            resolveModuleDecls(*target, *source, provider);
        }

        /*
         * The indirect form of the self-import above, and the one that had no diagnostic at all.
         *
         * A module reached this way exists - it was interned before its declarations were resolved -
         * so nothing downstream can tell it apart from a finished one. What it holds is whichever of
         * its declarations came before the import that led back here, which makes every signature
         * resolved against it depend on the order the files were named in. Rejected rather than
         * accepted partially, because there is no reading of a cycle that is the same program twice.
         *
         * The import is dropped as well as reported, so that the rest of this module resolves
         * against a target that is absent rather than against one that is half there.
         */
        if(target->declState == Module::DeclState::Resolving) {
            module.context.diagnostics.error("%@ and %@ import each other, directly or through another module - a cycle has no order for their declarations to be resolved in, so the two would see different halves of one another depending on which was compiled first"_v,
                                             imported.source, module.context.findName(module.name),
                                             module.context.findName(imported.from));
            continue;
        }

        auto duplicate = false;
        for(auto& existing: module.imports) {
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

        // §1.2: an import line is navigable. The module has no location of its own - it is a file,
        // not a declaration - so this is a reference whose target is jumped to by *path* rather
        // than by position; the server turns the module name into a URI through the module map.
        recordReference(module.context, imported.source, moduleSymbol(*target));

        for(auto symbol: imported.include.contents(module.parse)) entry.include.push(symbol);
        for(auto symbol: imported.exclude.contents(module.parse)) entry.exclude.push(symbol);
    }
}
