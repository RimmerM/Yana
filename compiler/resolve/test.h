#pragma once

#include "type.h"

namespace ast {
struct Decl;
struct ModuleGroup;
}

// `Program`, `Module` and `RecordType` come from type.h.
struct Function;

/*
 * `@test` - Design-Test.md §11.2's F1.
 *
 * The whole of the test framework's compiler half: what a `@test` declaration records (TestRegistry
 * below), reading the attribute off one (readTestAttribute), and synthesizing the entry that runs
 * them (resolveTestEntry).
 *
 * The registry is the compiler's because it cannot be the library's: there is no static
 * initializer, no linker section and no reflection in this language for a `Test` module to register
 * from, and a *written* registration list is worse than either - every test is then written twice
 * and the second half is invisible when it is missing, which is the failure mode the whole document
 * is about.
 *
 * Nothing here is a new mechanism. The attribute reads like every other attribute, the entry is
 * synthesized exactly as `resolveProgramEntry` already synthesizes one for a root module's
 * top-level statements, and which files a test build even contains is the `test` file selector
 * rather than a rule of its own.
 */

/*
 * One `@test` declaration, as the attribute pass read it.
 *
 * The `Test.Case` the entry builds out of this holds the same five things, which is not a
 * coincidence - the record is the contract, and this is what the compiler can fill it from.
 */
struct TestDeclaration {
    ModulePtr<Function> function = nullptr;

    // The display name - `@test("a key inserted twice keeps the last value")` - or the
    // declaration's own name where none was written. A test name is prose and reads better than it
    // computes, which is why the examples are `snake_case` and why this is a string rather than a
    // symbol.
    StringId name {};

    // Which module it was written in, so a report can group by it, and where, so a failure can be
    // jumped to. The site is the *declaration's* rather than any assertion's: `@caller` (caller.h)
    // is what gives an assertion its own.
    StringId module {};
    LocationId source = kNullLocation;

    // `@test(aborts)`: the case is expected to stop the process, so the harness runs it in a child
    // and reads the status rather than a report line. §5.3.
    bool aborts = false;
};

/*
 * Everything one compilation knows about its tests, which is a list and the two library names the
 * entry is built out of.
 *
 * A member of `Program` rather than a global, on the same terms as every other program-wide table:
 * two compilations in one process are two programs, and a registry that outlived one of them would
 * be a suite that grows every time the language server recompiles.
 *
 * Empty in every build without `-test`, where a `@test` is a diagnostic rather than an entry.
 */
struct TestRegistry {
    // Filled by readTestAttribute as each signature is resolved, in the order they were read, and
    // read once by resolveTestEntry.
    Array<TestDeclaration> declarations;

    /*
     * `Test.Case` and `Test.runMain`, found by name - the mechanism `Program::checkFailed` already
     * uses for `Core.Check.checkFailed`, applied to a module that is not part of the prelude.
     *
     * Null in a build that never asked for them, and null under `-test` in a program that cannot
     * reach the `Test` module - which is a diagnostic naming the missing module rather than an
     * entry that quietly runs nothing.
     */
    GlobalPtr<RecordType> caseType = nullptr;
    ModulePtr<Function> runMain = nullptr;
};

/*
 * `@test`, and the one attribute that adds a declaration to a list rather than setting a flag on
 * one. See the definition for the three forms and the signature rules.
 */
void readTestAttribute(Module& module, const ast::Decl& decl, Function& function);

// The entry of a test build: one array of `Test.Case` and one call. Replaces `resolveProgramEntry`'s
// ordinary search for `main` - a test build runs tests.
void resolveTestEntry(Program& program);

/*
 * Whether any file of this module declares a `@test` - asked of the parse tree, before anything is
 * resolved.
 *
 * A test module is not reached from the program it tests, because the dependency runs the other
 * way, so under `-test` something has to name the ones that are part of the compilation anyway.
 * This is that test, and it is deliberately the narrowest one that works: a module with no `@test`
 * in it contributes nothing to a suite, and a test module's *helpers* are reached from it by
 * ordinary imports like anything else. See resolveProgram's `testRoots`.
 */
bool moduleDeclaresTests(Context& context, ast::ModuleGroup& group);
