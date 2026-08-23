#pragma once

#include "module.h"
#include "../parse/ast.h"

/*
 * The Core module.
 *
 * Core is an ordinary module - it has a name, declarations, classes and instances, and every
 * other module reaches it through the same import machinery it would use for any other. What it
 * does not have is a place in the program's own source tree: its declarations are read from
 * `lib/Core/` beside the compiler, and its primitive instances are generated directly, because
 * `Int`'s `+` cannot be written in terms of anything more basic.
 *
 * Those generated instances are real functions with real bodies, and they also carry an
 * intrinsic hook, so an ordinary call to `+` expands to the one instruction it contains instead
 * of to a call some later pass would have to inline. Nothing about the call site knows this: it
 * selects a Num instance exactly as it would for a user-defined type.
 *
 * The four hooks below are what the compiler supplies, in the two positions it can supply them
 * from - Analysis-Modules.md §2.4. `definePreludeTypes` runs before any of Core's source is read,
 * because the five primitives and the fixed-width family are what its declarations are written in
 * terms of; the other three run once every file of Core *and* Native has been through the
 * declaration passes, because each needs a class, a record or a signature that some file of either
 * declares. Nothing needs a position in between, which is why the prelude is one pass sequence with
 * a hook at each end rather than six modules in dependency order.
 */
void definePreludeTypes(Program& program, Module& core, TypeList& widthTypes);

/// The declarations the compiler itself names - the classes the language's syntax is written in
/// terms of, the exit signal a continuation reports with, and the two container records. Lookups
/// only, so it runs early enough for the signature passes to see what they record.
void definePreludeLookups(Program& program, Module& core);

/// Core's own instances - the numeric tower, the conversion ladders, the bit families, and the
/// classes the language's own syntax is written in terms of.
void definePreludeCore(Program& program, Module& core, TypeList& widthTypes);

/// The container half: `Array(a)` and `Map(k, v)` as the compiler knows them, the subscript check,
/// the generated container instances and the bulk operations.
void definePreludeContainers(Program& program, Module& core);

/// The text half: the three functions a format expression is built out of.
void definePreludeText(Program& program, Module& core);

/*
 * Reads and parses every file of one standard library module.
 *
 * The one place `lib/` is turned into ASTs, shared by the two modules the compiler builds itself
 * and by every module an `import` names. What it replaces is the four near-identical lex-and-parse
 * blocks that each of the `define*` functions carried while the source was a string literal beside
 * them.
 *
 * `allowSignatures` is the one thing the callers disagree about: the prelude declares functions with
 * no body - a pointer intrinsic, a host hook, a bulk operation whose implementation the compiler
 * chooses - and a declaration with no body is a parse error anywhere else.
 *
 * Null where the library has no such module, and silent about it: the two callers below want
 * different things said.
 */
ast::ModuleGroup* parseLibraryGroup(Program& program, StringId name, bool allowSignatures);

/*
 * The same lookup for a module an `import` named, and silent when the library has no files for it.
 *
 * This is what makes `lib/` a place modules are *found* rather than a place the prelude happens to
 * be stored. `resolveImports` asks the project's own module map first and asks here second, so a
 * library module that nothing implicitly imports - `Math`, `File`, anything added to `lib/` past the
 * prelude - costs nothing until a program writes its name, and then resolves through exactly the
 * machinery a module in the program's own source tree would.
 *
 * The project wins where both have the name, which is the order that lets a program override a
 * library module by putting a file of that name in its own tree. Silent on a miss because the
 * import statement is where an unresolvable name is reported, and it reports it once.
 */
ast::ModuleGroup* findLibraryModule(Program& program, StringId name);

/*
 * And for a module of the prelude, which reports that it could not be read.
 *
 * A broken installation rather than a broken program, so it is said in those terms and names the
 * flag that fixes it.
 */
ast::ModuleGroup* parsePreludeGroup(Program& program, StringView name);
