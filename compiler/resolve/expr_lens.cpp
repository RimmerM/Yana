#include "expr.h"
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
static StringId continuationFunctionName(Module& module) {
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
static TypePtr stepType(Module& module, TypePtr carried, LocationId source) {
    return resolveOutcomeType(module, module.scalar.unit, carried, source);
}

/*
 * The `Try` instance a skipping lens's result carries - Analysis-Lens.md §3.2, §7.1.
 *
 * `class Try(m -> a, e)` is keyed on `m` alone, so the two type arguments nothing constrains are
 * asked for as holes and read back off the instance that matched. That is the load-bearing half of
 * reading B: the resolver never has to relate a return type to a type *constructor* applied to a
 * variable - the higher-kinded machinery Implementation-Generics.md part 7 fences off - because the
 * question it actually needs answered is "which case of the wrapper means the continuation ran", and
 * only an instance can answer that.
 *
 * The keying is the class's declared functional dependency rather than a rule this file knows, so
 * it is enforced where instances are written: two `Try` instances for one carrier that disagree
 * about what it proceeds with are a rejected declaration.
 *
 * Selected twice for one lens, and deliberately: once at the declaration, where `m` still mentions
 * the lens's own variables and what is being checked is that the instance's `a` *is* the
 * continuation's result; and once per call site, where `m` is concrete and what is wanted is the
 * implementation to call.
 */
struct TrySelection {
    ModulePtr<ClassInstance> instance = nullptr;
    TypeList instanceArgs;

    // The instance's own second and third arguments, under the bindings the head match made: what
    // the carrier holds when the continuation ran, and what it holds when it did not.
    TypePtr proceeds = nullptr;
    TypePtr reason = nullptr;

    U16 toOutcome = 0;

    explicit operator bool() const { return instance != nullptr; }
};

/*
 * Which slot one of a Core class's functions is, by name.
 *
 * The classes are Core's, but their declaration *order* is a detail of the source rather than
 * something this file may assume - the same rule the `Outcome` constructors are found under. False
 * when Core has no such function, which is a broken prelude rather than a program error.
 */
static bool classFunction(Module& module, GlobalPtr<TypeClass> typeClass, StringView name, U16& out) {
    auto global = *module.types;
    if(!typeClass) return false;

    auto hash = Context::nameHash(name);
    for(auto fun: global[typeClass]->functions.contents(global)) {
        if(fun.name != hash) continue;

        out = fun.index;
        return true;
    }

    return false;
}

static bool tryFunction(Module& module, StringView name, U16& out) {
    return classFunction(module, module.coreClasses.try_, name, out);
}

static bool selectTry(Module& module, TypePtr carrier, LocationId source, TrySelection& out) {
    auto typeClass = module.coreClasses.try_;
    if(!typeClass) return false;

    /*
     * The dependency `class Try(m -> a, e)` declares does this: the two positions nothing here
     * constrains are asked for as holes and answered off the instance `m` selects.
     *
     * This used to be written out by hand here, and was the compiler's one typeclass resolver that
     * dispatched differently from every other. The rule is now the class's own, which means it is
     * also checked where instances are declared - two `Try` instances for one carrier that disagree
     * about what it proceeds with are rejected, where before they were both accepted and selection
     * answered with whichever came first.
     */
    TypeList asked;
    asked.push(carrier);
    asked.push(nullptr);
    asked.push(nullptr);

    // `bindGeneric`, because this is asked twice and the first time is at the declaration, where
    // `m` is `Maybe(a)` over the lens's own variable and what is being checked is that the
    // instance's `a` *is* the continuation's result.
    auto match = resolveDetermined(module, typeClass, asked, true);
    if(!match) return false;

    out.instance = match.instance;
    replaceContents(out.instanceArgs, match.args);

    out.proceeds = asked[1];
    out.reason = asked[2];

    if(!tryFunction(module, "toOutcome"_v, out.toOutcome)) return false;
    return out.proceeds && out.reason;
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

        if(last->convention != ast::BindType::Borrow || last->returnRoot) {
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
        } else if(!selectTry(module, function.returnType, source, selection)) {
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

    Array<FunArg> callbackArgs;
    if(!isUnit(global, handed)) {
        callbackArgs.push(FunArg { handed, context.addUnqualifiedName("value", 5) });
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

    auto result = emitDynamicCall(continuation, toBuffer(args), expr.source, 0);

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

    terminate(emit<InstJe>(expr.source, 0, module.scalar.unit, leaving, stopped, next));

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
 * The exit signal - Analysis-Lens.md §5.1.
 *
 * A continuation is the rest of the caller's block, so it may contain a `return` that leaves past
 * the lens whose continuation it is. What makes that work with no new mechanism is that the lens is
 * already generic in what its continuation returns: the call site is free to instantiate that
 * variable with `Outcome(blockValue, functionResult)`, and a lens that has cleanup after its
 * `yield` runs that cleanup and hands the value back unchanged without knowing what is in it.
 *
 * So the lens body below is untouched by any of this, and both halves of the signal live at the
 * call site: the continuation wraps, and the glue after the call unwraps and performs the return.
 */

TypePtr resolveOutcomeType(Module& module, TypePtr value, TypePtr exit, LocationId source) {
    auto outcome = module.program.outcomeType;
    if(!outcome) {
        module.context.diagnostics.error("Core has no Outcome type, so a `return` inside a lens continuation cannot be carried out"_v,
                                         source);
        return nullptr;
    }

    TypePtr args[] = { value, exit };
    return instantiateRecord(module, outcome, { args, 2 }, source);
}

ModulePtr<Value> ExprResolver::makeOutcome(TypePtr type, bool proceed, ModulePtr<Value> value,
                                           LocationId source) {
    auto record = (RecordType*)global[type];
    auto index = proceed ? module.program.outcomeProceed : module.program.outcomeExit;

    // Both payloads unit is an ordinary enum, which is its discriminant and nothing else. It is the
    // shape a unit-returning function's exit signal has, so it is a real case rather than a corner.
    if(record->layout == RecordType::Enum) return makeInt(source, type, index);

    auto content = record->constructors.get(global, index).content;
    auto storage = allocate(type, source);
    auto root = placeFor(storage, source);

    if(record->layout == RecordType::Multi) {
        initialize(project(root, ProjectionKind::Discriminant, 0),
                   makeInt(source, module.scalar.int_, index), source);
    }

    if(content && !isUnit(global, content)) {
        auto converted = isMemoryType(global, content) ? value : convert(value, content, source);
        initialize(project(root, ProjectionKind::Downcast, index), converted, source);
    }

    return storage;
}

/*
 * Whether an `Outcome` is the leaving case, as a `Bool`.
 *
 * The one discriminant test the exit signal costs, in the one shape that pays for it. Written
 * against the record's layout rather than as a `match`, because this is emitted code: there is no
 * pattern for `resolvePattern` to read and no source position for a failed one to point at.
 */
ModulePtr<Value> ExprResolver::outcomeIsExit(ModulePtr<Value> value, LocationId source) {
    auto record = (RecordType*)global[valueType(value)];
    ModulePtr<Value> discriminant = nullptr;

    if(record->layout == RecordType::Enum) {
        discriminant = ref(emit<InstUnary>(source, 0, module.scalar.int_, Value::Cast, value));
    } else {
        discriminant = load(project(placeFor(value, source), ProjectionKind::Discriminant, 0), source);
    }

    ResolvedArg compared[] = {
        discriminant,
        makeInt(source, module.scalar.int_, module.program.outcomeExit),
    };

    auto leaving = emitCall(Context::nameHash("==", 2), { compared, 2 }, source, module.scalar.bool_);
    return leaving ? convert(leaving, module.scalar.bool_, source) : nullptr;
}

ModulePtr<Value> ExprResolver::outcomePayload(ModulePtr<Value> value, bool proceed, LocationId source) {
    auto type = valueType(value);
    auto record = (RecordType*)global[type];
    auto index = proceed ? module.program.outcomeProceed : module.program.outcomeExit;
    auto content = record->constructors.get(global, index).content;

    if(!content || isUnit(global, content)) return nullptr;

    auto place = project(placeFor(value, source), ProjectionKind::Downcast, index);

    /*
     * A payload whose type this body cannot see is taken out as a *move*.
     *
     * The signal is dead the moment the branch that examined it took its payload, so this is a
     * handover rather than a read - and a projected *load* of an owning value is exactly what
     * checkTransfer refuses: the handover would find no slot to mark moved, the wrapper would be
     * dropped at its last use, and whoever received the bytes would drop them too.
     *
     * Only where the payload's type is a *variable*, which is the one case a load cannot serve. An
     * unconstrained variable owns something whatever a caller substitutes, so the read the concrete
     * readers below make - and which every fixture of `?`, of a skipping lens and of a loop's exit
     * signal encodes - has no answer here. A `for` loop inside a generic body is what first asks:
     * the carried value's type is that body's own variable.
     */
    auto ownership = ownershipIn(module, functionGen(global, function), content);

    if(isGeneric(global, content) && !ownership.trivialCopy) {
        auto moved = create<InstMove>(source, 0, content, place);
        if(!ownership.trivialSink) moved->sink = sinkFor(module, content, source);

        append(moved);
        return ref(moved);
    }

    return load(place, source);
}

/*
 * Every `return` a continuation collected, terminated now that its result type is known.
 *
 * The blocks were left open rather than terminated with a value that would have had to be rewritten
 * afterwards: appending to a finished block is what makes a CFG walk read one thing and lower
 * another, and the resolver has exactly one rule about that - see the block-list invariants in
 * resolve/lower.cpp.
 */
void finishContinuationExits(Module& module, ExprResolver& body, TypePtr result, TypePtr outcome,
                             LocationId source) {
    auto global = *module.types;

    for(auto& exit: body.exits) {
        body.current = exit.block;

        auto value = exit.value;
        if(outcome) value = body.makeOutcome(outcome, false, value, exit.source);
        else if(value && !isUnit(global, result)) value = body.convert(value, result, exit.source);

        body.terminate(body.emit<InstRet>(exit.source, 0, module.scalar.unit,
                                          isUnit(global, result) ? nullptr : value));
    }

    body.current = nullptr;
}

/*
 * The call site.
 *
 * Everything from here down is one rewrite: `f(as)` followed by the rest of a block becomes
 * `f(as, K)` where `K` is that rest, lifted. The parts that need care are the three the rewrite
 * cannot see by itself - what types the continuation's parameters have, what the continuation
 * returns, and what a `return` inside it means.
 */

/*
 * What the continuation's parameters are, at this call site's types.
 *
 * The callback's declared parameter types may mention the lens's *other* variables -
 * `lens fn each(xs: [a]) -> a` hands an `a` - so the written arguments have to decide those before
 * the continuation can be built. That is the ordinary argument-to-parameter match, run early and
 * for one purpose: nothing here selects anything, and the call below re-runs the whole of selection
 * with the continuation in place.
 */
static bool continuationSignature(ExprResolver& resolver, Module& module, ModulePtr<Function> callee,
                                  Buffer<ResolvedArg> args, LocationId source, Array<FunArg>& out) {
    auto global = resolver.global;
    auto local = resolver.local;
    auto target = local[callee];

    auto callback = lensContinuationType(global, *target, local);
    if(!callback) return false;

    TypeList bindings;
    if(auto env = functionGen(global, *target)) {
        for(Size i = 0; i < env->types.size(); i++) bindings.push(nullptr);
    }

    auto declaredArgs = target->args.size() - 1;
    for(Size i = 0; i < args.length && i < declaredArgs; i++) {
        // A position with nothing worked out for it binds nothing. Its payload is null, which
        // `valueType` reads as `{}`, and inferring the continuation's shape from that would answer
        // a question about a call the caller has already stopped - see anyArgumentFailed, which is
        // what keeps one from reaching here.
        if(args[i].isFailed()) continue;

        auto declared = local[target->args.get(local, i)];
        resolver.bindPosition(declared->declaredType(), resolver.valueType(args[i].value), bindings, true);
    }

    /*
     * Which of the callee's variables this call left open, and then the slots filled with the
     * variables themselves.
     *
     * At least one is always open - the continuation's own result is what the block below is about
     * to decide, and nothing above it can have - so an empty slot is the ordinary case rather than
     * a failure. What is *not* fine is a handed type that depends on one of them.
     *
     * The test is stated over the open positions rather than over the substituted result, and the
     * difference is a generic caller. `iter fn mapped(xs: [a], f: (a) -> b)` looping over
     * `each(xs)` decides `each`'s element type completely - it is `mapped`'s own `a` - and the
     * result is generic all the same, because `a` is a variable here and stays one. Reading
     * "generic" as "undecided" rejected every adaptor for the one property that makes it an
     * adaptor.
     */
    auto env = functionGen(global, *target);
    U64 open = 0;

    for(Size i = 0; i < bindings.size(); i++) {
        if(bindings[i]) {
            bindings[i] = resolver.settleType(bindings[i]);
        } else {
            if(i < 64) open |= U64(1) << i;
            bindings[i] = (Type*)global[env->types.get(global, i)] - global;
        }
    }

    for(auto declared: callback->args.contents(global)) {
        auto type = declared.type;

        if(isGeneric(global, type)) {
            U64 mentioned = 0;
            genVariablesIn(global, type, mentioned);

            if(mentioned & open) {
                module.context.diagnostics.error("this call does not decide what %@ hands over - write the continuation out as a final argument, or say what the type arguments are"_v,
                                                 source, module.context.findName(target->name));
                return false;
            }

            auto substituted = substituteType(module, type, toBuffer(bindings), source);
            if(!substituted) return false;

            type = substituted;
        }

        out.push(FunArg { type, declared.name, declared.convention, declared.returnRoot });
    }

    return true;
}

/*
 * The lifted body, made into the function value the callee is handed.
 *
 * Shared by both kinds of continuation, and identical to what expr_fun.cpp does for a written
 * lambda: a body that named nothing outside itself is a bare code pointer, and one that did gets an
 * environment filled in the frame that builds it. `outer` is that frame - the continuation's
 * captures are its locals, which is what makes the common case a borrow of the stack rather than an
 * allocation.
 */
static ModulePtr<Value> closeContinuation(Module& module, ExprResolver& outer, ExprResolver& body,
                                          Function* lifted, Buffer<FunArg> params, TupType* envTuple,
                                          LocationId source) {
    auto global = outer.global;
    auto local = outer.local;

    Array<FunArg> signature;
    for(auto& param: params) signature.push(param);

    auto type = resolveFunType(module, toBuffer(signature), lifted->returnType, ast::FunKind::Plain);

    auto envType = (Type*)envTuple - global;
    checkTypeAcyclic(module, envType, source);

    if(body.captures.isEmpty()) return outer.makeFunValue(type, lifted - local, nullptr, source, 0);

    auto liftedPointer = (ModulePtr<Function>)(lifted - local);
    closureHeaderFor(module, liftedPointer, envType, source);

    auto storage = outer.allocate(envType, source, 0, ast::BindType::Borrow, true);
    ((InstAlloc*)local[storage])->closure = liftedPointer;

    auto place = outer.placeFor(storage, source);
    fillEnvironment(outer, body, place, source);

    auto address = outer.ref(outer.emit<InstAddress>(source, 0, funValueFieldType(module, FunValueLayout::kEnv), place));
    return outer.makeFunValue(type, lifted - local, address, source, 0);
}

/*
 * What one trip through a `for` body reports back - Analysis-Lens.md §7.3's step signal.
 *
 * Every way out of the body is a `return` of an `Outcome({}, carried)`: falling off the end and
 * `continue` are `Proceed({})`, which is the loop asking for the next element, and `break` and
 * `return` are `Exit`. What `carried` is depends on which of those the body actually uses, and the
 * three answers are the same economy V1 made at a lens call site - the signal costs what the body
 * asks for and nothing more:
 *
 *  - a body that never leaves the enclosing function carries `{}`, so `Exit` means `break` and the
 *    call site emits no test at all: a loop that broke and a loop that ran out both continue below;
 *  - one that returns but never breaks carries the enclosing function's result outright, so `Exit`
 *    means "return this" and the call site is one discriminant test;
 *  - only one that does both needs the two told apart, and that is the one nested `Outcome`.
 */
static ModulePtr<Value> finishLoopContinuation(Module& module, ExprResolver& outer, ExprResolver& body,
                                               Function* lifted, Buffer<FunArg> params, TupType* envTuple,
                                               LocationId source, ContinuationShape& shape) {
    auto global = outer.global;

    shape.fallsThrough = body.current != nullptr;
    shape.exits = body.exits.size() != 0;
    shape.value = module.scalar.unit;

    for(auto& exit: body.loopExits) {
        if(exit.isBreak) shape.breaks = true;
    }

    auto carried = module.scalar.unit;
    if(shape.exits) {
        carried = shape.breaks ? resolveOutcomeType(module, module.scalar.unit, outer.enclosingResultType(), source)
                               : outer.enclosingResultType();
    }

    if(!carried) return nullptr;

    auto step = stepType(module, carried, source);
    if(!step) return nullptr;

    lifted->returnType = step;
    shape.carried = carried;
    shape.outcome = step;

    if(body.current) {
        body.terminate(body.emit<InstRet>(source, 0, module.scalar.unit,
                                          body.makeOutcome(step, true, nullptr, source)));
    }

    for(auto& exit: body.loopExits) {
        body.current = exit.block;

        ModulePtr<Value> value = nullptr;
        if(!exit.isBreak) {
            // `continue` is the next element asked for early, which is what the end of the body
            // says anyway - so it is the same value, produced from a different block.
            value = body.makeOutcome(step, true, nullptr, exit.source);
        } else {
            auto payload = shape.exits ? body.makeOutcome(carried, true, nullptr, exit.source) : nullptr;
            value = body.makeOutcome(step, false, payload, exit.source);
        }

        body.terminate(body.emit<InstRet>(exit.source, 0, module.scalar.unit, value));
    }

    for(auto& exit: body.exits) {
        body.current = exit.block;

        auto value = exit.value;
        if(shape.breaks) value = body.makeOutcome(carried, false, value, exit.source);
        else if(value && !isUnit(global, carried)) value = body.convert(value, carried, exit.source);

        body.terminate(body.emit<InstRet>(exit.source, 0, module.scalar.unit,
                                          body.makeOutcome(step, false, value, exit.source)));
    }

    body.current = nullptr;
    return closeContinuation(module, outer, body, lifted, params, envTuple, source);
}

/*
 * The rest of the block, lifted - or, for a `for` loop, the loop's body.
 *
 * A near-copy of resolveFun's second half, and deliberately so: a continuation is a lambda whose
 * body is a span of statements rather than an expression, and everything else about it - the
 * environment, the discovered captures, their conventions - is the same question answered by the
 * same code. What it adds is the three exit shapes, which is Analysis-Lens.md §5.1 made concrete.
 *
 * `loop` is what makes it the second kind. The two differ in three places and nowhere else: what
 * names the handed values (a `let`'s pattern with its alternatives, or the loop's own pattern), what
 * the body is (the statements after the call, or the loop body written once and run per element),
 * and what the result says (the exit signal's three shapes, or §7.3's step signal). That is a small
 * enough delta to be one function, which is also the claim Implementation-Lens.md part 6 makes about
 * phase 1 as a whole.
 */
ModulePtr<Value> ExprResolver::makeContinuation(Buffer<FunArg> params, const ast::VarDecl* declaration,
                                                ast::ParseList<ast::Expr> block, Size from,
                                                LocationId source, ContinuationShape& shape,
                                                bool skipping, const ast::ForExpr* loop) {
    auto lifted = addAnonymousFunction(module, continuationFunctionName(module), source);
    lifted->used = true;
    lifted->takesEnv = true;

    /*
     * A continuation lifted out of a *generic* body is generic itself, and shares its lifter's
     * context rather than getting one of its own.
     *
     * The body about to be resolved into it names the enclosing function's type variables - an
     * `iter fn` is generic in what the loop body below it returns, so its own body is a generic
     * body, which is why this used to be rejected outright and why an adaptor (an `iter fn` whose
     * body is a `for` over another) could not be written. What it needs is not a witness: it is to
     * be specialized alongside whoever lifted it, and sharing the context is what makes one binding
     * list answer for both. See cloneLifted, which is the other half.
     *
     * `liftedFrom` is what tells the clone which symbols are these. Nothing else changes: the body
     * below is resolved exactly as it is in a concrete function, and the call site is the same call.
     */
    if(functionGen(global, function)) {
        lifted->gen = function.gen;
        lifted->liftedFrom = (ModulePtr<Function>)(&function - local);
    }

    auto envTuple = new (module.types) TupType;
    envTuple->named = true;

    /*
     * Generic before the pointer to it is interned, when the lifter is.
     *
     * An environment's fields are discovered as the body names them, so this type exists before any
     * of them do - and `generic` is a *cached* fact that nothing recomputes. The pointer type below
     * is interned from it immediately, and an interned `%Env` that recorded "not generic" is one
     * substituteType walks past: the specialization would keep a parameter typed over the caller's
     * variables while the environment it points at had been substituted, and the two would disagree
     * about how wide it is.
     *
     * Set from the lifter rather than from the fields, because that is the question this answers -
     * a continuation lifted out of a generic body is generic whether or not it captures anything.
     */
    if(functionGen(global, function)) ((Type*)envTuple)->generic = true;

    auto envPointer = resolvePointerType(module, (Type*)envTuple - global);
    auto envArgValue = lifted->addArg(module, context.addUnqualifiedName("env", 3), envPointer, source);

    ValueList handed;
    for(auto& param: params) {
        auto declared = lifted->addArg(module, param.name, param.type, source);
        declared->convention = param.convention;
        handed.push((ModulePtr<Value>)(declared - local));
    }

    ExprResolver body(context, module, *lifted);
    body.enclosing = this;
    body.envArg = (ModulePtr<Value>)(envArgValue - local);
    body.envType = envTuple;
    body.exitType = enclosingResultType();
    body.inContinuation = true;
    body.inLoopBody = loop != nullptr;

    bindFunctionArgs(body, module, *lifted, 1);

    // The names the call site wrote for what is handed over. One parameter binds the pattern
    // directly; several are gathered into the record the pattern was written against, which is what
    // makes `let {before, hit, after} = ...` and `for {key, value} in ...` mean what they read as.
    if(declaration || loop) {
        auto& pattern = loop ? loop->pat : declaration->pat;
        ModulePtr<Value> pivot = nullptr;

        if(handed.size() == 0) {
            // `let _ = f(...)` is the exception, and the one shape that needs it is a *skipping*
            // lens handing nothing over: `| else ->` only has a place to be written beside a `let`,
            // so a `tryLock`-shaped call has no other way in. Nothing is bound either way - the
            // wildcard names nothing - so the two cases differ only in whether it was asked for.
            if(pattern.kind != ast::Pat::Any) {
                context.diagnostics.error(loop
                    ? "this iterator hands over nothing, so the loop's pattern has nothing to bind - an `iter fn` a `for` loop names has to declare what it yields"_v
                    : "this lens hands over nothing, so there is nothing for `let` to bind - write the call as a statement of its own, or `let _ =` it if the alternatives are what the `let` is for"_v,
                    pattern.source);
            }
        } else if(handed.size() == 1) {
            pivot = handed[0];
        } else {
            Array<Field> fields;
            for(Size i = 0; i < params.length; i++) {
                if(!params[i].name) {
                    context.diagnostics.error("this hands over several values and does not name them, so the pattern cannot destructure them - name the continuation's parameters"_v,
                                              pattern.source);
                    return nullptr;
                }

                fields.push(Field { params[i].type, params[i].name });
            }

            auto tuple = (Type*)resolveTupleType(module, toBuffer(fields), source) - global;
            auto storage = body.allocate(tuple, source);
            auto place = body.placeFor(storage, source);

            for(Size i = 0; i < handed.size(); i++) {
                body.initialize(body.project(place, ProjectionKind::Field, U16(i)), handed[i], source);
            }

            pivot = storage;
        }

        // A loop has no `| else ->` to fail into and no way to say "skip this element", so its
        // pattern has to cover everything the iterator can yield - which is the same rule a `let`
        // without alternatives already follows, said where there is nowhere to put them. A skipping
        // lens's `let` has alternatives and they are spoken for: they say where the *skip* goes, not
        // what to do about a pattern that did not match what was handed over.
        if(pivot && loop) {
            body.bindIrrefutable(pattern, pivot,
                                 "a `for` loop has no alternative to take for an element it does not match"_v);
        } else if(pivot && skipping) {
            body.bindIrrefutable(pattern, pivot,
                                 "the `| else ->` beside a skipping lens call says where the skip goes, so there is nothing left for a pattern that does not match what it handed over"_v);
        } else if(pivot) {
            body.resolveBinding(*declaration, pivot);
        }
    }

    if(loop) {
        body.resolve(loop->body, nullptr, false);
        return finishLoopContinuation(module, *this, body, lifted, params, envTuple, source, shape);
    }

    ModulePtr<Value> result = nullptr;
    auto statements = block.contents(parse);

    for(Size i = from; i < statements.size() && body.current; i++) {
        auto last = i + 1 == statements.size();

        // The same check the block loop in expr.cpp makes, because a continuation *is* a block: a
        // second lens call inside it consumes what is left the same way the first one did, which is
        // what makes `withLock(a)` followed by `withLock(b)` two nested calls rather than an error.
        ModulePtr<Value> lens = nullptr;
        if(body.resolveLensStatement(block, i, last, lens)) {
            result = lens;
            break;
        }

        result = body.resolve(statements[i], nullptr, last);
        if(!last) result = body.settle(result, statements[i].source);
    }

    if(body.current) result = body.settle(result, source);

    shape.fallsThrough = body.current != nullptr;
    shape.exits = body.exits.size() != 0;
    shape.value = shape.fallsThrough ? body.valueType(result) : module.scalar.unit;

    /*
     * The three shapes, and why there are three rather than one.
     *
     * A continuation that never leaves is an ordinary function returning the block's own value, and
     * nothing anywhere pays for the exit signal - which is the common case and the one that has to
     * cost nothing. A continuation every path of which leaves returns the enclosing function's
     * result outright, and the call site returns it: no wrapper is needed because there is no
     * second case to tell apart. Only a continuation that does both needs `Outcome`, and only that
     * one pays for it.
     */
    if(!shape.exits) {
        lifted->returnType = shape.value;
        if(body.current) {
            body.terminate(body.emit<InstRet>(source, 0, module.scalar.unit,
                                              isUnit(global, shape.value) ? nullptr : result));
        }
    } else if(!shape.fallsThrough) {
        lifted->returnType = enclosingResultType();
        finishContinuationExits(module, body, lifted->returnType, nullptr, source);
    } else {
        auto outcome = resolveOutcomeType(module, shape.value, enclosingResultType(), source);
        if(!outcome) return nullptr;

        lifted->returnType = outcome;
        shape.outcome = outcome;

        body.terminate(body.emit<InstRet>(source, 0, module.scalar.unit,
                                          body.makeOutcome(outcome, true, result, source)));

        finishContinuationExits(module, body, outcome, outcome, source);
    }

    return closeContinuation(module, *this, body, lifted, params, envTuple, source);
}

void ExprResolver::emitFunctionReturn(ModulePtr<Value> value, LocationId source) {
    // Before the split below, because both halves hand the value to a caller: a continuation's exit
    // carries it out through the enclosing function's frame, which consumes it just as a plain
    // `ret` does. See returnValue.
    value = returnValue(value, source);

    if(inContinuation) {
        // Left open on purpose: what this block ends with depends on whether any *other* path of
        // the same continuation finishes normally, which is not known until the whole body is
        // resolved. finishContinuationExits comes back for it.
        exits.push(ContinuationExit { current, value, source });
        current = nullptr;
        return;
    }

    terminate(emit<InstRet>(source, 0, module.scalar.unit,
                            isUnit(global, function.returnType) ? nullptr : value));
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
void ExprResolver::resolveHandedArguments(ModulePtr<Function> callee, ast::ParseList<ast::TupArg> arguments,
                                          ArgList& values) {
    auto target = callee ? local[callee] : nullptr;
    Size position = 0;

    for(auto arg: arguments.contents(parse)) {
        if(arg.name) {
            context.diagnostics.error("named call arguments are not available yet"_v, arg.value.source);
        }

        // No signature to push down from is the same case as a generic position: what the argument
        // should be is what selecting a callee from it is about to decide.
        auto expected = target && position < target->args.size()
            ? local[target->args.get(local, position)]->declaredType() : TypePtr(nullptr);

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
    auto declaredArgs = target->args.size() - 1;

    // The continuation written out is an ordinary call, and stays one. That form is always legal
    // and is what a call site reaches for when the rest of the block is not what it wants to run.
    if(arguments.size() != declaredArgs) return false;

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

    ArgList values;
    resolveHandedArguments(callee, arguments, values);

    // The call is this statement, and the rest of the block is its continuation - so stopping here
    // means the block below is resolved as itself rather than lifted, which is the same thing every
    // other diagnostic in this function does. See anyArgumentFailed.
    if(anyArgumentFailed(toBuffer(values))) return true;

    Array<FunArg> params;
    if(!continuationSignature(*this, module, callee, toBuffer(values), source, params)) return true;

    ContinuationShape shape;
    auto continuation = makeContinuation(toBuffer(params), declaration, block, index + 1, source, shape, skipping);
    if(!continuation) return true;

    values.push(continuation);

    auto call_ = emitKnownFunction(callee, toBuffer(values), source, nullptr, 0);

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
    if(!selectTry(module, valueType(call_), source, selection)) {
        context.diagnostics.error("%@ returns %@ here, which has no `Try` instance to say whether its continuation ran"_v,
                                  source, context.findName(target->name),
                                  describeType(context, global, valueType(call_)));
        return true;
    }

    ResolvedArg carried[] = { call_ };
    auto outcome = emitInstanceCall(module, selection.instance, toBuffer(selection.instanceArgs),
                                    selection.toOutcome, { carried, 1 }, source, nullptr, 0);

    if(!outcome || !current) return true;

    auto leaving = outcomeIsExit(outcome, source);
    if(!leaving) return true;

    auto skipBlock = addBlock();
    auto proceedBlock = addBlock();

    terminate(emit<InstJe>(source, 0, module.scalar.unit, leaving, skipBlock, proceedBlock));

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

    terminate(emit<InstJe>(source, 0, module.scalar.unit, leaving, exitBlock, proceedBlock));

    current = exitBlock;
    emitFunctionReturn(outcomePayload(value, false, source), source);

    current = proceedBlock;
    return used ? outcomePayload(value, true, source) : nullptr;
}


/*
 * Which iterator a `for` loop names, or nothing after saying why this one cannot be named.
 *
 * The rule is V1's, one level up: the callee is a *named* `iter fn` and the loop's source is a call
 * of it with its written arguments. Everything else is one of Implementation-Lens.md part 6's phase
 * 1 exclusions, and each is reported with the reason attached rather than as "not a call" - those
 * rejections are the evidence base a phase 2 strategy gets chosen from, so a message that does not
 * say which shape was reached is a message that teaches nothing.
 */
ModulePtr<Function> ExprResolver::findLoopIterator(const ast::ForExpr& loop, const ast::AppExpr*& call,
                                                   ArgList& values) {
    auto& source = unwrapNested(loop.from);

    if(source.kind != ast::Expr::App) {
        // A binding, a field, a subscript: something already built, being stepped rather than run.
        // That is external iteration whichever way it is reached, and phase 2 is what it waits on.
        context.diagnostics.error("`for` iterates a call of a named `iter fn` in this version - a value that is already an iterator would have to be stepped by the loop rather than run by it, which is external iteration"_v,
                                  loop.from.source);
        return nullptr;
    }

    auto& application = *parse[source.app];
    auto& calleeExpr = unwrapNested(application.callee);

    if(calleeExpr.kind != ast::Expr::Var || findBinding(calleeExpr.var)) {
        // `xs.filter(p).map(f)` lands here, since the callee of the outer call is not a name: an
        // adaptor takes the iterator it wraps as an argument, and passing one needs the erased
        // callback ABI - a function value says nothing about which of its arguments is the
        // continuation, so nothing at the call site can split a block against it.
        context.diagnostics.error("a `for` loop reaches its iterator by name in this version - an iterator passed or returned as a value, which is what an adaptor chain like `xs.filter(p).map(f)` is made of, needs the erased callback ABI"_v,
                                  loop.from.source);
        return nullptr;
    }

    /*
     * The overload set, and one selection over it - Design.md's R5, run by the same function an
     * ordinary call runs.
     *
     * A `for` loop used to have its own: its own gather, its own R5, its own ambiguity report and
     * its own missing-instance tracking. Three separate defects lived in the gap - the class half
     * was shadowed outright, the wrong signature was pushed into the arguments, and the wrong class
     * was blamed for a missing instance - and every one of them was a rule the ordinary selection
     * already kept. What genuinely differs is three facts about the *call site*, and they are what
     * CallShape carries: the callee is declared `iter fn`, the loop supplies the last argument
     * itself, and there is no generic dispatch to defer an undecided match to.
     */
    CallShape shape;
    shape.kind = ast::FunKind::Iter;
    shape.requiresKind = true;
    shape.supplied = 1;
    shape.dispatches = false;

    OverloadSet set;
    gatherOverloads(calleeExpr.var, application.args.size(), source.source, calleeExpr.source, set, shape);

    // Resolved once, whichever half serves the loop - resolving is emission, so there is no
    // resolving a second time and no discarding the first.
    resolveHandedArguments(pushdownSignature(set), application.args, values);
    if(anyArgumentFailed(toBuffer(values))) return nullptr;

    ResolvedCallee selected;
    selectCallee(set, toBuffer(values), nullptr, source.source, selected);

    // Whatever selection settled on, including a borrow it read through - see ResolvedCallee::args.
    replaceContents(values, selected.args);

    switch(selected.kind) {
        case ResolvedCallee::Kind::Failed:
            return nullptr;

        case ResolvedCallee::Kind::Plain:
            call = &application;
            return selected.function;

        case ResolvedCallee::Kind::Instance: {
            // The implementation the instance supplies is an `iter fn` like any other, desugared
            // where it was written - so everything the loop does below this point is unchanged.
            call = &application;
            return local[selected.match.instance]->functions.get(local, selected.match.index);
        }

        case ResolvedCallee::Kind::Dispatch:
            // `shape.dispatches` is false, so selection never answers this.
            context.diagnostics.error("internal: a `for` loop was given a deferred class dispatch"_v,
                                      loop.from.source);
            return nullptr;
    }

    return nullptr;
}

/*
 * `for pat in f(as): body` - Design.md's Iterators, Analysis-Lens.md §7.3.
 *
 * The same rewrite a lens call site performs, with the continuation spelled by the loop rather than
 * found: `pat` names what the iterator hands over and the loop body is what runs per element, in
 * place of "the pattern of the `let`" and "everything left in this block". Nothing here suspends
 * anything - the iterator's frame stays live for the whole loop and each `yield` is a call out and
 * an ordinary return back in, which is what makes phase 1 a delta on V1's lowering rather than a
 * second mechanism.
 *
 * What follows the call is the same join, reading the step signal the body chose: see
 * finishLoopContinuation for why there are three shapes of it and which one costs anything.
 */
void ExprResolver::resolveFor(const ast::Expr& expr, const ast::ForExpr& loop) {
    // `for i in 0 .. n` counts over an interval and shares nothing with the iterator form but its
    // spelling: no continuation is lifted and no `iter fn` is named. See resolveCountedFor.
    if(loop.to) {
        resolveCountedFor(expr, loop);
        return;
    }

    if(loop.step) {
        context.diagnostics.error("`step` belongs to a counted `for` and needs the interval it steps over - write `a .. b step n`, or drop it to iterate what the call yields one at a time"_v,
                                  expr.source);
        return;
    }

    const ast::AppExpr* application = nullptr;

    // Filled by the lookup, because a class iterator is selected *from* its arguments and resolving
    // them a second time would run whatever they do a second time too.
    ArgList values;

    auto callee = findLoopIterator(loop, application, values);
    if(!callee) return;

    auto target = local[callee];
    auto source = expr.source;

    Array<FunArg> params;
    if(!continuationSignature(*this, module, callee, toBuffer(values), source, params)) return;

    ContinuationShape shape;
    auto continuation = makeContinuation(toBuffer(params), nullptr, {}, 0, source, shape, false, &loop);
    if(!continuation) return;

    values.push(continuation);

    auto call_ = emitKnownFunction(callee, toBuffer(values), source, nullptr, 0);

    if(!call_ || !current) return;

    /*
     * A loop whose body never leaves the enclosing function has nothing to report: a `break` and a
     * completed iterator both continue below, so the step signal is consumed by the iterator alone
     * and the call site reads none of it.
     */
    if(!shape.exits) return;

    auto leaving = outcomeIsExit(call_, source);
    if(!leaving) return;

    auto exitBlock = addBlock();
    auto completedBlock = addBlock();

    terminate(emit<InstJe>(source, 0, module.scalar.unit, leaving, exitBlock, completedBlock));

    current = exitBlock;
    auto carried = outcomePayload(call_, false, source);

    if(!shape.breaks) {
        emitFunctionReturn(carried, source);
        current = completedBlock;
        return;
    }

    /*
     * The body both broke and returned, so what came out has to be told apart. `Exit` inside is the
     * enclosing function's result and leaves; `Proceed` is a `break`, which lands where the loop
     * running out lands - so the two of them join, and the join block is created last so that the
     * block list stays in the reverse postorder resolve/lower.cpp walks it in.
     */
    auto inner = outcomeIsExit(carried, source);
    if(!inner) return;

    auto returnBlock = addBlock();
    auto brokeBlock = addBlock();

    terminate(emit<InstJe>(source, 0, module.scalar.unit, inner, returnBlock, brokeBlock));

    current = returnBlock;
    emitFunctionReturn(outcomePayload(carried, false, source), source);

    auto afterBlock = addBlock();

    current = brokeBlock;
    terminate(emit<InstJmp>(source, 0, module.scalar.unit, afterBlock));

    current = completedBlock;
    terminate(emit<InstJmp>(source, 0, module.scalar.unit, afterBlock));

    current = afterBlock;
}

/*
 * `x?` - Implementation-Semantics.md part 5.
 *
 * The whole of the operator is one match:
 *
 *     match Try.toOutcome(x):
 *       Proceed(v) -> v
 *       Exit(e)    -> return Try.fromExit(e)
 *
 * with one asymmetry that is the entire reason it is compiler-known rather than a declared suffix
 * operator: the `fromExit` is selected for the *enclosing function's* return type, not for the
 * operand's. Every other continuation-shaped form in part 6's table has its skip destination
 * written at the call site - a guard's alternatives, an `is`'s `else`, a `for`'s exit - and this
 * one's is the enclosing return edge, typed by the enclosing signature. Nothing a declaration can
 * write names that. Everything else about `?` is ordinary: `Try` is a class in Core, and a carrier
 * joins by writing an instance.
 *
 * Here rather than in expr.cpp because it shares every piece of machinery with the skipping lens
 * above - the same class, the same two `Outcome` readers, and the same emitFunctionReturn, which
 * already knows that leaving a lifted continuation leaves the frame the block was split out of.
 * That is what makes `?` inside a lens continuation need no rule of its own: it departs exactly
 * where a written `return` there departs.
 */

/*
 * What a carrier's `Try` says, in a body where the carrier may still be a type variable.
 *
 * Two answers, and the split is the one every class call makes. A carrier that names a type -
 * `Result(Int, a)`, and `Maybe(a)` over the enclosing function's own variable - is answered by the
 * instance table with the class's dependency filling the two positions nothing constrains. A
 * carrier that *is* a variable is answered by the requirement the signature declared, because a
 * body's meaning is fixed by its own signature: reading a blanket instance for `m` would commit
 * `fn (Try(m, a, e)) f() -> m` to that instance and ignore the one the caller's type actually has.
 */
static bool tryShape(Module& module, Function& function, TypePtr carrier, TypeList& out) {
    auto typeClass = module.coreClasses.try_;
    if(!typeClass || !carrier) return false;

    auto global = *module.types;

    TypeList asked;
    asked.push(carrier);
    asked.push(nullptr);
    asked.push(nullptr);

    if(global[carrier]->kind != Type::Gen) {
        // `bindGeneric`, for the same reason a lens declaration asks that way: what is wanted is
        // the *shape* of the instance, and `Try(Maybe(a), a, {})` is that shape whether or not `a`
        // is decided here. The restriction it lifts is about a bare variable, which the branch
        // above has already excluded.
        if(!resolveDetermined(module, typeClass, asked, true)) return false;

        replaceContents(out, asked);
        return asked[1] && asked[2];
    }

    if(auto env = functionGen(global, function)) {
        TypeList declared;

        if(findClassRequirement(module, *env, typeClass, toBuffer(asked), declared) && declared.size() == 3) {
            replaceContents(out, declared);
            return declared[1] && declared[2];
        }
    }

    return false;
}

/*
 * One of `Try`'s functions, called for a shape tryShape worked out.
 *
 * The instance is selected here and not there, because the two callers below want different things
 * from the same shape: `toOutcome`'s is read for its `Outcome` types before anything is emitted,
 * and `fromExit`'s decides what the exit payload has to be converted to first.
 */
static ModulePtr<Value> emitTry(ExprResolver& resolver, Module& module, Buffer<TypePtr> shape, U16 index,
                                Buffer<ResolvedArg> args, LocationId source,
                                GlobalPtr<TypeClass> typeClass = nullptr) {
    auto global = resolver.global;

    ClassMatch match;
    match.typeClass = typeClass ? typeClass : module.coreClasses.try_;
    match.index = index;
    for(auto type: shape) match.args.push(type);

    // A type argument that is still this body's own variable selects no instance here and the
    // dispatch travels to whoever specializes - which is the rule resolveClassCall applies to every
    // class call, said again because this one does not go through it.
    if(match.args.contains([&](TypePtr type) { return isGeneric(global, type); })) {
        return resolver.emitGenericDispatch(match, args, source, 0);
    }

    match.instance = resolver.selectInstance(match.typeClass, toBuffer(match.args), match.instanceArgs);
    if(!match.instance) return nullptr;

    return resolver.emitInstanceCall(module, match.instance, toBuffer(match.instanceArgs), match.index,
                                     args, source);
}

ModulePtr<Value> ExprResolver::resolveTry(const ast::Expr& expr, TypePtr target, bool used, bool implicit) {
    auto source = expr.source;

    /*
     * What a rejected `?` produces, which is a poisoned value rather than nothing.
     *
     * Every diagnostic below is about the operator, and the binding it was written into is
     * ordinarily fine - `let v = source()?` in a function returning `Int` binds a perfectly good
     * `v`. Answering with nothing would unbind it and turn one message about the signature into a
     * message about the signature plus one "unknown scalar value v" per use. The error type is the
     * one every consumer already knows to stay quiet about, which is what materializeLiteral's own
     * failure hands back for the same reason.
     */
    auto failed = [&]() { return constant<ConstInt>(source, module.scalar.error, 0); };

    /*
     * The operand first, and settled.
     *
     * Settled because `?` is a statement boundary for what it is applied to in the same way `let`
     * is for its initializer: the instance selected below is the operand's own, chosen for its
     * type, so a literal still deciding what it is would pick one and then change underneath it.
     * A literal has no `Try` instance at any width, so what this actually buys is the diagnostic
     * naming `Int` rather than naming a literal.
     */
    auto value = settle(resolve(*parse[expr.tryValue]), source);
    if(!current) return nullptr;
    if(!value) return failed();

    auto carrier = valueType(value);

    // Already reported, by whatever produced it. Passing the poison straight through keeps this
    // operator out of a diagnostic it has nothing to add to.
    if(global[carrier]->kind == Type::Error) return value;

    if(!module.coreClasses.try_) {
        context.diagnostics.error("Core has no `Try` class, so `?` has nothing to ask which path a value is on"_v,
                                  source);
        return failed();
    }

    /*
     * Where `?` may be written, asked of the function it would *leave* rather than of the one it is
     * written in. Inside a lifted continuation those are different functions - the block was split
     * out of an enclosing body, and Design.md's Leaving through a lens says a departure there leaves
     * that body - so this follows the same chain `return` does, and for the same reason.
     */
    auto& leaving = enclosingBody().function;

    if(leaving.funKind == ast::FunKind::Iter) {
        context.diagnostics.error("`?` leaves the enclosing function, and an `iter fn` ends by running out of values rather than by returning - what it produces is the loop's own signal, which is not a carrier `?` can rebuild"_v,
                                  source);
        return failed();
    }

    if(leaving.funKind == ast::FunKind::Lens && !leaving.skipping) {
        context.diagnostics.error("`?` cannot be written in a transparent `lens fn` - it returns *by* calling its continuation, so it has no return edge of its own for `?` to take. A lens that may skip declares the carrier it skips through, and `?` is accepted in one of those"_v,
                                  source);
        return failed();
    }

    /*
     * The enclosing function's result type, which `?` needs before it can resolve anything.
     *
     * Null when that function is an `=` form whose result its own body decides - the cycle
     * requireReturnType reports for a recursive one, reached one step earlier and by a different
     * route. Worth its own sentence because the fix is three characters and because the alternative
     * is a null type turning into a complaint about a missing instance for nothing in particular.
     */
    auto result = enclosingResultType();

    if(!result) {
        if(enclosingBody().resultInferred) {
            context.diagnostics.error("this lambda's result type is decided by its body, so `?` cannot be written in it - `?` returns from the enclosing function, and that needs the type first. Write the lambda where a function type is expected"_v,
                                      source);
        } else {
            context.diagnostics.error("`?` returns from the enclosing function, so it needs that function's result type - and the `=` form infers its result from the body this is written in. Write the result type out, as in `-> Maybe(Int)`"_v,
                                      source);
        }

        return failed();
    }

    // Already reported by whatever could not decide it.
    if(global[result]->kind == Type::Error) return failed();

    // What the operand carries, and what the enclosing signature can be left through. Both are
    // wanted before anything is emitted: the second decides what the first's exit payload has to be
    // converted to, and neither failing should leave half a branch behind.
    TypeList carried;
    if(!tryShape(module, function, carrier, carried)) {
        context.diagnostics.error("`?` needs %@ to say which of its cases means \"carry on\", and it has no `Try` instance"_v,
                                  source, describeType(context, global, carrier));
        return failed();
    }

    TypeList rebuilt;
    if(!tryShape(module, function, result, rebuilt)) {
        /*
         * The one rule part 5 calls out: this is an error naming the enclosing return type, not a
         * failed overload resolution. `?` in a function returning `Int` is a mistake about the
         * signature - the operator has nowhere to go - and a message about the operand would send
         * the reader to the one part of `let v = parse(text)?` that is fine.
         */
        context.diagnostics.error("`?` returns from the enclosing function when it exits, so that function's result type has to be one it can rebuild - %@ has no `Try` instance"_v,
                                  source, describeType(context, global, result));
        return failed();
    }

    U16 toOutcome = 0, fromExit = 0;
    if(!tryFunction(module, "toOutcome"_v, toOutcome) || !tryFunction(module, "fromExit"_v, fromExit)) {
        context.diagnostics.error("Core's `Try` class does not declare both `toOutcome` and `fromExit`, which `?` needs"_v,
                                  source);
        return failed();
    }

    /*
     * The two reasons, related by `Widen` - part 2b's one step, never a chain.
     *
     * A `Maybe` propagated into a `Result(String, _)` lands here and is rejected: `{}` carries no
     * reason, and nothing can invent one. That is a real mismatch rather than a missing instance,
     * so it is worth its own sentence - the fix is a `| else ->` naming what to fail with, not an
     * instance the author would go looking for.
     */
    auto exitFrom = carried[2];
    auto exitTo = rebuilt[2];

    if(!sameType(exitFrom, exitTo) && !isLiteral(global, exitFrom)) {
        TypePtr pair[] = { exitFrom, exitTo };

        if(!findInstance(module, module.coreClasses.widen, { pair, 2 })) {
            context.diagnostics.error("`?` on %@ exits carrying %@, and this function exits carrying %@ - no `Widen` instance relates the two, so there is nothing to rebuild its result from. Write the failure out with `| else -> ...` instead"_v,
                                      source, describeType(context, global, carrier),
                                      describeType(context, global, exitFrom),
                                      describeType(context, global, exitTo));
            return failed();
        }
    }

    ResolvedArg operand[] = { value };
    auto outcome = emitTry(*this, module, toBuffer(carried), toOutcome, { operand, 1 }, source);
    if(!outcome || !current) return nullptr;

    auto leavingHere = outcomeIsExit(outcome, source);
    if(!leavingHere) return nullptr;

    // Exit first, then proceed, so the block list stays in the reverse postorder resolve/lower.cpp
    // walks it in - the exit arm ends in a return and reaches nothing after it.
    auto exitBlock = addBlock();
    auto proceedBlock = addBlock();

    terminate(emit<InstJe>(source, 0, module.scalar.unit, leavingHere, exitBlock, proceedBlock));

    /*
     * The exit arm.
     *
     * Nothing here drops what an outer expression already evaluated - `f(parse(a)?, parse(b)?)`
     * leaves `a`'s result owned and unconsumed on this path - because that is not this file's job.
     * The value is a tracked local whose liveness ends at this return, which is exactly the fact
     * analyze_drop.cpp reads to place a drop, and it reads it for every early return equally.
     */
    current = exitBlock;

    auto reason = convert(outcomePayload(outcome, false, source), exitTo, source);
    ResolvedArg rebuiltArgs[] = { reason };
    auto carrierValue = emitTry(*this, module, toBuffer(rebuilt), fromExit, { rebuiltArgs, 1 }, source);

    if(current) emitFunctionReturn(convert(carrierValue, result, source), source);

    current = proceedBlock;

    if(!used) return nullptr;

    auto proceeded = outcomePayload(outcome, true, source);
    return proceeded && target ? convert(proceeded, target, source, implicit) : proceeded;
}

/*
 * `a?.b` - optional chaining.
 *
 * A different operator from `?`, and the difference is the whole of it: `?` skips to the enclosing
 * function's return edge, and `?.` skips to the end of the chain it is written in. So
 * `row?.name.trim()` is a `Maybe(String)` and nothing departs, which is what every language with
 * this spelling means by it - and it is why the two cannot be one token. `(x?).name` is still
 * available and still leaves the function; the two read differently because they are different.
 *
 * The skip's extent is syntactic, which is what makes this implementable without the continuation
 * machinery next door: the rest of the chain is a *span of AST nodes* rather than the rest of a
 * block, so nothing is lifted and nothing is called. It is one branch per `?.` and one join at the
 * end, threaded through `current` exactly as an `if` is.
 *
 * What the chain comes back in is `Rewrap(m, b -> n)` (Core): the first `?.`'s operand type and
 * whatever the chain produced decide the result type. That is the one thing `Try` cannot say, since
 * it reads a carrier to what is inside it and this needs the other direction.
 */

// The nodes of the chain `expr` tops, innermost last. A chain is a `selexpr` with call, subscript,
// field and `?.` suffixes on it - the same spine parseChain builds, walked back down.
static void optionalSpine(ast::ParseBase parse, const ast::Expr& expr,
                          SmallArray<const ast::Expr*, 8>& out) {
    auto node = &expr;

    while(node) {
        out.push(node);

        switch(node->kind) {
            // Not `Nested`: parentheses end a chain. `(a?.b).c` is a `?.` whose chain is what the
            // parentheses hold, and then a field read of the `Maybe` that produced - which is the
            // whole point of being able to write them.
            case ast::Expr::Field:  node = &parse[node->field]->target; break;
            case ast::Expr::Unwrap: node = parse[node->unwrap]; break;
            case ast::Expr::App:    node = &parse[node->app]->callee; break;
            case ast::Expr::Sub:    node = &parse[node->sub]->callee; break;
            default: return;
        }
    }
}

bool ExprResolver::chainSkips(const ast::Expr& expr) {
    SmallArray<const ast::Expr*, 8> spine;
    optionalSpine(parse, expr, spine);

    return spine.contains([](const ast::Expr* node) { return node->kind == ast::Expr::Unwrap; });
}

/*
 * One `?.` inside a chain.
 *
 * Everything before the join: unwrap, branch, and carry on down the proceeding arm with the payload
 * in hand. The skip arm is recorded rather than finished, because what it has to produce is the
 * chain's own result type and the chain has not been resolved yet.
 *
 * What is written *after* the `?.` never reaches here. `a?.b` is a field of this, `a?.[i]` a
 * subscript of it and `a?.(x)` a call of it, each resolved by the code that already resolves those
 * - so the three spellings cost one node and no cases.
 */
ModulePtr<Value> ExprResolver::resolveUnwrap(const ast::Expr& expr) {
    auto source = expr.source;
    auto failed = [&]() { return constant<ConstInt>(source, module.scalar.error, 0); };

    /*
     * Reached without a chain to skip to, which is a resolver mistake rather than a program one:
     * the dispatch enters resolveOptionalChain at the top of any chain containing one of these, so
     * every `?.` that is resolved at all is resolved inside that span.
     */
    if(!optionalChain) {
        context.diagnostics.error("internal: `?.` resolved outside the chain it skips to the end of"_v,
                                  source);
        return failed();
    }

    auto value = settle(resolve(*parse[expr.unwrap]), source);
    if(!current) return nullptr;
    if(!value) return failed();

    auto carrier = valueType(value);
    if(global[carrier]->kind == Type::Error) return value;

    TypeList carried;
    if(!tryShape(module, function, carrier, carried)) {
        context.diagnostics.error("`?.` needs %@ to say which of its cases means \"carry on\", and it has no `Try` instance"_v,
                                  source, describeType(context, global, carrier));
        return failed();
    }

    U16 toOutcome = 0;
    if(!tryFunction(module, "toOutcome"_v, toOutcome)) {
        context.diagnostics.error("Core's `Try` class does not declare `toOutcome`, which `?.` needs"_v, source);
        return failed();
    }

    // The first one decides what the chain comes back in. A later `?.` in the same chain may hold a
    // different carrier - `a?.b?.c` where the two are a `Maybe` and a `Result` - and only their
    // reasons have to meet, which the join does with the ordinary Widen step.
    if(!optionalChain->carrier) optionalChain->carrier = carrier;

    ResolvedArg operand[] = { value };
    auto outcome = emitTry(*this, module, toBuffer(carried), toOutcome, { operand, 1 }, source);
    if(!outcome || !current) return nullptr;

    auto leaving = outcomeIsExit(outcome, source);
    if(!leaving) return nullptr;

    auto skipBlock = addBlock();
    auto proceedBlock = addBlock();

    terminate(emit<InstJe>(source, 0, module.scalar.unit, leaving, skipBlock, proceedBlock));

    // Left open, holding what the carrier exited with. The join comes back for it once the chain's
    // result type - and therefore what this has to be rebuilt as - is known.
    current = skipBlock;

    optionalChain->skips.push(OptionalSkip {
        skipBlock, outcomePayload(outcome, false, source), carried[2], source,
    });

    current = proceedBlock;

    auto payload = outcomePayload(outcome, true, source);
    if(!payload) {
        context.diagnostics.error("`?.` unwraps %@ into %@, which carries no value for the rest of this chain to be about"_v,
                                  source, describeType(context, global, carrier),
                                  describeType(context, global, carried[1]));
        return failed();
    }

    return payload;
}

/*
 * The whole chain, from its topmost node.
 *
 * Entered before anything below is resolved, because the join every `?.` in it branches to has to
 * exist first. What is resolved underneath is the ordinary chain - the `?.` nodes find this through
 * `optionalChain` and add themselves to it - so nothing here duplicates field access or calls.
 */
ModulePtr<Value> ExprResolver::resolveOptionalChain(const ast::Expr& expr, TypePtr target, bool used,
                                                    bool implicit) {
    auto source = expr.source;
    auto failed = [&]() { return constant<ConstInt>(source, module.scalar.error, 0); };

    OptionalChain chain;
    optionalSpine(parse, expr, chain.spine);

    // Saved and restored rather than assigned, so that a chain written inside this one's arguments
    // is its own - `a?.b(c?.d)` has two joins, and the inner `?.` belongs to the inner one.
    auto outer = optionalChain;
    optionalChain = &chain;

    auto produced = resolve(expr, nullptr, used, false);

    optionalChain = outer;

    // Every path of the chain left, which is a chain whose first `?.` could not proceed at all.
    // There is no value to rewrap, and the skips still have to be finished.
    if(!chain.carrier || chain.skips.isEmpty()) return produced;

    // Nothing came back, which means something below already said why - an unknown field, most
    // often. Poison rather than a unit payload, or the wrapper built around it turns one message
    // about the field into a second about `Maybe(())`.
    if(!produced && current) return failed();

    auto payload = produced ? valueType(produced) : module.scalar.unit;
    if(global[payload]->kind == Type::Error) return produced;

    /*
     * What the chain comes back in - `Rewrap(m, b -> n)`.
     *
     * The one question `Try` cannot answer: it reads a carrier to what is inside it, and this needs
     * a carrier and a payload to give back the wrapper around that payload. An ordinary
     * three-parameter class keyed on its first two, rather than a type constructor applied to a
     * variable, for the same reason `Try` is - see Core.
     */
    auto rewrapClass = module.coreClasses.rewrap;
    TypeList rewrapShape;

    if(rewrapClass) {
        rewrapShape.push(chain.carrier);
        rewrapShape.push(payload);
        rewrapShape.push(nullptr);
    }

    if(!rewrapClass || !resolveDetermined(module, rewrapClass, rewrapShape, true)) {
        context.diagnostics.error("`?.` on %@ has to put what the rest of this chain produced - %@ - back into the same kind of wrapper, and there is no `Rewrap` instance saying which type that is"_v,
                                  source, describeType(context, global, chain.carrier),
                                  describeType(context, global, payload));
        return failed();
    }

    auto result = rewrapShape[2];

    // What the result exits with, which is what every skip's reason has to become.
    TypeList rebuilt;
    if(!tryShape(module, function, result, rebuilt)) {
        context.diagnostics.error("`?.` produces %@ here, which has no `Try` instance to build its empty case with"_v,
                                  source, describeType(context, global, result));
        return failed();
    }

    U16 rewrapIndex = 0, fromExit = 0;
    if(!classFunction(module, rewrapClass, "rewrap"_v, rewrapIndex) ||
       !tryFunction(module, "fromExit"_v, fromExit)) {
        context.diagnostics.error("Core's `Rewrap` and `Try` classes do not declare both `rewrap` and `fromExit`, which `?.` needs"_v,
                                  source);
        return failed();
    }

    auto exitTo = rebuilt[2];

    /*
     * The proceeding arm: what the chain produced, put back in the wrapper it came out of.
     *
     * `current` is null when every path of the chain skipped, which a chain ending in a `return`
     * can do. The join then has only the skip arms, and no phi.
     */
    BranchArmList arms;

    if(current) {
        ResolvedArg wrapped[] = { produced };
        auto value = emitTry(*this, module, toBuffer(rewrapShape), rewrapIndex, { wrapped, 1 }, source,
                             rewrapClass);

        if(current) arms.push(BranchArm { current, value, source });
    }

    /*
     * The skip arms, finished now that there is a type for them to produce.
     *
     * Each holds the reason its own carrier exited with, and each is widened into the result's -
     * part 2b's one step, never a chain. `a?.b?.c` over two carriers that disagree lands here.
     */
    for(auto& skip: chain.skips) {
        current = skip.block;

        if(!sameType(skip.reasonType, exitTo) && !isLiteral(global, skip.reasonType)) {
            TypePtr pair[] = { skip.reasonType, exitTo };

            if(!findInstance(module, module.coreClasses.widen, { pair, 2 })) {
                context.diagnostics.error("this `?.` skips carrying %@, and the chain it is in comes back carrying %@ - no `Widen` instance relates the two"_v,
                                          skip.source, describeType(context, global, skip.reasonType),
                                          describeType(context, global, exitTo));
                return failed();
            }
        }

        auto reason = convert(skip.reason, exitTo, skip.source);
        ResolvedArg args[] = { reason };
        auto empty = emitTry(*this, module, toBuffer(rebuilt), fromExit, { args, 1 }, skip.source);

        if(current) arms.push(BranchArm { current, empty, skip.source });
    }

    /*
     * The mistake this operator is most likely to be met with, named before it becomes a conversion
     * error about a type the author did not write.
     *
     * `?.` and `?` look related and are not: a chain containing `?.` comes back *wrapped*, because
     * the skip goes to the end of the chain rather than out of the function. Someone who expected
     * Rust's `?` writes `node(n)?.id` in a function returning `Int` and is otherwise told only that
     * `Maybe(Int)` is not `Int`, which is true and explains nothing.
     */
    if(target && !sameType(result, target) && sameType(payload, target)) {
        context.diagnostics.error("`?.` produces %@ rather than %@ - a chain containing it comes back wrapped, because the skip goes to the end of the chain and not out of this function. `?` is the operator that leaves, so write `(x?).field` if that is what was meant"_v,
                                  source, describeType(context, global, result),
                                  describeType(context, global, target));
        return failed();
    }

    auto joined = finishBranches(arms, source, used);
    return joined && target ? convert(joined, target, source, implicit) : joined;
}
