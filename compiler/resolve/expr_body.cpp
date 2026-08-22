/*
 * A function body, from its arguments to its return.
 *
 * The entry points the module driver calls, and the two checks that can only be made once a whole
 * body exists: that a lazy argument is forced on every path that needs it, and that a body which
 * falls off its end has a return type that permits it.
 */

#include "expr.h"
#include "complete.h"
#include "generic.h"
#include "name.h"
#include "index.h"

/*
 * Names one binding per parameter, and storage for the ones that need it.
 *
 * `firstArg` is where the declared parameters start, which is one for anything reached as a
 * function value: those take the closure environment as argument zero, and it is bound by whoever
 * knows what is in it rather than by name.
 */
void bindFunctionArgs(ExprResolver& resolver, Module& module, Function& function, Size firstArg) {
    Size index = 0;

    for(auto argPointer: function.args.contents(*module.arena)) {
        if(index++ < firstArg) continue;

        auto arg = (*module.arena)[argPointer];
        auto value = (ModulePtr<Value>)argPointer;
        Binding binding { arg->name, value };
        binding.definition = arg->source;

        if(arg->isLazy()) {
            // The name holds the thunk, not the value the signature declared, and reading it is
            // what runs the caller's expression. No local and no place: there is nothing here to
            // load from until the force has happened - see ExprResolver::force.
            binding.lazy = true;
            resolver.bindings.push(binding);
            recordBindingDefinition(resolver, binding);
            continue;
        }

        if(arg->isMutableBorrow()) {
            // A `&` parameter names storage the caller owns. The argument arrived as the address
            // of it, so the parameter gets a local whose value *is* that address - which is
            // exactly what a local of an ordinary allocation holds - and the binding names the
            // slot rather than the value, so reads load and assignments write through.
            //
            // `borrowed` is what keeps this frame from treating the slot as its own: it is never
            // allocated here and never dropped here.
            binding.local = function.addLocal(module, arg->type, arg->name, value,
                                              ast::BindType::Ref, true);
        } else if(isMemoryType(*module.types, arg->type)) {
            function.addLocal(module, arg->type, arg->name, value, arg->convention);
        }

        resolver.bindings.push(binding);
        recordBindingDefinition(resolver, binding);
    }
}

/*
 * A `@lazy` parameter may be forced at most once on any path (Design.md's Deferred arguments).
 *
 * This is what makes the absence of a memoization slot a rule rather than an omission: forcing
 * twice is rejected, so no program can tell call-by-name from call-by-need and no cell has to exist
 * to make that true. It is also the whole of the linearity checking this version needs - the same
 * shape linear types will want, stated over one parameter instead of over every owner.
 *
 * *Using* the parameter rather than calling it, because there are two ways to spend the one
 * evaluation and only one of them is a call. Reading the name forces it here; passing it on to
 * another `@lazy` parameter hands the evaluation to a callee that may spend it, and a body that did
 * both would have evaluated the caller's argument twice. Every use is therefore counted, which is
 * what the value's own use list already records.
 *
 * A forward fixpoint over "may already have been used", which is the only formulation that gets a
 * loop right: using it once in a loop body is using it once per iteration, and that is the second
 * use. Iterated because the block list is in RPO but a back edge still carries state backwards -
 * one pass would clear a body that the second visit rejects.
 */
static void checkLazyForcing(Module& module, Function& function) {
    auto local = *module.arena;
    auto blocks = function.blocks.size();

    for(auto argPointer: function.args.contents(local)) {
        auto arg = local[argPointer];
        if(!arg->isLazy()) continue;

        if(arg->useCount() < 2) continue;

        auto isUse = [&](ModulePtr<Inst> instruction) {
            for(auto user: arg->uses(local)) {
                if(user == instruction) return true;
            }

            return false;
        };

        // `exit[i]` is whether some path through block i has used it by the time it ends. Nothing
        // else has to be remembered: a block's entry state is the union of its predecessors' exits,
        // which is recomputed each visit.
        Array<bool> exit;
        for(Size i = 0; i < blocks; i++) exit.push(false);

        auto reported = false;

        for(auto changed = true; changed && !reported;) {
            changed = false;

            for(Size i = 0; i < blocks; i++) {
                auto block = local[function.blocks.get(local, i)];

                auto used = false;
                for(auto incoming: block->incoming(local)) {
                    if(exit[local[incoming]->index]) used = true;
                }

                if(i == 0) used = false;

                for(auto instPointer: block->instructions(local)) {
                    if(!isUse(instPointer)) continue;

                    if(used) {
                        module.context.diagnostics.error("%@ is a `@lazy` parameter and may be used at most once on any path, but this path uses it again - passing it to another `@lazy` parameter counts, since the callee may be the one that runs it. Read it into a `let` and use that instead"_v,
                                                         local[instPointer]->source,
                                                         module.context.findName(arg->name));
                        reported = true;
                        break;
                    }

                    used = true;
                }

                if(reported) break;
                if(exit[i] != used) changed = true;

                exit[i] = used;
            }
        }
    }
}

// Class signatures, generated functions and specializations have no AST and are already complete.
bool resolveFunctionBody(Module& module, Function& function) {
    auto& context = module.context;
    if(!function.ast || function.resolving) return true;

    auto& decl = *module.parse[function.ast];

    /*
     * A declaration the compiler implements, whose body is therefore optional rather than absent.
     *
     * Most intrinsics have none: what they mean is one instruction at each call site and there is
     * nothing writable to resolve. `!`, `&&` and `||` are the exception and the reason the test is
     * on the *body* rather than on the hook. Their expansion needs the operand's `Truth` instance,
     * which a generic body has not got - `fn (Truth(a)) f(x: a) = !x` knows only that one exists -
     * so a call there is an ordinary `gencall` that specialization resolves later. Without a body to
     * reach, that gencall named a function with no blocks in it.
     *
     * So the two coexist, on the terms generateInstanceFunction already sets for a generated
     * instance: the hook is what a call site expands to and the body is what a call that cannot be
     * expanded reaches, and they are written to be the same operation.
     *
     * Both hooks, because one that wants its `@lazy` arguments unevaluated sits in the other field.
     */
    if((function.intrinsic || function.deferredIntrinsic) && !decl.fun.body) return true;

    if(!decl.fun.body) {
        context.diagnostics.error("function %@ requires a body"_v, decl.source, context.findName(function.name));
        return false;
    }

    function.resolving = true;

    ExprResolver resolver(context, module, function);
    bindFunctionArgs(resolver, module, function, 0);

    auto errors = context.diagnostics.errorCount();

    /*
     * A `yield`-form lens returns what its continuation produced, not what its last statement did.
     *
     * Everything after the `yield` is cleanup - Design.md's `withLock` unlocks there - so the value
     * that leaves is the one the `yield` handed back, and the body's own fall-through result is
     * discarded. That is the whole of what the sugar does that the explicit form would have had to
     * write out by hand.
     */
    if(function.yieldForm) {
        resolver.resolve(*module.parse[decl.fun.body], nullptr, false);

        if(resolver.current) {
            // An iterator falling off the end ran to completion, which is the step signal's
            // `Proceed` - there is no carried value, because nothing stopped it. A lens instead
            // returns what its one `yield` handed back, since everything after that is cleanup.
            auto result = function.funKind == ast::FunKind::Iter
                        ? resolver.makeOutcome(function.returnType, true, nullptr, decl.source)
                        : resolver.yieldResult;

            resolver.terminate(resolver.emit<InstRet>(decl.source, StringId(), module.scalar.unit, result));
        }

        checkLensYields(module, function, toBuffer(resolver.yields), decl.source);
        checkLazyForcing(module, function);

        function.ast = nullptr;
        function.resolving = false;
        return errors == context.diagnostics.errorCount();
    }

    if(decl.fun.implicitReturn) {
        /*
         * An `=` body is the function's result, so what is resolved here is a value and what
         * follows it is a `ret` carrying that value.
         *
         * Three ways the result type is known. Written, in which case the body is checked against
         * it. Inferred, in which case the body decides and `settle` runs first for the same reason
         * it does for a lambda - a bare literal body must not leave a result type no caller could
         * name. Or written as unit, where the body is resolved with no expected type, because `()`
         * is not a type a literal or a class function could have been asked to produce.
         */
        auto infer = function.inferReturn;
        auto unit = !infer && isUnit(*module.types, function.returnType);
        auto expected = infer ? nullptr : function.returnType;
        auto result = resolver.resolve(*module.parse[decl.fun.body], unit ? nullptr : expected, !unit);

        if(resolver.current) {
            if(infer) {
                result = resolver.settle(result, decl.source);
                function.returnType = result ? resolver.valueType(result) : module.scalar.unit;
                function.inferReturn = false;
                applyReturnRoots(module, function, decl.source);

                if(isUnit(*module.types, function.returnType)) result = nullptr;
            } else {
                result = unit ? nullptr : resolver.convert(result, function.returnType, decl.source);
            }

            resolver.terminate(resolver.emit<InstRet>(decl.source, StringId(), module.scalar.unit,
                                                      resolver.returnValue(result, decl.source)));
        } else if(infer) {
            // Every path left through an explicit `return`, so nothing falls off the end for the
            // type to be read off. Those returns were checked against null and reported there.
            function.returnType = module.scalar.unit;
            function.inferReturn = false;
        }

        /*
         * An `=` function that produces nothing is written in the wrong form.
         *
         * The `=` form says "this function *is* this expression", so a body with no value is a
         * statement wearing an expression's syntax - `fn bump(&x: Int) = x = x + 1` reads as though
         * it returned something. The block form is how that is said, and it is what this points at.
         *
         * Only when the unit result was not written down: `-> ()` is the author saying the same
         * thing the warning would, so repeating it back is noise.
         */
        if(!decl.fun.ret && isUnit(*module.types, function.returnType) && !function.instanceOf
           && errors == context.diagnostics.errorCount()) {
            context.diagnostics.warning("`%@` is written with `=` but its body produces no value, so it returns `()` - use the `:` block form for a function that runs statements rather than producing a result"_v,
                                        decl.source, context.findName(function.name));
        }
    } else {
        resolver.resolve(*module.parse[decl.fun.body], nullptr, false);

        if(resolver.current) {
            if(isUnit(*module.types, function.returnType)) {
                resolver.terminate(resolver.emit<InstRet>(decl.source, StringId(), module.scalar.unit, nullptr));
            } else if(!resolver.sawParseError) {
                // A body with a hole in it does not return a value because it is not finished,
                // which the parser has already said. Saying it again puts a second mark on a
                // function whose only problem is that it is halfway through being written.
                context.diagnostics.error("not all paths return a value"_v, decl.source);
            }
        }
    }

    checkLazyForcing(module, function);

    function.ast = nullptr;
    function.resolving = false;
    return errors == context.diagnostics.errorCount();
}

/*
 * One dynamically initialized global, and the whole of what makes it one.
 *
 * The initializer decides the type - a global has no other way to say what it is, since a `let`
 * pattern carries no annotation and the `:: T` form is part of the expression - so this is where a
 * dynamic global stops being typeless. Everything downstream depends on that having happened before
 * any other body is resolved, which is what orders `resolveProgramEntry` ahead of them.
 *
 * The write is an `Init` and not an `Assign`, and that is load-bearing rather than tidy: the storage
 * holds the zero it was emitted with, and an `Assign` would release that zero as though it were a
 * live value - the drop-of-zeroed-storage defect that a global with no initializer had, reintroduced
 * by the feature that fixes it.
 */
static void initializeGlobal(ExprResolver& resolver, ModulePtr<Global> pointer,
                             const ast::VarDecl& declaration) {
    auto& module = resolver.module;
    auto& context = module.context;
    auto global = *module.types;
    auto source = declaration.pat.source;
    auto definition = resolver.local[pointer];

    auto value = resolver.settle(resolver.resolve(*module.parse[declaration.content]), source);

    // Out of the pending list once its own initializer has been resolved and not before, so that
    // `let x = x + 1` is the use-before-init it reads as rather than a read of the zeroes. The
    // sequence declares them in order, so the one being initialized is the first one left.
    auto& pending = *resolver.uninitialized;
    for(Size i = 0; i < pending.size(); i++) {
        if(pending[i] != pointer) continue;

        pending.remove(i);
        break;
    }

    // A type is owed either way. A global whose initializer did not resolve is still a name the rest
    // of the module can write, and leaving it typeless would turn one report into a crash.
    if(!value) {
        definition->type = module.scalar.error;
        return;
    }

    auto type = resolver.valueType(value);

    /*
     * A global cannot hold a borrow, and the reason is the one `analyze_borrow` already gives from
     * the other side: its storage outlives every frame, so there is no extent a reference into one
     * could be checked against. What would be stored is a pointer into the entry function's own
     * frame, which stops existing the moment the program starts.
     */
    if(isBorrow(global, type)) {
        context.diagnostics.error("a global cannot hold a borrow - its storage outlives every frame, so there is nothing for the reference to refer to"_v,
                                  source);
        definition->type = module.scalar.error;
        return;
    }

    // The teardown a global never gets is reported by `checkGlobalTeardown` over every global at
    // once, rather than here: a *constant* one has no initializer to resolve and would have been
    // left out, which is exactly the shape `let &held = Handle {id: 4}` became.

    definition->type = type;
    resolver.initialize(Place::inGlobal(pointer), value, source);
}

/*
 * The root module's top level, as the body of one function - Analysis-Initialization.md stage B.
 *
 * Root-only is the whole of what makes this cheap: there is exactly one module with executable
 * top-level code, so there is no cross-module order to define, no import side effects and no
 * reference analysis to write. What runs here is what was written here, in the order it was
 * written, and `main` - where the module declares one - is called at the end of it.
 */
static void resolveEntryBody(Module& module, Function& function, ModulePtr<Function> main) {
    auto& context = module.context;
    auto parse = module.parse;
    auto local = *module.arena;
    auto source = function.source;

    ExprResolver resolver(context, module, function);

    PendingGlobals uninitialized;
    for(auto statement: module.topLevel.contents(local)) {
        for(auto global_: statement.globals.contents(local)) {
            if(global_ && local[global_]->dynamic) uninitialized.push(global_);
        }
    }

    resolver.uninitialized = &uninitialized;

    for(auto statement: module.topLevel.contents(local)) {
        if(!resolver.current) break;

        auto& decl = *parse[statement.decl];

        if(decl.stmt.kind != ast::Expr::Decl) {
            // An ordinary statement, resolved for its effect. Nothing consumes its value - a top
            // level has nothing to hand one to - which is the same thing a block's non-final
            // statement is.
            resolver.settle(resolver.resolve(decl.stmt, nullptr, false), decl.source);
            continue;
        }

        // The declarations and what each of them declared, walked together. `declareGlobal` pushes
        // one entry per written name including the rejected ones, which is what keeps the two in
        // step - see TopLevelStmt.
        Size index = 0;

        for(auto declaration: decl.stmt.decl.contents(parse)) {
            auto global_ = index < statement.globals.size() ? statement.globals.get(local, index)
                                                            : ModulePtr<Global>(nullptr);
            index++;

            // A constant needs no code at all: it folds at every read and occupies nothing, which is
            // exactly what it did before there was an entry sequence to leave it out of.
            if(!global_ || !local[global_]->dynamic || !declaration.content) continue;
            if(!resolver.current) break;

            initializeGlobal(resolver, global_, declaration);
        }
    }

    /*
     * `main`, last, and its result is the program's status.
     *
     * The call is emitted here rather than left to the native wrapper because it is a fact about the
     * *program* - a program whose top level ran and then called `main` did those two things in that
     * order on every target - and because it is the only thing that gives the JavaScript output a
     * program start at all.
     */
    /*
     * Every dynamic global has a type by now, or is given the error type here.
     *
     * The sweep rather than trusting the loop above, because what the loop guarantees is that each
     * initializer was *reached* - and a statement that leaves no reachable code after it stops the
     * sequence where it is. A global left typeless would be read by whichever body names it next,
     * which is a crash rather than the report that already happened.
     */
    for(auto statement: module.topLevel.contents(local)) {
        for(auto global_: statement.globals.contents(local)) {
            if(!global_ || local[global_]->type) continue;
            local[global_]->type = module.scalar.error;
        }
    }

    ModulePtr<Value> status = nullptr;

    if(main && resolver.current) {
        auto& declaration = *local[main];

        if(declaration.args.isNotEmpty()) {
            context.diagnostics.error("`main` is the program's entry point and cannot take arguments yet - there is no argument or environment model to fill them from"_v,
                                      declaration.source);
        } else if(declaration.gen) {
            context.diagnostics.error("`main` is the program's entry point and cannot be generic - nothing calls it, so there is no call site for its type arguments to come from"_v,
                                      declaration.source);
        } else {
            status = resolver.emitDirectCall(main, {}, declaration.source);
        }
    }

    if(!resolver.current) return;

    /*
     * What the entry answers is what `main` answered, where that is something a process can report.
     * A result held in storage is not: the status leaves through a register on the way to C's
     * `main`, and there is nothing at the other end to receive an aggregate. Such a result is
     * discarded here and released like any other value the frame owns.
     */
    if(status && !isMemoryType(*module.types, resolver.valueType(status))) {
        function.returnType = resolver.valueType(status);
    } else {
        status = nullptr;
        function.returnType = module.scalar.unit;
    }

    resolver.terminate(resolver.emit<InstRet>(source, StringId(), module.scalar.unit, status));
}

/*
 * Whether the root module's top level has anything to run.
 *
 * A module whose every top-level `let` is a constant is the program every existing fixture is: there
 * is nothing to execute before `main`, so nothing is synthesized and `main` is the entry itself.
 * That is what makes this rule cost nothing where it is not used - the degenerate case is not a
 * special case, it is the general one with an empty statement list.
 */
static bool runsAtStartup(Module& module) {
    auto local = *module.arena;

    for(auto statement: module.topLevel.contents(local)) {
        if(module.parse[statement.decl]->stmt.kind != ast::Expr::Decl) return true;

        for(auto global_: statement.globals.contents(local)) {
            if(global_ && local[global_]->dynamic) return true;
        }
    }

    return false;
}

void resolveProgramEntry(Program& program) {
    auto module = program.root;
    if(!module) return;

    auto& context = program.context;
    auto found = module->functions.get(context.addUnqualifiedName("main", 4));
    ModulePtr<Function> main = found ? found.unwrap() : nullptr;

    if(!runsAtStartup(*module)) {
        program.entry = main;
        return;
    }

    // Anonymous, because nothing in the source can name it: it is reached from `Program::entry` and
    // from the reachability walk that reads it, which is what `anonymous` means everywhere else.
    auto first = module->topLevel.get(*module->arena, 0).decl;
    auto source = first ? module->parse[first]->source : kNullLocation;
    auto function = addAnonymousFunction(*module, context.addUnqualifiedName("main$", 5), source);

    function->returnType = module->scalar.unit;
    program.entry = function - *module->arena;

    resolveEntryBody(*module, *function, main);
}

bool resolveModuleBodies(Module& module) {
    auto success = true;
    auto local = *module.arena;

    /*
     * The functions whose result type their own body decides, first.
     *
     * A call reads its callee's result type, so every one of these has to be known before any body
     * that might call one is resolved - otherwise the answer would depend on declaration order.
     * Doing them as their own pass makes the order they are settled in irrelevant: what remains is
     * one inferring function calling another, which requireReturnType() resolves on demand.
     */
    for(Size i = 0; i < module.functionOrder.size(); i++) {
        auto function = local[module.functionOrder.get(local, i)];
        if(function->inferReturn) success = resolveFunctionBody(module, *function) && success;
    }

    // Resolving one body adds specialized functions to the module, so the list is walked by index
    // rather than by iterator: a specialization created while resolving function 3 is reached
    // when the loop gets to it.
    for(Size i = 0; i < module.functionOrder.size(); i++) {
        success = resolveFunctionBody(module, *local[module.functionOrder.get(local, i)]) && success;
    }

    return success;
}
