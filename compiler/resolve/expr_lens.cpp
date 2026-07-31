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
 * `Try(m, a, e)` is keyed on `m` alone, so the two type arguments nothing constrains are asked for
 * as nulls and read back off whichever instance matched. That is the load-bearing half of reading B:
 * the resolver never has to relate a return type to a type *constructor* applied to a variable - the
 * higher-kinded machinery Implementation-Generics.md part 7 fences off - because the question it
 * actually needs answered is "which case of the wrapper means the continuation ran", and only an
 * instance can answer that.
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

static bool selectTry(Module& module, TypePtr carrier, LocationId source, TrySelection& out) {
    auto typeClass = module.coreClasses.try_;
    if(!typeClass) return false;

    auto global = *module.types;
    auto local = *module.arena;

    TypePtr asked[] = { carrier, nullptr, nullptr };
    auto match = matchInstance(module, typeClass, { asked, 3 });
    if(!match) return false;

    auto instance = local[match.instance];
    if(instance->forTypes.size() != 3) return false;

    out.instance = match.instance;
    replaceContents(out.instanceArgs, match.args);

    auto bindings = toBuffer(out.instanceArgs);
    out.proceeds = substituteType(module, instance->forTypes.get(local, 1), bindings, source);
    out.reason = substituteType(module, instance->forTypes.get(local, 2), bindings, source);

    // Which slot `toOutcome` is, by name. The class is Core's, but its declaration order is a detail
    // of the source rather than something this file may assume - the same rule the Outcome
    // constructors above are found under.
    auto name = Context::nameHash("toOutcome", 9);
    for(auto fun: global[typeClass]->functions.contents(global)) {
        if(fun.name == name) {
            out.toOutcome = fun.index;
            return out.proceeds && out.reason;
        }
    }

    return false;
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

    auto explicitForm = false;
    if(function.args.size()) {
        auto last = local[function.args.get(local, function.args.size() - 1)];
        explicitForm = !last->isLazy() && global[last->declaredType()]->kind == Type::Fun;
    }

    /*
     * An `iter fn` that wrote its continuation out has written the step signal by hand, and the two
     * halves of it - what `Proceed` means to the loop below and what the payload of an `Exit` is -
     * are decided by the `for` desugaring rather than by the declaration. Accepting the shape would
     * mean checking a written type against one this file constructs, and the diagnostic for getting
     * it wrong would be about a type the author never meant to name.
     */
    if(iterator && explicitForm) {
        context.diagnostics.error("an `iter fn` declares what it hands over and uses `yield` in this version - the explicit continuation form would have to write the step signal out, and what its cases mean is decided by the `for` loop rather than by this declaration"_v,
                                  source);
        function.funKind = ast::FunKind::Plain;
        return;
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
     * The function this `yield` hands over for is the one it is written in, and no other.
     *
     * That is a restriction as much as a rule. `iter fn evens(n): for x in upTo(n): yield x` -
     * one iterator written over another, which is the shape every adaptor has - writes its `yield`
     * inside a *lifted* loop body, so the parameter it calls would be a capture. What blocks it is
     * not the capture: it is that a `for` loop cannot appear in an `iter fn` body at all, because
     * an iterator is generic in what its consumer returns and a lifted body inside a generic
     * function needs specializing alongside it. See makeContinuation.
     */
    if(function.funKind == ast::FunKind::Plain || !function.yieldForm) {
        context.diagnostics.error("`yield` is only available inside a `lens fn` or `iter fn` that declares what it hands over - one that named its continuation parameter calls it by name instead"_v,
                                  expr.source);
        return nullptr;
    }

    auto iterator = function.funKind == ast::FunKind::Iter;
    auto kindName = iterator ? "this iterator"_v : "this lens"_v;

    auto continuation = (ModulePtr<Value>)function.args.get(local, function.args.size() - 1);
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

    yields.push(LensYield { current, expr.source });

    auto result = emitDynamicCall(continuation, toBuffer(args), expr.source, 0);
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
            auto allPaths = block->incoming.size() != 0;

            for(auto incoming: block->incoming.contents(local)) {
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
        if(local[block->terminator]->kind != Value::Ret) continue;

        context.diagnostics.error("this path leaves a `lens fn` without yielding - the continuation would never run, so the block below the call site would be skipped"_v,
                                  local[block->terminator]->source);
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

    ModulePtr<Value> compared[] = {
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
    return load(project(placeFor(value, source), ProjectionKind::Downcast, index), source);
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
                                  Buffer<ModulePtr<Value>> args, LocationId source, Array<FunArg>& out) {
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
        auto declared = local[target->args.get(local, i)];
        resolver.bindPosition(declared->declaredType(), resolver.valueType(args[i]), bindings, true);
    }

    /*
     * The slots the arguments left open, filled with the variables themselves.
     *
     * At least one is always open - the continuation's own result is what the block below is about
     * to decide, and nothing above it can have - so an empty slot is the ordinary case rather than
     * a failure. Substituting a variable for itself leaves the position generic, which is exactly
     * the test below: a handed type that is still generic afterwards is one this call did not
     * decide, and one that is concrete is decided whatever else was left open.
     */
    auto env = functionGen(global, *target);
    for(Size i = 0; i < bindings.size(); i++) {
        if(bindings[i]) bindings[i] = resolver.settleType(bindings[i]);
        else bindings[i] = (Type*)global[env->types.get(global, i)] - global;
    }

    for(auto declared: callback->args.contents(global)) {
        auto type = declared.type;

        if(isGeneric(global, type)) {
            auto substituted = substituteType(module, type, toBuffer(bindings), source);

            if(!substituted || isGeneric(global, substituted)) {
                module.context.diagnostics.error("this call does not decide what %@ hands over - write the continuation out as a final argument, or say what the type arguments are"_v,
                                                 source, module.context.findName(target->name));
                return false;
            }

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
    if(functionGen(global, function)) {
        /*
         * The same restriction a lambda in a generic body already carries, and for the same reason:
         * the lifted body names the enclosing function's type variables, so specializing the caller
         * would have to specialize it too, and a function *value* that is still generic needs the
         * witness Implementation-Generics.md keeps behind its fence.
         *
         * Worth naming for a lens or an iterator, because one of those is generic without looking
         * it: the variable is the one its own continuation returns. That is what stops an adaptor -
         * an `iter fn` whose body is a `for` over another - from being writable in this version.
         */
        auto kindName = function.funKind == ast::FunKind::Iter ? "an `iter fn` is generic in what the loop body below it returns, so its own body is a generic body"_v
                      : function.funKind == ast::FunKind::Lens ? "a `lens fn` is generic in what its continuation returns, so its own body is a generic body"_v
                      : "the lifted body would have to be specialized alongside its caller"_v;

        context.diagnostics.error(loop
            ? "a `for` loop over an iterator inside a generic function is not available yet - %@"_v
            : "a lens call inside a generic function is not available yet - %@"_v,
            source, kindName);
        return nullptr;
    }

    auto lifted = addAnonymousFunction(module, continuationFunctionName(module), source);
    lifted->used = true;
    lifted->takesEnv = true;

    auto envTuple = new (module.types) TupType;
    envTuple->named = true;

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
                                          ValueList& values) {
    auto target = local[callee];
    Size position = 0;

    for(auto arg: arguments.contents(parse)) {
        if(arg.name) {
            context.diagnostics.error("named call arguments are not available yet"_v, arg.value.source);
        }

        auto expected = local[target->args.get(local, position)]->declaredType();
        auto value = resolve(arg.value, isGeneric(global, expected) ? nullptr : expected);

        values.push(isGeneric(global, expected) ? settle(value, arg.value.source) : value);
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

    auto callee = findFunction(module, calleeExpr.var, call.source);
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

    ValueList values;
    resolveHandedArguments(callee, arguments, values);

    Array<FunArg> params;
    if(!continuationSignature(*this, module, callee, toBuffer(values), source, params)) return true;

    ContinuationShape shape;
    auto continuation = makeContinuation(toBuffer(params), declaration, block, index + 1, source, shape, skipping);
    if(!continuation) return true;

    values.push(continuation);

    auto call_ = target->gen ? emitGenericCall(callee, toBuffer(values), source, nullptr, 0)
                             : emitDirectCall(callee, toBuffer(values), source, nullptr, 0);

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

    ModulePtr<Value> carried[] = { call_ };
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
    Array<BranchArm> arms;

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
ModulePtr<Function> ExprResolver::findLoopIterator(const ast::ForExpr& loop, const ast::AppExpr*& call) {
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

    auto callee = findFunction(module, calleeExpr.var, source.source);
    if(!callee) return nullptr;

    auto target = local[callee];
    if(target->funKind != ast::FunKind::Iter) {
        context.diagnostics.error("%@ is not an `iter fn`, so a `for` loop has nothing to be the body of - a collection is iterated by an `iter fn` over it rather than directly"_v,
                                  loop.from.source, context.findName(target->name));
        return nullptr;
    }

    if(application.args.size() != target->args.size() - 1) {
        context.diagnostics.error("%@ takes %@ arguments before the loop body, but this call was given %@"_v,
                                  loop.from.source, context.findName(target->name),
                                  U32(target->args.size() - 1), U32(application.args.size()));
        return nullptr;
    }

    call = &application;
    return callee;
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
    auto callee = findLoopIterator(loop, application);
    if(!callee) return;

    auto target = local[callee];
    auto source = expr.source;

    ValueList values;
    resolveHandedArguments(callee, application->args, values);

    Array<FunArg> params;
    if(!continuationSignature(*this, module, callee, toBuffer(values), source, params)) return;

    ContinuationShape shape;
    auto continuation = makeContinuation(toBuffer(params), nullptr, {}, 0, source, shape, false, &loop);
    if(!continuation) return;

    values.push(continuation);

    auto call_ = target->gen ? emitGenericCall(callee, toBuffer(values), source, nullptr, 0)
                             : emitDirectCall(callee, toBuffer(values), source, nullptr, 0);

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
