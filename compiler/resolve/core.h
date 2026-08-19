#pragma once

#include "module.h"
#include "../parse/ast.h"

/*
 * The Core module.
 *
 * Core is an ordinary module - it has a name, declarations, classes and instances, and every
 * other module reaches it through the same import machinery it would use for any other. What it
 * does not have is a place in the program's own source tree: its declarations are read from
 * `lib/Core.yana` beside the compiler, and its primitive instances are generated directly, because
 * `Int`'s `+` cannot be written in terms of anything more basic.
 *
 * Those generated instances are real functions with real bodies, and they also carry an
 * intrinsic hook, so an ordinary call to `+` expands to the one instruction it contains instead
 * of to a call some later pass would have to inline. Nothing about the call site knows this: it
 * selects a Num instance exactly as it would for a user-defined type.
 */
void defineCore(Program& program);

/*
 * Reads and parses one standard library module, or reports that it could not be found.
 *
 * The one place `lib/` is turned into an AST, shared by all seven of the modules the compiler
 * builds itself. What it replaces is the four near-identical lex-and-parse blocks that each of the
 * `define*` functions carried while the source was a string literal beside them.
 *
 * `allowSignatures` is the one thing the callers disagree about: every library module except `Text`
 * declares at least one function with no body - a pointer intrinsic, a host hook, a bulk operation
 * whose implementation the compiler chooses - and a declaration with no body is a parse error
 * anywhere else.
 *
 * Null with a diagnostic reported when the file is missing. That is a broken installation rather
 * than a broken program, so it is said in those terms and names the flag that fixes it.
 */
ast::Module* parseLibraryModule(Context& context, StringView name, bool allowSignatures);

/*
 * The same lookup for a module an `import` named, and silent when the library has no file for it.
 *
 * This is what makes `lib/` a place modules are *found* rather than a place seven of them happen to
 * be stored. `resolveImports` asks the project's own module map first and asks here second, so a
 * library module that nothing implicitly imports - anything added to `lib/` past the seven the
 * compiler builds itself - costs nothing until a program writes its name, and then resolves through
 * exactly the machinery a module in the program's own source tree would.
 *
 * The project wins where both have the name, which is the order that lets a program override a
 * library module by putting a file of that name in its own tree. Silent on a miss because the
 * import statement is where an unresolvable name is reported, and it reports it once.
 */
ast::Module* findLibraryModule(Context& context, StringId name);

/*
 * The Collections module.
 *
 * Built the same way as Core and for the same reason, except that it is written entirely in the
 * language: the growable array `[a]` needs raw pointers and the heap, which are Native's, and
 * Native imports Core - so this is a third module rather than more of the first. It is implicitly
 * imported, because the grammar produces `[a]` types and array literals whether or not a program
 * asked for them.
 */
void defineCollections(Program& program);

// `Text` - String's operations, over what NativeText hands out. Implicitly imported, and defined
// last for that reason: see Program::text.
void defineText(Program& program);
