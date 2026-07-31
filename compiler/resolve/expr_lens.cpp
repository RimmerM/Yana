#include "expr.h"
#include "analyze.h"
#include "generic.h"
#include "name.h"
#include "witness.h"

/*
 * Lenses - Design.md's Lens functions, and Analysis-Lens.md's V1.
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
 * What is *not* here is the skipping half (Analysis-Lens.md's V3): a lens whose result type is not
 * its continuation's must carry a `Try` instance and its call site must carry `| else ->`. This
 * version reports that shape at the declaration rather than accepting it with the wrapper ignored,
 * because "the ability to skip is exactly the presence of a wrapper" only holds if a wrapper cannot
 * be silently dropped.
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

FunType* lensContinuationType(GlobalBase global, Function& function, ModuleBase local) {
    if(function.funKind != ast::FunKind::Lens || function.args.size() == 0) return nullptr;

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
 */
void resolveLensSignature(Module& module, Function& function, GenEnv* env, ast::Decl& decl) {
    auto& context = module.context;
    auto global = *module.types;
    auto local = *module.arena;
    auto source = decl.source;

    if(!env) {
        context.diagnostics.error("a lens needs a generic context for its continuation's result type"_v, source);
        function.funKind = ast::FunKind::Plain;
        return;
    }

    auto explicitForm = false;
    if(function.args.size()) {
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
        } else {
            context.diagnostics.error("a skipping lens is not available yet - this one returns %@ rather than its continuation's %@, which needs the `Try` instance and the `| else ->` call site of Analysis-Lens.md's V3"_v,
                                      source, describeType(context, global, function.returnType),
                                      describeType(context, global, callback->result));
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
        context.diagnostics.error("a lens needs an open generic context for its continuation's result type"_v, source);
        function.funKind = ast::FunKind::Plain;
        return;
    }

    auto result = (TypePtr)((Type*)global[variable] - global);

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
    if(function.funKind != ast::FunKind::Lens || !function.yieldForm) {
        context.diagnostics.error("`yield` is only available inside a `lens fn` that declares what it hands over - one that named its continuation parameter calls it by name instead"_v,
                                  expr.source);
        return nullptr;
    }

    auto continuation = (ModulePtr<Value>)function.args.get(local, function.args.size() - 1);
    auto callback = (FunType*)global[valueType(continuation)];

    ValueList args;
    if(callback->args.size()) {
        auto handed = callback->args.get(global, 0).type;
        auto value = expr.ret ? resolve(*parse[expr.ret], handed) : nullptr;

        if(!value) {
            context.diagnostics.error("this lens hands over %@, so `yield` needs a value"_v, expr.source,
                                      describeType(context, global, handed));
            return nullptr;
        }

        args.push(convert(value, handed, expr.source));
    } else if(expr.ret) {
        // A unit hand-over has a nullary continuation, so `yield {}` is the written form and there
        // is nothing to pass. Resolved anyway, so that a value with an effect in it still runs.
        auto value = resolve(*parse[expr.ret], nullptr, false);
        if(value && !isUnit(global, valueType(value))) {
            context.diagnostics.error("this lens hands over nothing, so `yield` cannot carry a value"_v,
                                      expr.source);
        }
    }

    yields.push(LensYield { current, expr.source });

    auto result = emitDynamicCall(continuation, toBuffer(args), expr.source, 0);
    yieldResult = result;

    return result;
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
 * The rest of the block, lifted.
 *
 * A near-copy of resolveFun's second half, and deliberately so: a continuation is a lambda whose
 * body is a span of statements rather than an expression, and everything else about it - the
 * environment, the discovered captures, their conventions - is the same question answered by the
 * same code. What it adds is the three exit shapes, which is Analysis-Lens.md §5.1 made concrete.
 */
ModulePtr<Value> ExprResolver::makeContinuation(Buffer<FunArg> params, const ast::VarDecl* declaration,
                                                ast::ParseList<ast::Expr> block, Size from,
                                                LocationId source, ContinuationShape& shape) {
    if(functionGen(global, function)) {
        context.diagnostics.error("a lens call inside a generic function is not available yet - the continuation would have to be specialized alongside its caller"_v,
                                  source);
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

    bindFunctionArgs(body, module, *lifted, 1);

    // The names the call site wrote for what the lens hands over. One parameter binds the pattern
    // directly; several are gathered into the record the pattern was written against, which is what
    // makes `let {before, hit, after} = ...` mean what it reads as.
    if(declaration) {
        if(handed.size() == 0) {
            context.diagnostics.error("this lens hands over nothing, so there is nothing for `let` to bind - write the call as a statement of its own"_v,
                                      declaration->pat.source);
        } else if(handed.size() == 1) {
            body.resolveBinding(*declaration, handed[0]);
        } else {
            Array<Field> fields;
            for(Size i = 0; i < params.length; i++) {
                if(!params[i].name) {
                    context.diagnostics.error("this lens hands over several values and does not name them, so `let` cannot destructure them - name the continuation's parameters"_v,
                                              declaration->pat.source);
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

            body.resolveBinding(*declaration, storage);
        }
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

    Array<FunArg> signature;
    for(auto& param: params) signature.push(param);

    auto type = resolveFunType(module, toBuffer(signature), lifted->returnType, ast::FunKind::Plain);

    auto envType = (Type*)envTuple - global;
    checkTypeAcyclic(module, envType, source);

    if(body.captures.isEmpty()) return makeFunValue(type, lifted - local, nullptr, source, 0);

    auto liftedPointer = (ModulePtr<Function>)(lifted - local);
    closureHeaderFor(module, liftedPointer, envType, source);

    auto storage = allocate(envType, source, 0, ast::BindType::Borrow, true);
    ((InstAlloc*)local[storage])->closure = liftedPointer;

    auto place = placeFor(storage, source);
    fillEnvironment(*this, body, place, source);

    auto address = ref(emit<InstAddress>(source, 0, funValueFieldType(module, FunValueLayout::kEnv), place));
    return makeFunValue(type, lifted - local, address, source, 0);
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

    ValueList values;
    Size position = 0;

    for(auto arg: arguments.contents(parse)) {
        if(arg.name) {
            context.diagnostics.error("named call arguments are not available yet"_v, arg.value.source);
        }

        /*
         * Pushed down only where the parameter's type says something: a generic position is what
         * the argument is being resolved to decide, so there is nothing to push.
         *
         * Settled here, which an ordinary call does not do. The continuation's parameter types come
         * out of these arguments and the whole block below is resolved against them, so a literal
         * still deciding what it is would have the block written against a type that then changed -
         * making the lens call a statement boundary for its arguments in the same way `let` is for
         * its initializer.
         */
        auto expected = local[target->args.get(local, position)]->declaredType();
        auto value = resolve(arg.value, isGeneric(global, expected) ? nullptr : expected);

        values.push(isGeneric(global, expected) ? settle(value, arg.value.source) : value);
        position++;
    }

    result = nullptr;

    Array<FunArg> params;
    if(!continuationSignature(*this, module, callee, toBuffer(values), source, params)) return true;

    ContinuationShape shape;
    auto continuation = makeContinuation(toBuffer(params), declaration, block, index + 1, source, shape);
    if(!continuation) return true;

    values.push(continuation);

    auto call_ = target->gen ? emitGenericCall(callee, toBuffer(values), source, nullptr, 0)
                             : emitDirectCall(callee, toBuffer(values), source, nullptr, 0);

    if(!call_ || !current) return true;

    /*
     * The join, per Analysis-Lens.md §5.1's third bullet.
     *
     * Nothing is emitted for a continuation that never leaves, which is the case the lowering has to
     * be fast for. A continuation every path of which leaves produced the enclosing function's
     * result, so the call site performs that return - which is what makes the `if` arm holding a
     * lens call diverge rather than falling into the code after the `if`.
     */
    if(!shape.exits) {
        result = call_;
        return true;
    }

    if(!shape.fallsThrough) {
        emitFunctionReturn(call_, source);
        return true;
    }

    auto record = (RecordType*)global[valueType(call_)];
    ModulePtr<Value> discriminant = nullptr;

    if(record->layout == RecordType::Enum) {
        discriminant = ref(emit<InstUnary>(source, 0, module.scalar.int_, Value::Cast, call_));
    } else {
        discriminant = load(project(placeFor(call_, source), ProjectionKind::Discriminant, 0), source);
    }

    ModulePtr<Value> compared[] = {
        discriminant,
        makeInt(source, module.scalar.int_, module.program.outcomeExit),
    };

    auto leaving = emitCall(Context::nameHash("==", 2), { compared, 2 }, source, module.scalar.bool_);
    if(!leaving) return true;

    auto exitBlock = addBlock();
    auto proceedBlock = addBlock();

    terminate(emit<InstJe>(source, 0, module.scalar.unit, convert(leaving, module.scalar.bool_, source),
                           exitBlock, proceedBlock));

    current = exitBlock;
    emitFunctionReturn(outcomePayload(call_, false, source), source);

    current = proceedBlock;
    result = used ? outcomePayload(call_, true, source) : nullptr;
    return true;
}
