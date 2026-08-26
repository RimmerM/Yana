#include "expr_lens_internal.h"
#include "solve.h"
#include "analyze.h"
#include "generic.h"
#include "name.h"
#include "witness.h"

/*
 * Lenses - Design.md's Lens functions, and Analysis-Lens.md's V1 through V3.
 *
 * A lens is an ordinary function whose last parameter is a continuation and whose result is
 * whatever that continuation returns. Everything below is about the two ends of that sentence:
 *
 *  - a *declaration* may leave the parameter out (`lens fn withLock(m: &Mutex) -> {}`), in which
 *    case resolveLensSignature synthesizes it and `yield` is the call of it. The type it returns is
 *    a fresh variable, so every lens is generic in its continuation's result and specializes per
 *    call site - which is what makes the whole construct compile away once the callee is known;
 *  - a *call site* may leave the argument out, in which case the rest of the enclosing block is
 *    lifted into that continuation. That is resolveLensCall, and it is the one genuinely new piece
 *    of resolve logic here.
 *
 * The rest of the machinery is already in the file next door: a continuation is lifted exactly the
 * way expr_fun.cpp lifts a lambda, with the same environment, the same discovered captures and the
 * same conventions. A lens is not a second closure mechanism; it is a closure the caller did not
 * have to write.
 *
 * A lens comes in two kinds, and the whole of the difference is one comparison of declared types
 * (Analysis-Lens.md §7.1). A *transparent* lens returns what its continuation returns, so the call
 * site needs nothing written and the continuation runs exactly once. A *skipping* one returns
 * something else - a wrapper carrying a `Try` instance - so it may return without calling the
 * continuation at all, and the call site says where that goes with `| else ->`. **The ability to
 * skip is exactly the presence of a wrapper**: there is no form in which a call may fail to continue
 * into the block below it without that fact appearing in both signatures and in the caller's text.
 */

// The name the synthesized continuation parameter of a `yield`-form lens is printed under. A
// call site may pass it explicitly - the desugared form is an ordinary signature - so it is an
// ordinary name rather than one source cannot write.
/*
 * Lenses - Design.md's Lens functions, and Analysis-Lens.md's V1 through V3.
 *
 * A lens is an ordinary function whose last parameter is a continuation and whose result is
 * whatever that continuation returns. Everything below is about the two ends of that sentence:
 *
 *  - a *declaration* may leave the parameter out (`lens fn withLock(m: &Mutex) -> {}`), in which
 *    case resolveLensSignature synthesizes it and `yield` is the call of it. The type it returns is
 *    a fresh variable, so every lens is generic in its continuation's result and specializes per
 *    call site - which is what makes the whole construct compile away once the callee is known;
 *  - a *call site* may leave the argument out, in which case the rest of the enclosing block is
 *    lifted into that continuation. That is resolveLensCall, and it is the one genuinely new piece
 *    of resolve logic here.
 *
 * The rest of the machinery is already in the file next door: a continuation is lifted exactly the
 * way expr_fun.cpp lifts a lambda, with the same environment, the same discovered captures and the
 * same conventions. A lens is not a second closure mechanism; it is a closure the caller did not
 * have to write.
 *
 * A lens comes in two kinds, and the whole of the difference is one comparison of declared types
 * (Analysis-Lens.md §7.1). A *transparent* lens returns what its continuation returns, so the call
 * site needs nothing written and the continuation runs exactly once. A *skipping* one returns
 * something else - a wrapper carrying a `Try` instance - so it may return without calling the
 * continuation at all, and the call site says where that goes with `| else ->`. **The ability to
 * skip is exactly the presence of a wrapper**: there is no form in which a call may fail to continue
 * into the block below it without that fact appearing in both signatures and in the caller's text.
 */

// The name the synthesized continuation parameter of a `yield`-form lens is printed under. A
// call site may pass it explicitly - the desugared form is an ordinary signature - so it is an
// ordinary name rather than one source cannot write.
static StringId continuationName(Context& context) {
    return context.addUnqualifiedName("body", 4);
}

/*
 * The type variable a `yield`-form lens returns.
 *
 * `$r` rather than something a program could write, because it shares the function's own context
 * with the variables the author declared and shadowing one of those would silently make the
 * continuation's result the same type as an argument.
 */
static StringId continuationResultName(Context& context) {
    return context.addUnqualifiedName("$r", 2);
}

// The name a lifted continuation is printed and linked under.
StringId continuationFunctionName(Module& module) {
    StringBuilder text;
    text << module.context.findName(module.name) << ".continuation$";
    show(module.program.lambdaCounter++, text);

    return module.context.addQualifiedName(text.pointer(), text.size(), 1);
}

/*
 * The step signal an iterator and its consumer exchange - Analysis-Lens.md §7.3.
 *
 * `Outcome({}, r)` in both directions, which is what makes `yield` a pass-through rather than a
 * rebuild: the continuation returns `Proceed({})` to ask for the next element and `Exit(v)` to stop
 * carrying `v`, and the iterator's own result is that same value - `Proceed({})` when its body ran
 * to completion, and whatever a step handed back when one stopped it.
 *
 * §7.3 writes the iterator's own result as `Maybe(r)`, which is the same two cases named differently.
 * One type rather than two means the propagation at a `yield` is `return` of the value it was just
 * given, with nothing unwrapped and rewrapped - and `r` is a variable there, so a rebuild would need
 * the payload's witness in a body that has no reason to want one.
 */
TypePtr stepType(Module& module, TypePtr carried, LocationId source) {
    return resolveOutcomeType(module, module.scalar.unit, carried, source);
}

FunType* lensContinuationType(GlobalBase global, Function& function, ModuleBase local) {
    if(function.funKind == ast::FunKind::Plain || function.args.size() == 0) return nullptr;

    auto last = local[function.args.get(local, function.args.size() - 1)];
    if(global[last->type]->kind != Type::Fun) return nullptr;

    return (FunType*)global[last->type];
}

/*
 * The two declaration forms, reduced to one.
 *
 * A `lens fn` is in the *explicit-callback* form when its last parameter already is an ordinary
 * function type, and in the `yield` form otherwise; the second is sugar for the first, and the
 * desugaring happens here so that only one shape is ever checked, called or lowered.
 *
 * The transparent/skipping split is then one comparison: transparent exactly when the declared
 * result *is* the callback's result. Analysis-Lens.md §7.1 is what that sentence comes from, and
 * the reason it is checkable at all is that the callback's result is a type variable of this
 * function's own context rather than anything inferred.
 *
 * An `iter fn` desugars in the same place and to the same shape, with both of its types fixed by
 * the construct rather than declared by the author (§7.3): the continuation returns the step signal
 * and so does the iterator, which is what makes `break` and `continue` mean one thing in every
 * loop. Only the `yield` form of it exists in this version - see below.
 */
void resolveLensSignature(Module& module, Function& function, GenEnv* env, ast::Decl& decl) {
    auto& context = module.context;
    auto global = *module.types;
    auto local = *module.arena;
    auto source = decl.source;
    auto iterator = function.funKind == ast::FunKind::Iter;
    auto kindName = iterator ? "an iterator"_v : "a lens"_v;

    if(!env) {
        context.diagnostics.error("%@ needs a generic context for its continuation's result type"_v,
                                  source, kindName);
        function.funKind = ast::FunKind::Plain;
        return;
    }

    /*
     * An `iter fn` is always the `yield` form, whatever its last parameter is.
     *
     * The explicit continuation form does not exist for one: writing it out would mean writing the
     * step signal by hand, and the two halves of that - what `Proceed` means to the loop below and
     * what the payload of an `Exit` is - are decided by the `for` desugaring rather than by the
     * declaration. So there is nothing for the shape test below to tell apart, and running it
     * anyway is what rejected `iter fn mapped(xs: [a], f: (a) -> b) -> b` for having a function as
     * its last *written* parameter - which is not a continuation, it is what an adaptor maps with.
     *
     * A lens keeps the test, because for a lens the explicit form is a real declaration.
     */
    auto explicitForm = false;
    if(!iterator && function.args.size()) {
        auto last = local[function.args.get(local, function.args.size() - 1)];
        explicitForm = !last->isLazy() && global[last->declaredType()]->kind == Type::Fun;
    }

    if(explicitForm) {
        auto last = local[function.args.get(local, function.args.size() - 1)];
        auto callback = (FunType*)global[last->type];

        if(callback->kind != ast::FunKind::Plain) {
            context.diagnostics.error("a lens's continuation parameter is an ordinary function - it is what the lens hands its values to, not a second lens"_v,
                                      last->source);
            function.funKind = ast::FunKind::Plain;
            return;
        }

        if(last->convention != ast::BindType::Borrow || last->returnRoot()) {
            context.diagnostics.error("a lens's continuation parameter cannot carry `&`, `->` or `return` - it is called, not stored, and its extent is the call"_v,
                                      last->source);
        }

        // Transparent exactly when the lens returns what its continuation returns. Nothing weaker
        // works as the test: a lens declared `-> Maybe(a)` over a continuation returning `a` is the
        // skipping shape, and the whole point of Analysis-Lens.md §7.1 is that the two are told
        // apart by the presence of that wrapper rather than by an attribute.
        if(function.returnType == callback->result && global[callback->result]->kind == Type::Gen) {
            return;
        }

        if(global[callback->result]->kind != Type::Gen) {
            context.diagnostics.error("a lens's continuation must return a type variable - it produces whatever the rest of the caller's block produces, which the lens cannot name"_v,
                                      last->source);
            function.funKind = ast::FunKind::Plain;
            return;
        }

        /*
         * The skipping half.
         *
         * What the wrapper has to supply is one fact the type itself does not carry: which of its
         * cases means the continuation ran. `Try` is the only thing that answers it, so the check is
         * "there is an instance for this result" plus "the value it proceeds with is what the
         * continuation produces" - and the second is what stops a lens declared `-> Maybe(Int)` over
         * a continuation returning `a` from compiling as though the two were related.
         *
         * Checked here, once, rather than at every call site. `Res` still mentions this function's
         * own variables, which is exactly why it is checkable: `Maybe(a)` matched against
         * `Try(Maybe(x), x, {})` binds `x` to the variable, and comparing what came back with the
         * callback's result is an equality between two of this signature's own types.
         */
        TrySelection selection;
        if(global[function.returnType]->kind == Type::Gen) {
            // A result that is a bare variable is the fully generic carrier - `?`'s own shape. It
            // needs the instance to travel with the call as a witness rather than be selected here,
            // which is the erased path §10 still lists as open.
            context.diagnostics.error("a skipping lens whose result is the type variable %@ is not available yet - the `Try` instance would have to travel with the call as a witness rather than be selected from the signature, which is the erased callback ABI"_v,
                                      source, describeType(context, global, function.returnType));
        } else if(!selectTry(module, function, function.returnType, selection)) {
            context.diagnostics.error("this lens returns %@ rather than its continuation's %@, so it may skip the continuation - which needs an instance of `Try` for %@ saying which of its cases means the continuation ran"_v,
                                      source, describeType(context, global, function.returnType),
                                      describeType(context, global, callback->result),
                                      describeType(context, global, function.returnType));
        } else if(selection.proceeds != callback->result) {
            context.diagnostics.error("this lens returns %@, whose `Try` instance proceeds with %@ rather than with its continuation's %@ - a skipping lens's wrapper has to carry what its continuation produced"_v,
                                      source, describeType(context, global, function.returnType),
                                      describeType(context, global, selection.proceeds),
                                      describeType(context, global, callback->result));
        } else {
            function.skipping = true;
            return;
        }

        function.funKind = ast::FunKind::Plain;
        return;
    }

    /*
     * The `yield` form. What the declaration wrote as its result is the type it *hands over*, so
     * the parameter it did not write takes that as its argument and the function returns the fresh
     * variable instead.
     *
     * A unit hand-over gives a nullary continuation rather than one taking `{}`: `withLock(m)` is
     * written as a bare statement and there is nothing at that call site to name, so a parameter
     * would be one the caller has to spell in the explicit form and no one wants in the sugar.
     */
    auto handed = function.returnType;

    auto variable = genVariable(module, *env, continuationResultName(context));
    if(!variable) {
        context.diagnostics.error("%@ needs an open generic context for its continuation's result type"_v,
                                  source, kindName);
        function.funKind = ast::FunKind::Plain;
        return;
    }

    auto carried = (TypePtr)((Type*)global[variable] - global);

    /*
     * What the continuation returns, which is the whole of the difference between the two kinds.
     *
     * A lens's continuation runs once, so its result travels out of the lens unexamined and the
     * variable itself is the type. An iterator's runs repeatedly and has to be able to say *stop*,
     * so the same variable travels inside the step signal - and the iterator returns that signal
     * rather than the variable, since a body that ran to completion has no carried value at all.
     */
    auto result = carried;
    if(iterator) {
        result = stepType(module, carried, source);
        if(!result) {
            function.funKind = ast::FunKind::Plain;
            return;
        }
    }

    /*
     * The convention what is handed over is received under - Analysis-Language.md §3a.
     *
     * A yielded value has a binding convention exactly as an argument does, and until this there was
     * no way to declare it anything but a borrow: `?` moves a payload out, a borrowed value has
     * nothing to move, and so every consumer of a fallible iterator wrote a `match` in its success
     * path. What the declaration writes as `-> ->T` lands *here*, on the synthesized continuation's
     * own parameter, and from there every pass that has an opinion about a binding reads it where it
     * already looks. Nothing else has to learn a new rule: `yield x` is a call of this parameter, so
     * the move happens at the argument, and a `for` body is this parameter's body, so the loop's
     * name owns what it received and drops it - on `break` as much as on falling out.
     *
     * The `$` is what keeps this out of the loop body's namespace - see classContinuationSignature,
     * where the same name is invented for a class member and the same shadowing was silent.
     */
    Array<FunArg> callbackArgs;
    if(!isUnit(global, handed)) {
        FunArg value { handed, context.addUnqualifiedName("value$", 6) };
        value.convention = decl.fun.retBind;
        callbackArgs.push(value);
    } else if(decl.fun.retBind != ast::BindType::Borrow) {
        // Nothing is handed over, so there is no binding for a convention to be about. Said here
        // rather than ignored, because `-> ->{}` is a sentence somebody meant something by.
        context.diagnostics.error("%@ that hands over nothing has no value for `->` to be about"_v,
                                  source, kindName);
    }

    auto callbackType = resolveFunType(module, toBuffer(callbackArgs), result, ast::FunKind::Plain);
    function.addArg(module, continuationName(context), callbackType, source);
    function.returnType = result;
    function.yieldForm = true;
}

/*
 * `yield e`.
 *
 * A call of the continuation parameter, and nothing else. What makes it worth a case of its own is
 * what surrounds it: the value it produces is what the lens returns, so the body's fall-through
 * return is this value rather than the last statement's, and the one-per-path rule below is stated
 * over the blocks these land in.
 */
ModulePtr<Value> ExprResolver::resolveYield(const ast::Expr& expr) {
    /*
     * The function this `yield` hands over for is the innermost enclosing lens or iterator, which
     * is not always the one this body belongs to.
     *
     * `iter fn evens(n): for x in upTo(n): yield x` - one iterator written over another, which is
     * the shape every adaptor has - writes its `yield` inside a *lifted* loop body, so the
     * continuation it calls belongs to the function that body was lifted out of. `enclosingBody()`
     * walks to it the same way `enclosingResultType()` does, and the parameter is reached by name:
     * the synthesized one is called `body`, an ordinary binding of the enclosing scope, so naming
     * it from here is an ordinary capture and the environment grows a word for it with no rule of
     * this file's own.
     */
    auto& owner = enclosingBody().function;

    if(owner.funKind == ast::FunKind::Plain || !owner.yieldForm) {
        context.diagnostics.error("`yield` is only available inside a `lens fn` or `iter fn` that declares what it hands over - one that named its continuation parameter calls it by name instead"_v,
                                  expr.source);
        return nullptr;
    }

    auto iterator = owner.funKind == ast::FunKind::Iter;
    auto kindName = iterator ? "this iterator"_v : "this lens"_v;

    ModulePtr<Value> continuation = nullptr;

    if(&owner == &function) {
        continuation = (ModulePtr<Value>)owner.args.get(local, owner.args.size() - 1);
    } else if(auto binding = findBinding(continuationName(context), expr.source)) {
        continuation = binding->isPlace() ? load(placeOf(*binding, expr.source), expr.source)
                                          : binding->value;
    }

    if(!continuation) {
        context.diagnostics.error("internal: this `yield` has no continuation to hand over to"_v, expr.source);
        return nullptr;
    }

    if(global[valueType(continuation)]->kind != Type::Fun) {
        context.diagnostics.error("internal: this `yield`'s continuation is not a function"_v, expr.source);
        return nullptr;
    }

    auto callback = (FunType*)global[valueType(continuation)];

    ValueList args;
    if(callback->args.size()) {
        auto handed = callback->args.get(global, 0).type;
        auto value = expr.ret ? resolve(*parse[expr.ret], handed) : nullptr;

        if(!value) {
            context.diagnostics.error("%@ hands over %@, so `yield` needs a value"_v, expr.source,
                                      kindName, describeType(context, global, handed));
            return nullptr;
        }

        args.push(convert(value, handed, expr.source));
    } else if(expr.ret) {
        // A unit hand-over has a nullary continuation, so `yield {}` is the written form and there
        // is nothing to pass. Resolved anyway, so that a value with an effect in it still runs.
        auto value = resolve(*parse[expr.ret], nullptr, false);
        if(value && !isUnit(global, valueType(value))) {
            context.diagnostics.error("%@ hands over nothing, so `yield` cannot carry a value"_v,
                                      expr.source, kindName);
        }
    }

    /*
     * Recorded on the body that hands over, which is not always the one this is written in.
     *
     * For an iterator all the record does is answer "did this ever yield", so a `yield` in a lifted
     * loop body counts and there is nothing else to say. For a lens it also has to answer "on which
     * path", and that question does not survive the split: the blocks are another function's, and
     * the forward fixpoint in checkLensYields is stated over one function's block list. So an
     * adaptor is an iterator's shape and not a lens's, and saying so here is better than accepting
     * it and checking nothing.
     */
    if(&owner != &function) {
        if(!iterator) {
            context.diagnostics.error("a `lens fn` hands over exactly once on every path, so its `yield` cannot be inside a lifted body - this one is in a `for` loop or a lens call's continuation, where the paths belong to another function"_v,
                                      expr.source);
            return nullptr;
        }

        enclosingBody().yields.push(LensYield { current, expr.source });
    } else {
        yields.push(LensYield { current, expr.source });
    }

    auto result = emitDynamicCall(continuation, toBuffer(args), expr.source, StringId());

    // Marked here rather than derived in the ownership passes, because this is the only place that
    // knows the callee is a continuation. A lifted body reaches its enclosing one by capture, so
    // the callable is a load from an environment there and the `Arg` it started as is no longer
    // recognizable - see InstCallDyn::handover.
    if(result && local[result]->kind == Value::CallDyn) {
        ((InstCallDyn*)local[result])->handover = true;
    }

    yieldResult = result;

    if(!iterator || !result || !current) return result;

    /*
     * The iterator half: what came back says whether to carry on.
     *
     * `Proceed` is the loop asking for the next element, and is the fall-through here. `Exit` is the
     * loop leaving - a `break`, or a `return` travelling out of the enclosing function - and the
     * iterator's answer to it is to return that same value, unexamined: it is the consumer's, the
     * type it lives in is a variable of this function's context, and every frame between here and
     * the call site passes it along by an ordinary return.
     *
     * What this version does *not* do is run whatever the body has after the loop the `yield` is in.
     * Design.md's "an iterator's own cleanup after its last `yield` runs when a loop breaks out of
     * it" holds for cleanup written as a lens - `withFile(f)` splits the rest of this body into a
     * continuation, and a `return` from inside it is V1's exit signal, which runs the lens's own
     * cleanup on the way past. Trailing statements of the iterator's own body are skipped, which is
     * this version's boundary and is why a lens is the shape to reach for.
     */
    auto leaving = outcomeIsExit(result, expr.source);
    if(!leaving) return nullptr;

    auto stopped = addBlock();
    auto next = addBlock();

    terminate(emit<InstJe>(expr.source, StringId(), module.scalar.unit, leaving, stopped, next));

    current = stopped;
    emitFunctionReturn(result, expr.source);

    current = next;

    // An iterator hands over a value and produces nothing: `let x = yield e` would be binding the
    // step signal, which is the consumer's business rather than the body's.
    return nullptr;
}

/*
 * A `lens fn` in `yield` form yields exactly once on every path that does not diverge.
 *
 * The same forward fixpoint checkLazyForcing uses, and for the same reason: a yield inside a loop
 * body is a yield per iteration, which one pass over blocks in RPO would clear and the second
 * visit rejects. Two facts are tracked rather than one - whether some path reaching a block has
 * yielded, and whether every path has - because the two failures are different diagnostics: a
 * second yield on one path, and a return that leaves without having yielded at all.
 */
void checkLensYields(Module& module, Function& function, Buffer<LensYield> yields, LocationId source) {
    auto& context = module.context;
    auto local = *module.arena;
    auto blocks = function.blocks.size();

    /*
     * An iterator hands over any number of times, so none of the rule below applies to it. Not even
     * the zero case: a legitimately empty iterator exists - a filter that accepted nothing is one -
     * so a body with no `yield` in it is worth saying out loud and is not worth rejecting, which is
     * what settles Implementation-Lens.md part 2's open call.
     */
    if(function.funKind == ast::FunKind::Iter) {
        if(yields.length == 0) {
            context.diagnostics.warning("this `iter fn` never yields, so every `for` loop over it runs its body no times"_v,
                                        source);
        }

        return;
    }

    if(yields.length == 0) {
        context.diagnostics.error("a `lens fn` in `yield` form must `yield` - this one never hands anything over, so the call site's block below it would never run"_v,
                                  source);
        return;
    }

    if(yields.length > 1) {
        // Every path yielding exactly once is what the rule asks for, and two yields can satisfy it
        // (one per branch of an `if`). What this version cannot do is *return* the result of two of
        // them, since the value would need a join the fall-through return has no arm to build - so
        // the restriction is stated here rather than discovered as a value from the wrong branch.
        context.diagnostics.error("a `lens fn` may contain only one `yield` in this version - the cleanup after it already runs on every path, so a second one would need the two results joined"_v,
                                  yields[1].source);
        return;
    }

    Array<bool> some;
    Array<bool> every;
    for(Size i = 0; i < blocks; i++) { some.push(false); every.push(false); }

    auto yieldBlock = local[yields[0].block]->index;

    for(auto changed = true; changed;) {
        changed = false;

        for(Size i = 0; i < blocks; i++) {
            auto block = local[function.blocks.get(local, i)];

            auto anyPath = false;
            auto allPaths = block->predecessorCount() != 0;

            for(auto incoming: block->incoming(local)) {
                auto predecessor = local[incoming]->index;

                // The two facts are read off different halves of the same edge: one path having
                // yielded is what makes a second yield a repeat, and *every* path having yielded is
                // what makes a return legal. Answering both from `some` would accept a `return` on
                // the branch that did not yield.
                if(some[predecessor]) anyPath = true;
                if(!every[predecessor]) allPaths = false;
            }

            if(i == 0) { anyPath = false; allPaths = false; }

            if(Size(i) == Size(yieldBlock)) {
                if(anyPath) {
                    context.diagnostics.error("this `yield` can run twice - a `lens fn` hands over exactly once on every path that does not diverge"_v,
                                              yields[0].source);
                    return;
                }

                anyPath = true;
                allPaths = true;
            }

            if(some[i] != anyPath || every[i] != allPaths) changed = true;
            some[i] = anyPath;
            every[i] = allPaths;
        }
    }

    for(Size i = 0; i < blocks; i++) {
        auto block = local[function.blocks.get(local, i)];
        if(!block->isComplete() || every[i]) continue;

        // Every way out of the body, which is one list: the fall-through return the caller has
        // already appended is a Ret like any other by the time this runs.
        if(local[block->terminator()]->kind != Value::Ret) continue;

        context.diagnostics.error("this path leaves a `lens fn` without yielding - the continuation would never run, so the block below the call site would be skipped"_v,
                                  local[block->terminator()]->source);
        return;
    }
}

/*
 * The arguments a lens or iterator call actually wrote, resolved.
 *
 * Pushed down only where the parameter's type says something: a generic position is what the
 * argument is being resolved to decide, so there is nothing to push.
 *
 * Settled here, which an ordinary call does not do. The continuation's parameter types come out of
 * these arguments and the whole body below is resolved against them, so a literal still deciding
 * what it is would have that body written against a type that then changed - making the call a
 * statement boundary for its arguments in the same way `let` is for its initializer.
 */
void ExprResolver::resolveHandedArguments(ModulePtr<Function> callee, const ArgMapping* mapping,
                                          ast::ParseList<ast::TupArg> arguments, ArgList& values,
                                          Size leading) {
    auto target = callee ? local[callee] : nullptr;

    // Where the written list starts in the callee's parameters, which is one to the right of zero
    // when a dot-call's receiver already occupies position 0 - see findLoopIterator. The receiver
    // itself was pushed by the caller, since it is resolved before the callee is known.
    Size position = leading;

    for(auto arg: arguments.contents(parse)) {
        // Through the mapping, since a named argument's expected type is the type of the parameter
        // it names rather than of the one in its place - see ArgMapping.
        auto filled = mapping && position < mapping->parameters.size() ? mapping->parameters[position]
                                                                       : U16(position);

        // No signature to push down from is the same case as a generic position: what the argument
        // should be is what selecting a callee from it is about to decide.
        auto expected = target && filled < target->args.size()
            ? local[target->args.get(local, filled)]->declaredType() : TypePtr(nullptr);

        auto erased = !expected || isGeneric(global, expected);
        auto value = resolveArgument(arg.value, erased ? nullptr : expected);

        if(erased && value.isValue()) value = settle(value.value, arg.value.source);

        values.push(value);
        position++;
    }
}

/*
 * `let pat = f(as)` or `f(as)`, where `f` is a lens - Design.md's Calling a lens.
 *
 * The whole of the rewrite is: resolve the written arguments, lift the rest of the block into the
 * argument the call site did not write, and call. What follows the call is the join the three exit
 * shapes need, and only the third of them emits anything at all.
 */
bool ExprResolver::resolveLensStatement(ast::ParseList<ast::Expr> block, Size index, bool used,
                                        ModulePtr<Value>& result) {
    auto statements = block.contents(parse);
    auto statement = statements.pointerAt(index);

    const ast::VarDecl* declaration = nullptr;
    const ast::Expr* written = statement;

    if(statement->kind == ast::Expr::Decl) {
        auto declarations = statement->decl;
        auto contents = declarations.contents(parse);

        // One declaration, with an initializer and no `in` body of its own. A `let` that binds two
        // names has two initializers and no single call for the block to be the continuation of.
        if(contents.size() != 1) return false;

        auto only = contents.pointerAt(0);
        if(!only->content || only->in) return false;

        declaration = only;
        written = parse[only->content];
    }

    auto& call = unwrapNested(*written);
    if(call.kind != ast::Expr::App) return false;

    auto& application = *parse[call.app];
    auto& calleeExpr = unwrapNested(application.callee);

    // By name and by declaration, since a binding of lens function type is reached through the
    // erased callback ABI Implementation-Generics.md still lists as open.
    if(calleeExpr.kind != ast::Expr::Var || findBinding(calleeExpr.var)) return false;

    // Looked up at the call and recorded against the *name*, which is the same split every ordinary
    // call keeps - see findFunction. Recording it against the whole application puts the callee on a
    // span the arguments are inside, so a cursor in one of them walks outwards and finds it.
    auto callee = findFunction(module, calleeExpr.var, call.source, calleeExpr.source);
    if(!callee || local[callee]->funKind != ast::FunKind::Lens) return false;

    auto target = local[callee];
    auto source = call.source;
    auto arguments = application.args;

    /*
     * The written arguments have to reach every parameter but the continuation - which is the same
     * normalization every other call performs, and is asked here for a different purpose: a call
     * that fills the continuation too is an *ordinary* call, and stays one. That form is always
     * legal and is what a call site reaches for when the rest of the block is not what it wants to
     * run, so a list this cannot normalize is handed back rather than reported on.
     */
    ArgNames names;
    collectArgNames(arguments, names);

    ArgMapping mapping;
    if(!mapArguments(callee, toBuffer(names), arguments.size(), 1, calleeExpr.var, source, false, mapping)) {
        return false;
    }

    result = nullptr;

    /*
     * A skipping lens says at the declaration that it may not continue, and the call site has to say
     * where that goes. Both halves of "the ability to skip is exactly the presence of a wrapper" are
     * checked here: a skipping call without alternatives is rejected, and - below - a transparent one
     * never grows a join it did not need.
     */
    auto skipping = target->skipping;
    if(skipping && !declaration) {
        context.diagnostics.error("%@ may skip its continuation, so this call has to say where the skip goes - write it as `let pat = %@(...) | else -> ...`, which is the only position `| else ->` can be written in"_v,
                                  source, context.findName(target->name), context.findName(target->name));
        return true;
    }

    auto alternativeList = declaration ? declaration->alts : ast::ParseList<ast::Alt>();
    if(skipping && alternativeList.contents(parse).size() == 0) {
        context.diagnostics.error("%@ returns %@ rather than what its continuation returns, so it may not continue into the block below - this call needs `| else -> ...` saying what happens then"_v,
                                  source, context.findName(target->name),
                                  describeType(context, global, target->returnType));
        return true;
    }

    ArgList handed;
    resolveHandedArguments(callee, &mapping, arguments, handed);

    // The call is this statement, and the rest of the block is its continuation - so stopping here
    // means the block below is resolved as itself rather than lifted, which is the same thing every
    // other diagnostic in this function does. See anyArgumentFailed.
    if(anyArgumentFailed(toBuffer(handed))) return true;

    // In the callee's parameter order with its defaults filled in, which is what the continuation's
    // own shape is inferred from below - see ArgMapping.
    ArgList values;
    normalizeArguments(mapping, toBuffer(handed), values);
    materializeDefaults(callee, source, values);

    Array<FunArg> params;
    if(!continuationSignature(*this, module, callee, toBuffer(values), source, params)) return true;

    ContinuationShape shape;
    auto continuation = makeContinuation(toBuffer(params), declaration, block, index + 1, source, shape, skipping);
    if(!continuation) return true;

    values.push(continuation);

    auto call_ = emitKnownFunction(callee, toBuffer(values), source, nullptr, StringId());

    if(!call_ || !current) return true;

    if(!skipping) {
        result = finishLensCall(call_, shape, used, source);
        return true;
    }

    /*
     * The skip join - Analysis-Lens.md §3.2's four steps, of which only the last two emit anything.
     *
     * `toOutcome` is the one call that turns "some wrapper the program named" into two cases this
     * file can branch on. Everything downstream of it is the same `match` V1 already emits for the
     * exit signal, and the two nest rather than interfering: the outer test asks whether the
     * continuation ran at all, and the inner one - inside finishLensCall - asks whether it left the
     * enclosing function when it did.
     */
    TrySelection selection;
    if(!selectTry(module, function, valueType(call_), selection)) {
        context.diagnostics.error("%@ returns %@ here, which has no `Try` instance to say whether its continuation ran"_v,
                                  source, context.findName(target->name),
                                  describeType(context, global, valueType(call_)));
        return true;
    }

    ResolvedArg carried[] = { call_ };
    auto outcome = emitInstanceCall(module, selection.instance, toBuffer(selection.instanceArgs),
                                    selection.toOutcome, { carried, 1 }, source, nullptr, StringId());

    if(!outcome || !current) return true;

    auto leaving = outcomeIsExit(outcome, source);
    if(!leaving) return true;

    auto skipBlock = addBlock();
    auto proceedBlock = addBlock();

    terminate(emit<InstJe>(source, StringId(), module.scalar.unit, leaving, skipBlock, proceedBlock));

    /*
     * The alternatives, and then the ordinary call site underneath them.
     *
     * The two arms join at the *block's* value rather than at the continuation's result, which is
     * what lets an alternative be written at all: what the continuation returns is a type the call
     * site chose (§5.1's three shapes, one of which is an `Outcome` no program can name), while what
     * the block produces is the type the code below the call was already going to have.
     */
    BranchArmList arms;

    current = skipBlock;
    resolveSkipAlternatives(*declaration, outcomePayload(outcome, false, source), used, arms);

    current = proceedBlock;
    result = finishLensCall(outcomePayload(outcome, true, source), shape, used, source);
    if(current) arms.push(BranchArm { current, result, source });

    result = finishBranches(arms, source, used);
    return true;
}

/*
 * What the call site does with the value the continuation produced - Analysis-Lens.md §5.1's third
 * bullet, and the whole of a transparent lens's join.
 *
 * Nothing is emitted for a continuation that never leaves, which is the case the lowering has to be
 * fast for. A continuation every path of which leaves produced the enclosing function's result, so
 * the call site performs that return - which is what makes the `if` arm holding a lens call diverge
 * rather than falling into the code after the `if`. Only a continuation that does both is wrapped,
 * and only that one pays for a test.
 *
 * Shared with the skipping form, which reaches it holding the same value by a different route: what
 * the continuation returned, unwrapped from the carrier the lens skipped through.
 */
ModulePtr<Value> ExprResolver::finishLensCall(ModulePtr<Value> value, ContinuationShape& shape, bool used,
                                              LocationId source) {
    if(!current) return nullptr;
    if(!shape.exits) return value;

    /*
     * A null value here is not a failure. The skipping form reaches this holding a payload
     * projection, and a payload of unit type is nothing to project - which is what a continuation
     * whose result is `{}` produces, and an enclosing function returning nothing is exactly that.
     * emitFunctionReturn wants nothing in that case either, so the two agree.
     */
    if(!shape.fallsThrough) {
        emitFunctionReturn(value, source);
        return nullptr;
    }

    // The wrapped shape, which is the only one that reads a discriminant. `value` is an `Outcome`
    // here by construction, so it is never the unit the projection above can be absent for.
    auto leaving = value ? outcomeIsExit(value, source) : nullptr;
    if(!leaving) return nullptr;

    auto exitBlock = addBlock();
    auto proceedBlock = addBlock();

    terminate(emit<InstJe>(source, StringId(), module.scalar.unit, leaving, exitBlock, proceedBlock));

    current = exitBlock;
    emitFunctionReturn(outcomePayload(value, false, source), source);

    current = proceedBlock;
    return used ? outcomePayload(value, true, source) : nullptr;
}
