/*
 * `?` and `?.`, which are a lens's skipping half without the lens.
 *
 * Two operators rather than one, for the reason recorded in Analysis-Lens.md: `?` unwraps and exits
 * with what it was handed, and `?.` unwraps, applies what follows, and wraps the answer back up
 * again - which needs an instance saying how, and is why `Rewrap` exists. What they share is the
 * `Try` instance selection at the top of this file, and the two-case threading below it.
 */

#include "expr_lens_internal.h"
#include "solve.h"
#include "analyze.h"
#include "generic.h"
#include "name.h"
#include "witness.h"

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

/*
 * What a carrier's `Try` says: the two positions nothing constrains, asked for as holes and answered
 * off whatever the dependency `class Try(m -> a, e)` declares says answers them.
 *
 * This used to be written out by hand and was the compiler's one typeclass resolver that dispatched
 * differently from every other. The rule is now the class's own, which means it is also checked
 * where instances are declared - two `Try` instances for one carrier that disagree about what it
 * proceeds with are rejected, where before they were both accepted and selection answered with
 * whichever came first.
 *
 * Two answers, and the split is the one every class call makes - see fillDependency. A carrier that
 * names a type - `Result(Int, a)`, and `Maybe(a)` over the enclosing function's own variable - is
 * answered by the instance table. A carrier that *is* a variable is answered by the requirement the
 * signature declared, because a body's meaning is fixed by its own signature: reading a blanket
 * instance for `m` would commit `fn (Try(m, a, e)) f() -> m` to that instance and ignore the one the
 * caller's type actually has.
 *
 * `bindGeneric`, because the same question is asked at a lens's declaration, where `m` is `Maybe(a)`
 * over the lens's own variable and what is being checked is that the instance's `a` *is* the
 * continuation's result. It does not lift the rule about a bare variable; fillDependency states that
 * one itself.
 */
static bool tryShape(Module& module, Function& function, TypePtr carrier, TypeList& out,
                     InstanceMatch& instance) {
    auto typeClass = module.coreClasses.try_;
    if(!typeClass || !carrier) return false;

    TypeList asked;
    asked.push(carrier);
    asked.push(nullptr);
    asked.push(nullptr);

    fillDependency(module, function, typeClass, asked, instance, true);
    if(!asked[1] || !asked[2]) return false;

    replaceContents(out, asked);
    return true;
}

// The same query for a caller with no use for the instance - which is every caller that only needs
// to know what the carrier proceeds and exits with.
static bool tryShape(Module& module, Function& function, TypePtr carrier, TypeList& out) {
    InstanceMatch instance;
    return tryShape(module, function, carrier, out, instance);
}

/*
 * The instance as well, for the two call sites that have to *call* one of `Try`'s functions.
 *
 * A shape the enclosing signature's requirement answered has no instance to call: which one serves
 * this carrier is the caller's to decide, and a body constrained by `Try(m, a, e)` reaches it
 * through a dispatch rather than through here. So this is the shape plus the one thing the
 * requirement half cannot supply.
 */
bool selectTry(Module& module, Function& function, TypePtr carrier, TrySelection& out) {
    TypeList shape;
    InstanceMatch instance;

    if(!tryShape(module, function, carrier, shape, instance) || !instance) return false;

    out.instance = instance.instance;
    replaceContents(out.instanceArgs, instance.args);

    out.proceeds = shape[1];
    out.reason = shape[2];

    return tryFunction(module, "toOutcome"_v, out.toOutcome);
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
