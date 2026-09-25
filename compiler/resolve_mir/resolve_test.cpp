#include "resolve_internal.h"
#include "gen.h"
#include "../parse/ast.h"

namespace mir {

/*
 * `@test` and the entry a test build runs - Design-Test.md §11.2's F1.
 *
 * Two halves and one file. Reading the attribute puts a declaration in `Program::tests`; building
 * the entry turns that list into an array of the library's `Test.Case` and one call of the library's
 * `Test.runMain`. There is no third piece: what a case *is*, how a failure is reported, what
 * `--isolate` does and how an `aborts` case is run are the library's, in `lib/Test/`, and nothing
 * here knows any of it.
 *
 * **What the compiler contributes is the list**, and it has to: there is no static initializer, no
 * linker section and no reflection in this language for a `Test` module to register from, and a
 * *written* registration list means every test is written twice with the second half invisible when
 * it is missing - which is the failure the whole framework exists to prevent.
 *
 * **Nothing here is a new mechanism.** The entry is one `Function` built with the ordinary
 * `Builder`; a case is `addAggregate` over five values; the array is `buildGrowableArray`, the same
 * library roles `[1, 2, 3]` goes through; a `Site` is `siteValue`, the same one a `@caller` position
 * is filled with; and the runner is reached by `callFunction`, which is how the compiler makes any
 * call. A second code path for any of those would be a second answer about what a call, a string or
 * a container is.
 */

/*
 * `@test`, and the one attribute that adds a declaration to a list rather than setting a flag on it.
 *
 * Three forms, and the arguments are read positionally because there are only two things to say:
 *
 *     @test                                     the declaration's own name
 *     @test("a key inserted twice keeps it")    a display name; a test name is prose
 *     @test(aborts)                             the case is expected to stop the process
 *
 * and `@test("name", aborts)` for both. It composes with `@platform` exactly as any other attribute
 * does, and a `@test` in a file this compilation did not select does not exist at all - which is the
 * `test` file selector rather than a rule of its own.
 *
 * **A `@test` outside a test build is a diagnostic naming `-test`, not a declaration silently
 * dropped.** A test that is not built is precisely the failure this framework exists to prevent, and
 * a build that compiles the attribute and then runs nothing is that failure with the evidence
 * removed.
 *
 * The signature rules are the runner's contract: no arguments, and not generic. A table-driven test
 * is a loop in the body rather than a parameter, which is what keeps the runner from needing a value
 * model for cases. The **result** is not checked here, because for an inferring body it is only
 * known once that body has been resolved - `buildTestCase` is the one pass that can ask.
 */
void Resolver::readTestAttribute(ast::Decl& declaration, Function& function) {
    auto testName = context.addUnqualifiedName("test", 4);
    auto abortsName = context.addUnqualifiedName("aborts", 6);

    for(auto attribute: declaration.attributes.contents(parse)) {
        if(attribute.name != testName) continue;

        if(!context.settings.test) {
            context.diagnostics.error(
                "`@test` needs a test build - pass `-test`, which is what selects the `.test.yana` files and synthesizes the entry that runs them. A test the build does not run is worse than one that fails"_v,
                attribute.source);
            return;
        }

        auto display = function.name;
        auto aborts = false;
        auto ok = true;

        for(auto argument: attribute.args.contents(parse)) {
            auto& value = argument.value;

            if(ast::isLiteral(value) &&
               ast::Literal::Kind(value.kind - ast::Expr::Lit) == ast::Literal::String) {
                display = value.lit.s;
                continue;
            }

            if(value.kind == ast::Expr::Var && value.var == abortsName) {
                aborts = true;
                continue;
            }

            context.diagnostics.error(
                "`@test` takes a display name as a string, `aborts`, or both - `@test(\"a name\", aborts)`"_v,
                attribute.source);
            ok = false;
        }

        if(!ok) return;

        if(function.args.isNotEmpty()) {
            context.diagnostics.error(
                "a `@test` function takes no arguments - there is nothing for the runner to fill them from. A table-driven test is a loop over the table in the body"_v,
                declaration.source);
            return;
        }

        if(isGeneric(decl(), function.gen)) {
            context.diagnostics.error(
                "a `@test` function cannot be generic - nothing calls it, so there is no call site for its type arguments to come from"_v,
                declaration.source);
            return;
        }

        program.tests.declarations.push(TestDeclaration {
            DeclPtr<Function>(&function - *program.decl), display, here() ? decl()[here()]->name
                                                                          : StringId(),
            declaration.source, aborts,
        });

        return;
    }
}

/*
 * `Test.Case` and `Test.runMain`, found by name.
 *
 * The mechanism `@builtin` uses for a role, applied to a module that is **not** part of the
 * prelude: whichever module of this program is called `Test` is the one, and a compilation with
 * `-test` that cannot reach one is a diagnostic naming it rather than an entry that quietly runs
 * nothing. A role would be the wrong instrument - a role is a promise the *standard library* makes
 * to the compiler, and a test framework is a library a program chooses.
 *
 * Reachable rather than imported here, because the entry is synthesized into the root module and an
 * import is a property of a file. What puts `Test` in the program is the `.test.yana` file that
 * imports it, which is the file that was going to name `check` anyway.
 */
struct TestLibrary {
    DeclPtr<RecordType> caseType = nullptr;
    DeclPtr<Function> runMain = nullptr;

    /*
     * The five fields of `Test.Case`, read off the record once rather than by index at each fill.
     *
     * The record is the contract between the compiler and the library, and this is the one place
     * that says so: a `Test` module whose `Case` is not the shape the entry fills is a diagnostic
     * here and nowhere else. `body`'s result is what a `@test` declaration's own result is checked
     * against, so even that comes from the library rather than from a name the compiler holds.
     */
    enum Field: U16 { Name, Module, Site, Aborts, Body, Count };

    DeclPtr<Type> type = nullptr;
    DeclPtr<Type> site = nullptr;
    DeclPtr<Type> result = nullptr;
    DeclPtr<Type> bodyType = nullptr;
};

bool Resolver::findTestLibrary(TestLibrary& into) {
    auto name = context.addUnqualifiedName("Test", 4);
    auto module = program.findModule(name);

    if(module) {
        into.caseType = program.findRecord(module, context.addUnqualifiedName("Case", 4));
        into.runMain = program.findFunction(module, context.addUnqualifiedName("runMain", 7), 1);
    }

    if(!into.caseType || !into.runMain) {
        context.diagnostics.error(
            "this is a test build and the `Test` module is not reachable - a `.test.yana` file has to `import Test`, which is what declares the runner the synthesized entry calls"_v,
            kNullLocation);
        return false;
    }

    auto& record = *decl()[into.caseType];
    into.type = asBase<Type>(into.caseType);

    if(record.cons.size() != 1 || record.fields.size() != TestLibrary::Count) {
        context.diagnostics.error(
            "`Test.Case` does not have the five fields the synthesized entry fills - name, moduleName, site, aborts and body"_v,
            record.source);
        return false;
    }

    into.site = record.field(decl(), TestLibrary::Site).occurrence.type;
    into.bodyType = record.field(decl(), TestLibrary::Body).occurrence.type;

    if(!isCallerSiteRecord(into.site)) {
        context.diagnostics.error(
            "`Test.Case`'s site field is not a record the compiler can fill - its fields are drawn from `file: String`, `line`, `column` and `function: String`"_v,
            record.source);
        return false;
    }

    if(decl()[into.bodyType]->kind != Type::Fun ||
       ((FunType*)decl()[into.bodyType])->params.size() != 0) {
        context.diagnostics.error("`Test.Case`'s body field is not a function of no arguments"_v,
                                  record.source);
        return false;
    }

    into.result = ((FunType*)decl()[into.bodyType])->result.type;
    return true;
}

/*
 * The body of a `@test` declaration that answers `{}`, as a function that answers `Test`.
 *
 * A `Case` holds one shape of body and both spellings are allowed, so the difference is absorbed
 * here rather than pushed onto the author: what a test looks like should not be decided by what the
 * runner's record happens to hold. The wrapper is one call and one `Ok({})`, and the inliner removes
 * it wherever it removes anything.
 */
DeclPtr<Function> Resolver::wrapUnitTest(DeclPtr<Function> test, DeclPtr<Type> resultType,
                                         LocationId source) {
    auto& declared = *decl()[test];
    auto module = declared.module;

    auto text = Tritium::format("%@$case", context.findName(declared.name));
    auto symbol = program.uniqueSymbol(module, context, StringView(text.text(), text.size()));

    auto wrapper = new (program.decl) Function(symbol, module);
    wrapper->source = source;
    wrapper->result = owned(resultType);
    wrapper->summary.bodyKnown = true;
    wrapper->once = true;

    auto pointer = DeclPtr<Function>(wrapper - *program.decl);
    decl()[module]->functions.push(program.decl, pointer);

    /*
     * `Ok`, found by name among the result's constructors.
     *
     * By name because the type is the library's `Result(Failure, {})` and its constructor *order* is
     * not something the compiler is entitled to assume - what `Core` promises about `Result` is the
     * two names. A record with no `Ok` is reported by the caller through the result check, since a
     * `@test` answering a type with no `Ok` in it never matched `Test` in the first place.
     */
    auto& record = *(RecordType*)decl()[resultType];
    U16 ok = record.cons.size();

    for(auto constructor: record.cons.contents(decl())) {
        if(context.findName(constructor.name) == "Ok"_v) ok = constructor.index;
    }

    if(ok >= record.cons.size()) {
        context.diagnostics.error(
            "`Test.Case`'s body answers %@, which has no `Ok` for a test that answers `{}` to be wrapped in"_v,
            source, typeText(context, decl(), resultType));
        return nullptr;
    }

    ErrorMark mark(context);
    Builder builder(context, program, *wrapper);
    beginEntry(*wrapper);

    Substitution none;
    if(!builder.addCall(source, *decl()[test], nullptr, 0, none)) return nullptr;

    /*
     * `Ok`, at whatever it carries.
     *
     * `Result(e, a)`'s carries one field, which for a `Test` is `{}`; a framework whose success
     * constructor carries nothing is the same statement with no field at all. Read off the
     * constructor rather than assumed, because which of the two the library wrote is the library's
     * business and getting it wrong is an aggregate of the wrong arity rather than a diagnostic.
     */
    auto arm = record.cons.get(decl(), ok);
    if(arm.count > 1) {
        context.diagnostics.error(
            "`Test.Case`'s body answers %@, whose `Ok` carries %@ fields - a test that answers `{}` has nothing to fill them with"_v,
            source, typeText(context, decl(), resultType), U64(arm.count));
        return nullptr;
    }

    Node* fields[1] = { nullptr };
    if(arm.count) fields[0] = builder.addConst(source, unitConstant(program.types.unit));

    auto value = builder.addAggregate(source, resultType, ok, fields, arm.count);
    if(!value) return nullptr;

    builder.addReturn(source, *value);
    finish(builder, *wrapper, false, source, mark);
    builder.seal();
    currentFunction = nullptr;

    return pointer;
}

/*
 * One `Test.Case`, built in the entry's own frame.
 *
 * Null where the declaration is not one a runner can call, having said so - which is a case dropped
 * from the suite rather than a compilation that fails, so that one malformed test does not hide
 * every other one's result.
 */
Node* Resolver::buildTestCase(Builder& builder, const TestLibrary& library,
                              const TestDeclaration& test) {
    auto& declared = *decl()[test.function];

    // The result, which for an inferring body is only known once that body has been resolved.
    // Forced here rather than assumed, because "no arguments and one of two results" is the whole of
    // what a runner can rely on.
    if(!requireResult(declared, test.source)) return nullptr;

    auto result = declared.result.type;
    auto unit = result == program.types.unit;

    if(!unit && result != library.result) {
        context.diagnostics.error(
            "a `@test` function answers `{}` or %@ - the runner has nothing else to do with a result"_v,
            test.source, typeText(context, decl(), library.result));
        return nullptr;
    }

    auto body = unit ? wrapUnitTest(test.function, library.result, test.source) : test.function;
    if(!body) return nullptr;

    auto site = siteValue(builder, library.site, test.source, declared.name);
    if(!site) return nullptr;

    // The five, in the record's own order - `TestLibrary::Field`, which is the one statement of what
    // that order is.
    Node* fields[TestLibrary::Count] = {};

    fields[TestLibrary::Name] = textValue(builder, test.name, test.source);
    fields[TestLibrary::Module] = textValue(builder, test.module, test.source);
    fields[TestLibrary::Site] = site;
    fields[TestLibrary::Aborts] = builder.addConst(test.source,
                                                   truthConstant(program, test.aborts));
    fields[TestLibrary::Body] = builder.addFunValue(test.source, body, library.bodyType, nullptr);

    for(auto field: fields) {
        if(!field) return nullptr;
    }

    return builder.addAggregate(test.source, library.type, 0, fields, TestLibrary::Count);
}

/*
 * A test file's `let`s, resolved into a function of the file's own module and called by the entry
 * before any case runs - Design-Test.md §11.2's F5.
 *
 * **A function per file rather than statements in the entry**, for two reasons that are the same
 * reason. A test file's globals belong to *its* module, so their initializers have to be resolved in
 * that module's scope and under that file's imports - and the entry lives in the root, which is
 * usually neither. And a function is what the reachability walk already understands: the entry calls
 * these, so they are roots exactly as `main` is, with nothing here to teach it.
 *
 * **The order is stated rather than invented.** Modules as `Program::modules` holds them, files
 * within a module in path order, statements within a file as written. What is deliberately *not*
 * claimed is that two test files may depend on each other's globals: `uninitialized` spans all of
 * them, so a read of a global whose file has not run yet is the use-before-init it already was
 * inside one top level, rather than a load of the zeroes.
 */
void Resolver::resolveTestFileInitializers(Builder& entry) {
    if(testTopLevel.isEmpty()) return;

    auto& outer = *currentFunction;

    // Every one of them at once, and only then the walk that removes each as its statement runs -
    // which is what makes a read of a *later* file's global a report rather than a zero.
    for(auto& statement: testTopLevel) {
        for(U32 i = 0; i < statement.count; i++) {
            auto pointer = writtenGlobals[statement.first + i];
            if(pointer && decl()[pointer]->dynamic) uninitialized.push(pointer);
        }
    }

    for(auto module: program.modules) {
        // The highest file index this module contributed, so the loop below is over the files that
        // have any rather than over every file of every module.
        auto files = U16(0);
        for(auto& statement: testTopLevel) {
            if(statement.module == module && statement.file >= files) files = U16(statement.file + 1);
        }

        for(U16 file = 0; file < files; file++) {
            // The file's first statement, which is both the test for "this file has any" and the
            // location a diagnostic about the synthesized function should land on.
            auto source = kNullLocation;

            for(auto& statement: testTopLevel) {
                if(statement.module != module || statement.file != file) continue;

                source = statement.source;
                break;
            }

            if(source == kNullLocation) continue;

            auto symbol = program.uniqueSymbol(module, context, "testInit$"_v);
            auto init = new (program.decl) Function(symbol, module);
            init->source = source;
            init->result = owned(program.types.unit);
            init->summary.bodyKnown = true;
            init->once = true;

            auto pointer = DeclPtr<Function>(init - *program.decl);
            decl()[module]->functions.push(program.decl, pointer);

            {
                ErrorMark mark(context);
                Builder builder(context, program, *init);
                beginEntry(*init);

                ModuleScope scope(*this, module, file);

                for(auto& statement: testTopLevel) {
                    if(statement.module != module || statement.file != file) continue;
                    if(builder.isTerminated()) break;

                    resolveTopLevelBinding(builder, statement);
                }

                if(!builder.isTerminated()) builder.addReturnUnit(source);

                finish(builder, *init, true, source, mark);
                builder.seal();
            }

            // Back in the entry, which the nested build above took `currentFunction` from.
            currentFunction = &outer;

            if(entry.isTerminated()) break;
            callFunction(entry, *init, nullptr, 0, source);
        }
    }

    uninitialized.clear();
}

/*
 * The entry of a test build - Design-Test.md §11.2's F1, and §3.3's third rule.
 *
 * One array of `Test.Case` and one call. It is synthesized for exactly the reason the ordinary entry
 * is: there is something the program has to do before anything the author wrote, and no place in the
 * source for it to be written.
 *
 * **A user-written `main` is not the entry and is not a root.** A test build runs tests. Nothing
 * here looks for one, which is the whole of how that rule is enforced.
 */
void Resolver::resolveTestEntry() {
    TestLibrary library;
    if(!findTestLibrary(library)) return;

    auto& tests = program.tests.declarations;

    // The first case's own line, so a diagnostic about the entry lands somewhere a reader can go.
    // Null for a suite with no cases at all, which `runMain` reports as the failure it is.
    auto source = tests.size() ? tests[0].source : kNullLocation;

    auto symbol = program.uniqueSymbol(program.root(), context, "main$"_v);
    auto entry = new (program.decl) Function(symbol, program.root());
    entry->source = source;
    entry->summary.bodyKnown = true;
    entry->once = true;
    entry->result = owned(decl()[library.runMain]->result.type);

    auto pointer = DeclPtr<Function>(entry - *program.decl);
    decl()[program.root()]->functions.push(program.decl, pointer);
    program.entry = pointer;

    ErrorMark mark(context);
    Builder builder(context, program, *entry);
    beginEntry(*entry);

    // Ahead of the cases, exactly as it runs ahead of `main` - a test that reads a global
    // initialized at startup needs it initialized. §3.3's rule is about `main`, not about startup.
    resolveRootTopLevel(builder);

    // Then the test files' own, which are initializers rather than a program start - F5.
    resolveTestFileInitializers(builder);

    ModuleScope scope(*this, program.root(), 0);

    SmallArray<Node*, 8> cases;
    HeldList<SmallArray<Node*, 8>> held(*this, cases);

    for(auto& test: tests) {
        if(builder.isTerminated()) break;

        // The case's own body runs once, from the runner - see `Function::once`.
        decl()[test.function]->once = true;
        if(auto value = buildTestCase(builder, library, test)) cases.push(value);
    }

    if(!builder.isTerminated()) {
        auto array = buildGrowableArray(builder, library.type, cases, source);

        if(array) {
            Node* arguments[1] = { array };
            auto status = callFunction(builder, *decl()[library.runMain], arguments, 1, source);
            if(status) builder.addReturn(source, *status);
        }
    }

    // A body that reported on the way here still has to end in something the verifier accepts, which
    // is the same tail `resolveEntryBody` gives a top level that could not be finished.
    if(!builder.isTerminated()) builder.addReturnUnit(source);

    finish(builder, *entry, false, source, mark);
    builder.seal();
    currentFunction = nullptr;
}

} // namespace mir
