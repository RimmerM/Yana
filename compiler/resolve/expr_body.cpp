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

    // A declaration whose implementation the compiler generates has no body to resolve and never
    // will: what it means is one instruction at each call site rather than anything writable.
    if(function.intrinsic) return true;

    auto& decl = *module.parse[function.ast];
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

            resolver.terminate(resolver.emit<InstRet>(decl.source, 0, module.scalar.unit, result));
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

            resolver.terminate(resolver.emit<InstRet>(decl.source, 0, module.scalar.unit,
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
                resolver.terminate(resolver.emit<InstRet>(decl.source, 0, module.scalar.unit, nullptr));
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
