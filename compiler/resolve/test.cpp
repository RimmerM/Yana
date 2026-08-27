#include "test.h"
#include "expr.h"
#include "caller.h"
#include "name.h"
#include "type_internal.h"
#include "../parse/ast.h"

/*
 * `@test` - Design-Test.md §11.2's F1, and the one attribute that adds a declaration to a list rather
 * than setting a flag on one.
 *
 * Three forms, and the arguments are read positionally because there are only two things to say:
 *
 *     @test                                     the declaration's own name
 *     @test("a key inserted twice keeps it")    a display name; a test name is prose
 *     @test(aborts)                             the case is expected to stop the process
 *
 * and `@test("name", aborts)` for both. It composes with `@platform` exactly as any other attribute
 * does, and a `@test` in a file this compilation did not select does not exist at all - which is the
 * point of the `test` file selector rather than a rule of its own.
 *
 * **A `@test` outside a test build is a diagnostic naming `-test`, not a declaration silently
 * dropped.** A test that is not built is precisely the failure this framework exists to prevent, and
 * a build that compiles the attribute and then runs nothing is that failure with the evidence
 * removed.
 *
 * The signature rules are the runner's contract: no arguments, and a result of `{}` or `Test`. A
 * table-driven test is a loop in the body rather than a parameter, which is what keeps the runner
 * from needing a value model for cases.
 */
void readTestAttribute(Module& module, const ast::Decl& decl, Function& function) {
    auto attributes = decl.attributes;
    if(attributes.isEmpty()) return;

    auto& context = module.context;
    auto testName = context.addUnqualifiedName("test", 4);
    auto abortsName = context.addUnqualifiedName("aborts", 6);

    for(auto attribute: attributes.contents(module.parse)) {
        if(attribute.name != testName) continue;

        if(!context.settings.test) {
            context.diagnostics.error("`@test` needs a test build - pass `-test`, which is what selects the `.test.yana` files and synthesizes the entry that runs them. A test the build does not run is worse than one that fails"_v,
                                      attribute.source);
            return;
        }

        auto display = function.name;
        auto aborts = false;
        auto ok = true;

        for(auto argument: attribute.args.contents(module.parse)) {
            auto& value = argument.value;

            if(ast::isLiteral(value) && ast::Literal::Kind(value.kind - ast::Expr::Lit) == ast::Literal::String) {
                display = value.lit.s;
                continue;
            }

            if(value.kind == ast::Expr::Var && value.var == abortsName) {
                aborts = true;
                continue;
            }

            context.diagnostics.error("`@test` takes a display name as a string, `aborts`, or both - `@test(\"a name\", aborts)`"_v,
                                      attribute.source);
            ok = false;
        }

        if(!ok) return;

        if(function.args.isNotEmpty()) {
            context.diagnostics.error("a `@test` function takes no arguments - there is nothing for the runner to fill them from. A table-driven test is a loop over the table in the body"_v,
                                      decl.source);
            return;
        }

        if(function.gen) {
            context.diagnostics.error("a `@test` function cannot be generic - nothing calls it, so there is no call site for its type arguments to come from"_v,
                                      decl.source);
            return;
        }

        // The result is not checked here, because for an inferring body it is only known once that
        // body has been resolved - see resolveTestEntry, which is the one pass that can ask.
        module.program.tests.declarations.push(TestDeclaration {
            .function = &function - *module.arena,
            .name = display,
            .module = module.name,
            .source = decl.source,
            .aborts = aborts,
        });

        return;
    }
}

bool moduleDeclaresTests(Context& context, ast::ModuleGroup& group) {
    ast::ParseBase parse(*context.parseRegion);
    auto testName = context.addUnqualifiedName("test", 4);

    for(auto file: group.files) {
        for(auto decl: file->decls.contents(parse)) {
            for(auto attribute: decl.attributes.contents(parse)) {
                if(attribute.name == testName) return true;
            }
        }
    }

    return false;
}

/*
 * `Test.Case` and `Test.runMain`, found by name.
 *
 * The mechanism `Program::checkFailed` already uses for `Core.Check.checkFailed`, applied to a module
 * that is not part of the prelude: whichever module of this program is called `Test` is the one, and
 * a compilation with `-test` that cannot reach one is a diagnostic naming it rather than an entry
 * that quietly runs nothing.
 *
 * Reachable rather than imported *here*, because the entry is synthesized into the root module and
 * an import is a property of a file. What puts `Test` in the program is the `.test.yana` file that
 * imports it, which is the file that was going to name `check` anyway.
 */
static bool findTestLibrary(Program& program) {
    auto& context = program.context;
    auto local = *program.arena;
    auto& registry = program.tests;

    for(auto module: program.modules) {
        if(context.findName(module->name) != "Test"_v) continue;

        if(auto type = module->namedTypes.get(Context::nameHash("Case"_v))) {
            auto found = type.unwrap();
            if((*program.types)[found]->kind == Type::Record) {
                registry.caseType = (GlobalPtr<RecordType>)found;
            }
        }

        if(auto run = module->functions.get(Context::nameHash("runMain"_v))) {
            registry.runMain = run.unwrap();
        }

        break;
    }

    if(registry.caseType && registry.runMain) {
        local[registry.runMain]->used = true;
        return true;
    }

    context.diagnostics.error("this is a test build and the `Test` module is not reachable - a `.test.yana` file has to `import Test`, which is what declares the runner the synthesized entry calls"_v,
                              kNullLocation);
    return false;
}

/*
 * The five fields of `Test.Case`, read off the record once rather than by index at each fill.
 *
 * The record is the contract between the compiler and the library, and this is the one place that
 * says so: a `Test` module whose `Case` is not the shape the entry fills is a diagnostic here and
 * nowhere else. `body`'s result is what a `@test` declaration's own result is checked against, so
 * even that comes from the library rather than from a name the compiler holds.
 */
struct TestCaseShape {
    TypePtr type = nullptr;
    TypePtr site = nullptr;
    TypePtr result = nullptr;

    enum Field: U16 { Name, Module, Site, Aborts, Body, Count };
};

static bool readTestCaseShape(Program& program, TestCaseShape& shape) {
    auto& context = program.context;
    auto global = *program.types;

    shape.type = (TypePtr)program.tests.caseType;

    auto record = (RecordType*)global[shape.type];
    auto content = record->constructors.get(global, 0).content;

    if(!content || global[content]->kind != Type::Tup) {
        context.diagnostics.error("internal: `Test.Case` is not a record of fields"_v, kNullLocation);
        return false;
    }

    auto fields = (TupType*)global[content];
    if(fields->fields.size() != TestCaseShape::Count) {
        context.diagnostics.error("`Test.Case` does not have the five fields the synthesized entry fills - name, moduleName, site, aborts and body"_v,
                                  kNullLocation);
        return false;
    }

    shape.site = fields->fields.get(global, TestCaseShape::Site).type;
    auto body = fields->fields.get(global, TestCaseShape::Body).type;

    if(global[body]->kind != Type::Fun) {
        context.diagnostics.error("`Test.Case`'s body field is not a function"_v, kNullLocation);
        return false;
    }

    shape.result = ((FunType*)global[body])->result;
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
static ModulePtr<Function> wrapUnitTest(Module& module, ModulePtr<Function> test, TypePtr resultType,
                                        LocationId source) {
    auto local = *module.arena;
    auto& context = module.context;

    StringBuilder name;
    name << context.findName(local[test]->name) << "$case";

    auto wrapper = addAnonymousFunction(module, builtName(context, name), source);
    wrapper->returnType = resultType;
    wrapper->used = true;

    ExprResolver resolver(context, module, *wrapper);
    resolver.emitDirectCall(test, {}, source);
    if(!resolver.current) return nullptr;

    auto record = (RecordType*)(*module.types)[resultType];
    U32 ok = 0;

    for(auto constructor: record->constructors.contents(*module.types)) {
        if(context.findName(constructor.name) == "Ok"_v) ok = constructor.index;
    }

    auto value = resolver.makeConstructed(resultType, ok, nullptr, source);
    resolver.terminate(resolver.emit<InstRet>(source, StringId(), module.scalar.unit,
                                              resolver.returnValue(value, source)));

    return wrapper - local;
}

/*
 * One `Test.Case`, built in the entry's own frame.
 *
 * Null where the declaration is not one a runner can call, having said so - which is a case dropped
 * from the suite rather than a compilation that fails, so that one malformed test does not hide
 * every other one's result.
 */
static ModulePtr<Value> buildTestCase(ExprResolver& resolver, const TestCaseShape& shape,
                                      const TestDeclaration& test) {
    auto& module = resolver.module;
    auto& context = resolver.context;
    auto local = *module.arena;
    auto global = resolver.global;

    auto& declared = *local[test.function];

    // The result, which for an inferring body is only known once that body has been resolved.
    // Forced here rather than assumed, because "no arguments and one of two results" is the whole of
    // what a runner can rely on.
    auto result = declared.inferReturn ? requireReturnType(*declared.module, declared, test.source)
                                       : declared.returnType;

    auto unit = result && isUnit(global, result);

    if(!result || !(unit || sameType(result, shape.result))) {
        context.diagnostics.error("a `@test` function answers `{}` or `Test` - the runner has nothing else to do with a result"_v,
                                  test.source);
        return nullptr;
    }

    auto body = unit ? wrapUnitTest(module, test.function, shape.result, test.source) : test.function;
    if(!body) return nullptr;

    // A root of the reachability walk in its own right. The entry names it through the function
    // value below, which is already a reason for it to exist - but §3.3 asks for the root, and a
    // `@test` that is dead in `exe` mode because the entry is the only root is the failure this is
    // here to rule out.
    declared.used = true;
    local[body]->used = true;

    auto site = buildCallerSite(resolver, shape.site, test.source, declared.name);
    auto callable = resolver.functionValue(body, test.source);
    if(!site || !callable) return nullptr;

    auto storage = resolver.allocate(shape.type, test.source);
    auto place = resolver.project(resolver.placeFor(storage, test.source), ProjectionKind::Downcast, 0);

    auto fill = [&](TestCaseShape::Field field, ModulePtr<Value> value) {
        resolver.initialize(resolver.project(place, ProjectionKind::Field, field), value, test.source);
    };

    fill(TestCaseShape::Name, resolver.resolveString(test.source, test.name));
    fill(TestCaseShape::Module, resolver.resolveString(test.source, test.module));
    fill(TestCaseShape::Site, site);
    fill(TestCaseShape::Aborts, resolver.makeInt(test.source, module.scalar.bool_, test.aborts ? 1 : 0));
    fill(TestCaseShape::Body, callable);

    return storage;
}

/*
 * A test file's `let`s, resolved into a function of the file's own module, called by the entry
 * before any case runs - Design-Test.md §3.1 and §11.2's F5.
 *
 * **A function per file rather than statements in the entry**, for two reasons that are the same
 * reason. A test file's globals belong to *its* module, so their initializers have to be resolved in
 * that module's scope and under that file's imports - and the entry lives in the root, which is
 * usually neither. And a function is what the reachability walk already understands: the entry calls
 * these, so they are roots exactly as `main` is, with nothing here to teach it.
 *
 * **The order is stated rather than invented.** Modules in the order the program collected them,
 * files within a module in path order, statements within a file in written order. The first two are
 * the order every declaration pass already runs in - `Module::files` is path-ordered for exactly
 * this kind of reason - and the third is the only order a top level has ever had. What is
 * deliberately *not* claimed is that two test files may depend on each other's globals: the pending
 * list below spans all of them, so a read of a global whose file has not run yet is reported instead
 * of loading the zeroes.
 */
static void resolveTestFileInitializers(Program& program, ExprResolver& entry, PendingGlobals& pending) {
    auto& context = program.context;
    auto local = *program.arena;
    auto initName = context.addUnqualifiedName("testInit$", 9);

    for(auto module: program.modules) {
        if(module->testTopLevel.isEmpty()) continue;

        collectPendingGlobals(*module, module->testTopLevel, pending);
    }

    for(auto module: program.modules) {
        if(module->testTopLevel.isEmpty()) continue;

        for(U32 file = 0; file < module->files.size(); file++) {
            // The file's first statement, which is both the test for "this file has any" and the
            // location a diagnostic about the synthesized function should land on.
            auto source = kNullLocation;

            for(auto statement: module->testTopLevel.contents(local)) {
                auto written = module->parse[statement.decl]->source;
                if(module->fileOf(written) != file) continue;

                source = written;
                break;
            }

            if(source == kNullLocation) continue;

            auto function = addAnonymousFunction(*module, initName, source);
            function->returnType = module->scalar.unit;

            {
                FileScope scope(*module, U16(file));
                ExprResolver resolver(context, *module, *function);

                resolver.uninitialized = &pending;
                resolveTopLevelStatements(resolver, module->testTopLevel, file);
                resolver.uninitialized = nullptr;

                if(!resolver.current) continue;
                resolver.terminate(resolver.emit<InstRet>(source, StringId(), module->scalar.unit, nullptr));
            }

            if(!entry.current) return;
            entry.emitDirectCall(function - local, {}, source);
        }
    }

    for(auto module: program.modules) {
        if(module->testTopLevel.isEmpty()) continue;

        settleTopLevelTypes(*module, module->testTopLevel);
    }
}

/*
 * The entry of a test build - Design-Test.md §11.2's F1, and §3.3's third rule.
 *
 * One array of `Test.Case` and one call. It is synthesized for exactly the reason the top-level
 * entry is: there is a thing the program has to do before anything the author wrote, and no place in
 * the source for it to be written. What makes it the compiler's rather than the library's is that
 * there is no static initializer, no linker section and no reflection here for a `Test` module to
 * register from - and a *written* registration list, which is the only alternative, means every test
 * is written twice and the second half is invisible when it is missing.
 *
 * **A user-written `main` is not the entry and is not a root.** A test build runs tests. Nothing here
 * looks for one, which is the whole of how that rule is enforced.
 */
void resolveTestEntry(Program& program) {
    auto module = program.root;
    auto& context = program.context;
    auto local = *program.arena;

    if(!findTestLibrary(program)) return;

    TestCaseShape shape;
    if(!readTestCaseShape(program, shape)) return;

    auto& tests = program.tests.declarations;

    // The first case's own line, so a diagnostic about the entry lands somewhere a reader can go.
    // Null for a suite with no cases at all, which `runMain` reports as the failure it is.
    auto source = tests.size() ? tests[0].source : kNullLocation;

    auto function = addAnonymousFunction(*module, context.addUnqualifiedName("main$", 5), source);
    function->returnType = module->scalar.int_;
    program.entry = function - local;

    /*
     * The file the top level was written in, which is what its imports are read through - the same
     * scope the ordinary entry runs under, for the same reason. A root with no top level at all has
     * no file to prefer, and takes the first.
     */
    auto first = module->topLevel.size() ? module->topLevel.get(local, 0).decl : nullptr;

    FileScope scope(*module, first ? module->fileOf(module->parse[first]->source) : U16(0));
    ExprResolver resolver(context, *module, *function);

    // Ahead of the cases, exactly as it runs ahead of `main` - a test that reads a global
    // initialized at startup needs it initialized. See resolveRootTopLevel.
    resolveRootTopLevel(resolver);
    if(!resolver.current) return;

    /*
     * And then the test files' own, in every module that has one - the root's included, since a
     * `.test.yana` file there is a test file like any other and its `let`s are initializers rather
     * than part of the program's written start.
     *
     * After the root's top level rather than before it: a test file is a file of a module that
     * already exists, so anything the program initializes at startup is initialized by the time one
     * of these runs, and the dependency cannot go the other way.
     */
    PendingGlobals pendingTests;
    resolveTestFileInitializers(program, resolver, pendingTests);
    if(!resolver.current) return;

    ValueList cases;
    for(auto& test: tests) {
        if(auto value = buildTestCase(resolver, shape, test)) cases.push(value);
    }

    auto array = resolver.buildArrayLiteral(shape.type, toBuffer(cases), nullptr, source);
    if(!array) return;

    ResolvedArg args[] = { ResolvedArg(array) };
    auto status = resolver.emitDirectCall(program.tests.runMain, { args, 1 }, source);

    if(!resolver.current) return;
    resolver.terminate(resolver.emit<InstRet>(source, StringId(), module->scalar.unit, status));
}
