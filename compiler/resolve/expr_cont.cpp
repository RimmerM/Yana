/*
 * A continuation, as a function.
 *
 * The block a lens call left out is lifted into a real function: the values it reads from around it
 * become an environment it is handed, its result becomes an outcome saying whether it ran to the
 * end, and a `return`, `break` or `continue` written inside it becomes an exit that the lens's
 * caller - not the lens - has to complete. That last part is the whole difficulty, and
 * finishContinuationExits is where it is paid for.
 */

#include "expr_lens_internal.h"
#include "solve.h"
#include "analyze.h"
#include "generic.h"
#include "name.h"
#include "witness.h"

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
        discriminant = ref(emit<InstUnary>(source, StringId(), module.scalar.int_, Value::Cast, value));
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
        auto moved = create<InstMove>(source, StringId(), content, place);
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

        body.terminate(body.emit<InstRet>(exit.source, StringId(), module.scalar.unit,
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
bool continuationSignature(ExprResolver& resolver, Module& module, ModulePtr<Function> callee,
                                  Buffer<ResolvedArg> args, LocationId source, Array<FunArg>& out) {
    auto global = resolver.global;
    auto local = resolver.local;
    auto target = local[callee];

    auto callback = lensContinuationType(global, *target, local);
    if(!callback) return false;

    auto env = functionGen(global, *target);

    /*
     * The ordinary argument solve, over the positions the call site writes: `Skips`, because this
     * one is inferring a shape from whatever fit rather than judging a candidate. A position with
     * nothing worked out for it binds nothing - its payload is null, which `valueType` reads as
     * `{}`, and inferring what the callee hands over from that would answer a question about a call
     * the caller has already stopped - and a position that does not fit is the call's own
     * diagnostic to report, not this step's.
     */
    Solution solution;
    Solver solver(resolver, solution, env);

    auto declaredArgs = target->args.size() - 1;
    solver.bindArguments(callee, { args.ptr, min(args.length, declaredArgs) }, Unresolved::Skips);

    /*
     * A variable the callee's own constraints determine is decided by them and not by this call -
     * the same step solveSignature takes, for the same reason, and it is what an iterator over a
     * container needs.
     *
     * `iter fn (Chunked(c, a)) items(self: c) -> a` mentions `a` in one place only: the type it
     * hands over. So the arguments decide `c` and nothing decides `a`, and without this the element
     * type reads as open and the call is rejected for not saying what it yields - when `Chunked(c ->
     * a)` says exactly that. Which is the shape every bulk operation over a container has, and
     * therefore the one this had to learn.
     */
    if(env) solver.settleDependencies(*env, source);

    auto& bindings = solution.types;

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
    auto open = env ? solver.settleOpen(*env) : 0;

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
 * The same question for a class member, where there is no continuation parameter to read it off.
 *
 * A class declares the *written* shape and stops there - resolveSignature skips the desugaring for
 * one, because it introduces a type variable for the continuation's result and a class has nowhere
 * to put one (Implementation-Containers.md §5). So the signature says `iter fn chunks(self: c) ->
 * &[a]`, and what it returns is what it hands over rather than what a call of it produces.
 *
 * That makes this the easier half of the two. There is nothing to infer: the class's type arguments
 * were decided by selection, so the handed type is the written result read at them, and the shape is
 * the one resolveLensSignature builds from the same sentence - one parameter, or none where nothing
 * is handed over.
 */
bool classContinuationSignature(ExprResolver& resolver, Module& module, ClassMatch& match,
                                LocationId source, Array<FunArg>& out) {
    auto& context = module.context;
    auto global = resolver.global;

    auto entry = global[match.typeClass]->functions.get(global, match.index);
    if(!entry.fun) return false;

    auto handed = substituteType(module, resolver.local[entry.fun]->returnType, toBuffer(match.args), source);
    if(!handed) return false;

    /*
     * A unit hand-over gives a nullary continuation rather than one taking `{}`, which is the rule
     * the desugaring states - there would be nothing at the call site to name.
     *
     * The name has a `$` in it because it is a name the loop body cannot have written, and the
     * shadowing is the reason: a continuation's parameters are bound into the body's scope by name,
     * so a parameter this compiler invented and called `value` silently *replaced* a caller's own
     * `value` inside every `for` body - one type error where the types disagreed and a wrong number
     * where they did not. What binds the hand-over is the loop's pattern; this name is what the
     * dump prints.
     */
    if(!isUnit(global, handed)) {
        out.push(FunArg { handed, context.addUnqualifiedName("value$", 6) });
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

    if(body.captures.isEmpty()) return outer.makeFunValue(type, lifted - local, nullptr, source, StringId());

    auto liftedPointer = (ModulePtr<Function>)(lifted - local);
    closureHeaderFor(module, liftedPointer, envType, source);

    auto storage = outer.allocate(envType, source, StringId(), ast::BindType::Borrow, true);
    ((InstAlloc*)local[storage])->closure = liftedPointer;

    auto place = outer.placeFor(storage, source);
    fillEnvironment(outer, body, place, source);

    auto address = outer.ref(outer.emit<InstAddress>(source, StringId(), funValueFieldType(module, FunValueLayout::kEnv), place));
    return outer.makeFunValue(type, lifted - local, address, source, StringId());
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
        body.terminate(body.emit<InstRet>(source, StringId(), module.scalar.unit,
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

        body.terminate(body.emit<InstRet>(exit.source, StringId(), module.scalar.unit, value));
    }

    for(auto& exit: body.exits) {
        body.current = exit.block;

        auto value = exit.value;
        if(shape.breaks) value = body.makeOutcome(carried, false, value, exit.source);
        else if(value && !isUnit(global, carried)) value = body.convert(value, carried, exit.source);

        body.terminate(body.emit<InstRet>(exit.source, StringId(), module.scalar.unit,
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
            body.terminate(body.emit<InstRet>(source, StringId(), module.scalar.unit,
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

        body.terminate(body.emit<InstRet>(source, StringId(), module.scalar.unit,
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

    terminate(emit<InstRet>(source, StringId(), module.scalar.unit,
                            isUnit(global, function.returnType) ? nullptr : value));
}
