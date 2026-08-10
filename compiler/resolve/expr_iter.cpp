/*
 * `for x in c`, which is a lens call whose continuation is the loop body.
 *
 * Everything a counted `for` does not need: which iterator function the container has, what it
 * yields, and how `break` and `continue` inside the body become exits from the continuation the
 * body was lifted into. The counted form is expr_flow.cpp and shares none of it.
 */

#include "expr_lens_internal.h"
#include "solve.h"
#include "analyze.h"
#include "generic.h"
#include "name.h"
#include "witness.h"

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
                                                   ArgList& values, ClassMatch& dispatch) {
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

    ArgNames names;
    collectArgNames(application.args, names);

    OverloadSet set;
    gatherOverloads(calleeExpr.var, application.args.size(), source.source, calleeExpr.source, set, shape,
                    toBuffer(names));

    /*
     * Which parameter of the sole candidate each written argument fills, where there is a sole
     * candidate at all. The two halves count their parameters differently and the mapping is what
     * has to know it: the plain half is a desugared `iter fn` carrying the continuation the loop
     * supplies, and a class member is not desugared - see CallShape::supplied.
     */
    auto pushdown = pushdownSignature(set);
    ArgMapping mapping;
    const ArgMapping* pushdownMapping = nullptr;

    if(pushdown && mapArguments(pushdown, toBuffer(names), application.args.size(),
                                pushdown == set.direct ? shape.supplied : 0, set.name, source.source,
                                false, mapping)) {
        pushdownMapping = &mapping;
    }

    // Resolved once, whichever half serves the loop - resolving is emission, so there is no
    // resolving a second time and no discarding the first.
    resolveHandedArguments(pushdown, pushdownMapping, application.args, values);
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
            /*
             * The class is known and the instance is not, because the types that would decide it
             * are this body's own variables. There is no implementation to hand back, so the loop
             * is desugared against the class signature and the call is deferred - which is what
             * the match carries out of here in place of a function.
             */
            call = &application;
            adopt(dispatch, selected.match);
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

    // Set in place of a callee where the loop reached a class iterator this body cannot select an
    // instance for. The two are exclusive and one of them is always set on success.
    ClassMatch dispatch;

    auto callee = findLoopIterator(loop, application, values, dispatch);
    if(!callee && !dispatch.typeClass) return;

    auto source = expr.source;

    /*
     * What the loop's body binds, from whichever half answered.
     *
     * A named `iter fn` has been desugared, so its continuation parameter states the shape and the
     * written arguments decide the types in it. A class member has not, so its written result *is*
     * the handed type and selection has already decided the class's arguments - see
     * classContinuationSignature.
     */
    Array<FunArg> params;
    auto shaped = dispatch.typeClass
        ? classContinuationSignature(*this, module, dispatch, source, params)
        : continuationSignature(*this, module, callee, toBuffer(values), source, params);

    if(!shaped) return;

    ContinuationShape shape;
    auto continuation = makeContinuation(toBuffer(params), nullptr, {}, 0, source, shape, false, &loop);
    if(!continuation) return;

    values.push(continuation);

    // The deferred half is told what this call produces, because the class signature does not say:
    // its result is what the iterator hands over, and the step signal is the shape the continuation
    // just settled. See emitGenericDispatch.
    auto call_ = dispatch.typeClass
        ? emitGenericDispatch(dispatch, toBuffer(values), source, StringId(), shape.outcome)
        : emitKnownFunction(callee, toBuffer(values), source, nullptr, StringId());

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

    terminate(emit<InstJe>(source, StringId(), module.scalar.unit, leaving, exitBlock, completedBlock));

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

    terminate(emit<InstJe>(source, StringId(), module.scalar.unit, inner, returnBlock, brokeBlock));

    current = returnBlock;
    emitFunctionReturn(outcomePayload(carried, false, source), source);

    auto afterBlock = addBlock();

    current = brokeBlock;
    terminate(emit<InstJmp>(source, StringId(), module.scalar.unit, afterBlock));

    current = completedBlock;
    terminate(emit<InstJmp>(source, StringId(), module.scalar.unit, afterBlock));

    current = afterBlock;
}
