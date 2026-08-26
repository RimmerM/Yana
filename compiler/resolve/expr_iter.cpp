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
#include "complete.h"

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
        /*
         * A binding, a field, a subscript: something already built.
         *
         * Two readings, and the message used to state only the second. A *container* here is the
         * common one by a wide margin, and what it is missing is not a mechanism but a written
         * call: a `for` reaches its iterator by name and nothing derives one from a type, which is
         * a deliberate absence - a container has several traversals and naming the one meant is the
         * point. The other reading is a value that genuinely is an iterator, which would have to be
         * stepped rather than run; that is external iteration and phase 2 waits on it. Unreachable
         * today in any case, since binding an iterator to a name is itself refused where the call
         * is written, and the author has that diagnostic already.
         */
        context.diagnostics.error("`for` runs a call of a named `iter fn`, and no rule finds one from the type of a value - name the traversal, as in `for x in items(xs)`. A value that is already an iterator is the other reading of this, and stepping one rather than running it is external iteration, which this version does not have"_v,
                                  loop.from.source);
        return nullptr;
    }

    auto& application = *parse[source.app];
    auto& calleeExpr = unwrapNested(application.callee);

    /*
     * Which name the loop reaches its iterator by, and what it wrote to the left of it.
     *
     * Two spellings reach the same rule: `f(as)` names the iterator directly, and `x.f(as)` is
     * Design.md's dot-call form, which is `f(x, as)` and therefore just as much a named `iter fn`.
     * The receiver becomes argument 0 exactly as it does in an ordinary call.
     *
     * `namedCallee` is that decision, and it is shared with the lens statement rather than written
     * here - see expr.h, and expr_call.cpp for why these two ask before resolving where an ordinary
     * call resolves before asking.
     *
     * It is worth being precise about which half of the old rejection the dot form removes: a
     * *single stage* is an ordinary named call and works. A chain - `xs.filter(p).map(f)` - still
     * does not, because its outer receiver is an iterator travelling as a value, which is the
     * erased callback ABI and is unaffected by anything here. The test below says which of the two
     * was written rather than reporting the second reason for both.
     */
    NamedCallee named;
    namedCallee(calleeExpr, application, ast::FunKind::Iter, 1, false, named);

    if(!named.name) {
        // `xs.filter(p).map(f)` lands here, since the callee of the outer call is not a name: an
        // adaptor takes the iterator it wraps as an argument, and passing one needs the erased
        // callback ABI - a function value says nothing about which of its arguments is the
        // continuation, so nothing at the call site can split a block against it.
        context.diagnostics.error("a `for` loop reaches its iterator by name in this version - an iterator passed or returned as a value, which is what an adaptor chain like `xs.filter(p).map(f)` is made of, needs the erased callback ABI"_v,
                                  loop.from.source);
        return nullptr;
    }

    /*
     * The overload set and one selection over it, shared with the lens statement - see
     * selectHandedCallee, which is where the rules a loop and a lens call have in common now live.
     * A loop reaches it holding `iter fn`; that is the whole of the difference.
     */
    HandedCallee selected;
    if(!selectHandedCallee(application, ast::FunKind::Iter, source.source, named, values, selected)) {
        return nullptr;
    }

    // Both halves are the loop's answer, and the caller reads whichever was set: an implementation
    // to desugar the body against, or a class the body is desugared against instead.
    call = &application;
    adopt(dispatch, selected.dispatch);
    return selected.function;
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
