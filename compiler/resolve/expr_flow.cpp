/*
 * Control flow: the expressions that build blocks.
 *
 * Every one of these ends up doing the same three things - split the current block, resolve each
 * arm into its own, and join what they produced - and the whole of the difference between them is
 * which arms exist and what decides the edges. `finishBranches` is the join, shared by all of them,
 * and it is where a branch that produced values of different types settles on one.
 *
 * The `for` that iterates a container is not here: it lowers to a lens call, which is
 * expr_iter.cpp. This file has the counted form, which is a loop and nothing else.
 */

#include "expr.h"
#include "complete.h"
#include "generic.h"
#include "name.h"
#include "index.h"

ModulePtr<Value> ExprResolver::finishBranches(BranchArmList& arms, LocationId source, bool used) {
    // Every arm that diverged - returned, or broke out of a loop - left no block behind. If none
    // of them did leave one, the expression as a whole never completes and there is no join.
    if(arms.isEmpty()) {
        current = nullptr;
        return nullptr;
    }

    // An arm with no value is one that could not produce one (a missing `else`, or an error
    // already reported); it makes the whole expression valueless rather than the phi partial.
    auto values = used;
    TypePtr resultType = nullptr;

    for(auto& arm: arms) {
        if(!values) break;
        if(!arm.value) {
            values = false;
            break;
        }

        // An arm that is a bare literal has no type of its own to join with; it takes the default
        // its class names, and the widening below then does what it would for any other pair. The
        // value itself is built in the arm's own block by the conversion loop underneath.
        auto type = settleType(valueType(arm.value));

        if(!type) {
            context.diagnostics.error("nothing decides the type of this literal, and its class has no default"_v,
                                      arm.source);
            values = false;
        } else if(!resultType) {
            resultType = type;
        } else if(global[type]->kind == Type::Error || global[resultType]->kind == Type::Error) {
            // One arm is already broken and said so. What its type disagrees with is not a second
            // fact about this expression.
            resultType = module.scalar.error;
        } else if(!sameType(resultType, type)) {
            if(auto common = commonWiden(resultType, type)) {
                resultType = common;
            } else {
                context.diagnostics.error("branches of this expression have different types"_v, arm.source);
                values = false;
            }
        }
    }

    auto join = addBlock();

    // Each arm's conversion goes at the end of that arm's own block: a phi input has to already
    // have the phi's type in the block it comes from, and the type to convert to is only known
    // once every arm has been seen.
    for(auto& arm: arms) {
        current = arm.end;
        if(values) arm.value = convert(arm.value, resultType, arm.source);
        terminate(emit<InstJmp>(arm.source, StringId(), module.scalar.unit, join));
    }

    current = join;
    if(!values) return nullptr;
    if(arms.size() == 1) return arms[0].value;

    auto phi = create<InstPhi>(source, StringId(), resultType);
    for(auto& arm: arms) phi->inputs.push(module.arena, PhiInput { arm.end, arm.value });
    append(phi);

    auto result = ref(phi);
    if(isMemoryType(global, resultType)) function.addLocal(module, resultType, StringId(), result);

    return result;
}

ModulePtr<Value> ExprResolver::resolveIf(const ast::Expr& expr, const ast::IfExpr& branch, TypePtr target, bool used, bool implicit) {
    BindingScope scope(*this);
    ModulePtr<Block> elseBlock = nullptr;

    // The condition leaves `current` at the block where it held, which is where an `is` test's
    // bindings are live - so the `then` arm is resolved with them in scope and the resize below
    // takes them away again, exactly as the arms of a `match` scope what their patterns bind.
    if(resolveCondition(branch.cond, elseBlock) == PatternResult::Never) return nullptr;

    BranchArmList arms;

    auto thenValue = resolve(branch.then, target, used, implicit);
    if(current) arms.push(BranchArm { current, thenValue, branch.then.source });
    scope.restore();

    current = elseBlock;
    ModulePtr<Value> elseValue = nullptr;
    auto elseSource = expr.source;

    if(branch.otherwise) {
        elseValue = resolve(branch.otherwise.unwrap(), target, used, implicit);
        elseSource = branch.otherwise.unwrap().source;
    } else if(used) {
        context.diagnostics.error("value-producing if requires an else branch"_v, expr.source);
    }

    if(current) arms.push(BranchArm { current, elseValue, elseSource });
    scope.restore();

    return finishBranches(arms, expr.source, used);
}

ModulePtr<Value> ExprResolver::resolveMultiIf(const ast::Expr& expr, ast::ParseList<ast::IfCase> cases, TypePtr target, bool used, bool implicit) {
    auto contents = cases.contents(parse);
    if(contents.size() == 0) return nullptr;

    BindingScope scope(*this);
    BranchArmList arms;
    auto hasElse = false;

    for(Size i = 0; i < contents.size() && current; i++) {
        // The parser writes a trailing `_`/`else` case as a `True` literal condition, so an
        // always-taken final case is recognized here rather than being tested at runtime.
        auto isElse = i + 1 == contents.size() &&
                      ast::isLiteral(contents[i].cond) &&
                      ast::Literal::Kind(contents[i].cond.kind - ast::Expr::Lit) == ast::Literal::Bool &&
                      contents[i].cond.lit.b;

        ModulePtr<Block> nextBlock = nullptr;

        if(isElse) {
            hasElse = true;
        } else if(resolveCondition(contents[i].cond, nextBlock) == PatternResult::Never) {
            return nullptr;
        }

        auto value = resolve(contents[i].then, target, used, implicit);
        if(current) arms.push(BranchArm { current, value, contents[i].then.source });
        scope.restore();

        current = nextBlock;
    }

    // Without an else case, control can fall out of the last test having produced nothing.
    if(current) {
        if(used) context.diagnostics.error("value-producing multi-if requires an else case"_v, expr.source);
        arms.push(BranchArm { current, nullptr, expr.source });
    }

    return finishBranches(arms, expr.source, used && hasElse);
}

void ExprResolver::resolveWhile(const ast::WhileExpr& loop) {
    auto conditionBlock = addBlock();

    // The exit block is made here rather than left to the condition, because `break` targets it
    // and the body is resolved before anything else refers to it.
    auto exitBlock = addBlock();

    terminate(emit<InstJmp>(loop.cond.source, StringId(), module.scalar.unit, conditionBlock));

    // A name the body binds belongs to the body, the way it does in the arms of an `if` or a
    // `match`. Letting one outlive the loop would also let it be read from the exit block, which
    // the value it was bound to does not dominate - the loop may have run zero times. The names an
    // `is` condition binds are in the same position and are scoped by the same resize: they are
    // live in the body, which is exactly where the pattern matched.
    BindingScope scope(*this);

    current = conditionBlock;
    if(resolveCondition(loop.cond, exitBlock) == PatternResult::Never) {
        current = exitBlock;
        return;
    }

    {
        LoopScope loopScope(*this, LoopTarget { conditionBlock, exitBlock });
        resolve(loop.body, nullptr, false);
    }

    scope.restore();

    if(current) terminate(emit<InstJmp>(loop.body.source, StringId(), module.scalar.unit, conditionBlock));
    current = exitBlock;
}

/*
 * `for pat in a .. b [step s]: body`, and its `..=` and `downto` spellings - Design.md's
 * Expressions.
 *
 * A counted loop, and nothing to do with the iterator form beside it: no continuation is lifted and
 * nothing is handed over. What the three spellings decide is one interval and one direction:
 *
 *  - `a .. b`  walks `[a, b)` upward   - the half-open interval, which is the one that composes;
 *  - `a ..= b` walks `[a, b]` upward   - for a bound that is a real member, `0 ..= 255` on a `U8`;
 *  - `a downto b` walks `[b, a)` *downward*, so `n downto 0` is `0 .. n` reversed exactly.
 *
 * `downto` excluding the bound written first is the one surprising part, and it is what makes
 * reversing a loop a one-token edit rather than an arithmetic one. The alternative - both ends
 * inclusive, as in Pascal - makes the reversal of `0 .. n` read `n - 1 downto 0`, and on an unsigned
 * counter with `n == 0` that subtraction wraps to the top of the type and the loop runs forever.
 * The rule to remember is that an interval is always `[low, high)` and the two forms differ only in
 * which end they start from.
 *
 * Every test below is written so that no bound can overflow, which is the whole reason the loop is
 * built here rather than desugared into source. The distance to the far end is what decides whether
 * to step again - `to - i` and `i - to` are computed on the side of the comparison that has already
 * been proved non-negative - so a counter that ends at the top of its type stops rather than
 * wrapping past it.
 */
void ExprResolver::resolveCountedFor(const ast::Expr& expr, const ast::ForExpr& loop) {
    auto source = expr.source;
    auto ascending = !loop.reverse;

    /*
     * The counter's type, decided by the two bounds together.
     *
     * Both are resolved without a target, because neither is more authoritative than the other:
     * `for i in 0 .. xs.length` has the literal take the length's type, and `for i in first .. 10`
     * has it the other way round. Two literals settle to their own default, which is what an
     * ordinary `let` of one would do.
     */
    auto fromValue = resolve(loop.from, nullptr);
    auto toValue = resolve(*parse[loop.to], nullptr);
    if(!fromValue || !toValue) return;

    auto fromLiteral = isLiteral(global, valueType(fromValue));
    auto toLiteral = isLiteral(global, valueType(toValue));

    if(fromLiteral && !toLiteral) {
        fromValue = convert(fromValue, valueType(toValue), loop.from.source);
    } else if(toLiteral && !fromLiteral) {
        toValue = convert(toValue, valueType(fromValue), parse[loop.to]->source);
    } else {
        fromValue = settle(fromValue, loop.from.source);
        toValue = convert(settle(toValue, parse[loop.to]->source), valueType(fromValue),
                          parse[loop.to]->source);
    }

    if(!fromValue || !toValue) return;
    auto counterType = valueType(fromValue);

    // The step, at the counter's type. A step of zero would never reach the far end, and a written
    // one is worth rejecting where it can be seen rather than leaving as a loop that does not stop.
    if(loop.step && ast::isLiteral(*parse[loop.step]) && parse[loop.step]->lit.i() == 0) {
        context.diagnostics.error("a `for` step of zero never reaches the end of its range"_v,
                                  parse[loop.step]->source);
        return;
    }

    auto stepValue = loop.step ? resolve(*parse[loop.step], counterType) : makeInt(source, counterType, 1);
    if(!stepValue) return;

    stepValue = convert(stepValue, counterType, source);

    /*
     * The blocks, created in the order the block list has to hold them.
     *
     * That order is the whole of why this is built by hand: `resolve/lower.cpp` walks blocks in list
     * order and requires every operand to have been lowered already, and `compiler/opt`'s inliner
     * splices and re-lays lists assuming the same. So the loop is laid out
     *
     *     [guards] condition  body...  advance  step  exit
     *
     * which is a reverse postorder: every edge runs forward down that list except the one back edge
     * from the step to the condition. The condition is the loop header and the only way into the
     * cycle, which is also what keeps the loop reducible for the passes that read dominance.
     *
     * The cost is that `exit` does not exist while the body is being resolved, so a `break` cannot
     * jump to it and neither can a guard that decides the loop runs no times. Both are collected and
     * terminated at the end instead - see LoopTarget, and finishContinuationExits for the same
     * pattern applied to a `return` inside a lifted continuation.
     */
    struct PendingBranch {
        ModulePtr<Block> block;
        ModulePtr<Value> condition;
        ModulePtr<Block> taken;
    };

    Array<PendingBranch> pending;
    Array<ModulePtr<Block>> breaks;
    Array<ModulePtr<Block>> continues;

    ModulePtr<Block> ordered = nullptr;
    ModulePtr<Block> reachable = nullptr;

    if(!ascending) {
        ordered = addBlock();
        reachable = addBlock();
    }

    auto conditionBlock = addBlock();
    auto bodyBlock = addBlock();

    /*
     * A descending loop needs what an ascending one gets from its own condition: that the interval
     * is non-empty, and that it holds at least one step. Both are checked before the counter is
     * built, because the counter starts one step below the bound written first and that subtraction
     * has to be known not to wrap.
     */
    auto initial = fromValue;
    if(!ascending) {
        ResolvedArg above[] = { fromValue, toValue };
        auto isAbove = emitCall(Context::nameHash(">", 1), { above, 2 }, source, module.scalar.bool_);
        if(!isAbove) return;

        pending.push(PendingBranch { current, convert(isAbove, module.scalar.bool_, source), ordered });

        current = ordered;
        ResolvedArg span[] = { fromValue, toValue };
        auto distance = emitCall(Context::nameHash("-", 1), { span, 2 }, source, counterType);
        if(!distance) return;

        ResolvedArg fits[] = { distance, stepValue };
        auto hasStep = emitCall(Context::nameHash(">=", 2), { fits, 2 }, source, module.scalar.bool_);
        if(!hasStep) return;

        pending.push(PendingBranch { current, convert(hasStep, module.scalar.bool_, source), reachable });

        current = reachable;

        ResolvedArg back[] = { fromValue, stepValue };
        initial = emitCall(Context::nameHash("-", 1), { back, 2 }, source, counterType);
        if(!initial) return;
    }

    auto counter = allocate(counterType, source, StringId(), ast::BindType::Ref);
    initialize(placeFor(counter, source), convert(initial, counterType, source), source);
    terminate(emit<InstJmp>(source, StringId(), module.scalar.unit, conditionBlock));

    /*
     * The test that says whether this iteration runs at all.
     *
     * Ascending, it is the interval's own upper bound and is what makes an empty range run zero
     * times. Descending, the guards above already proved it, and it is emitted anyway so that the
     * cycle has one header rather than being entered at its body.
     */
    current = conditionBlock;
    auto value = load(placeFor(counter, source), source);

    auto compare = !ascending ? Context::nameHash(">=", 2)
                 : loop.inclusive ? Context::nameHash("<=", 2)
                 : Context::nameHash("<", 1);

    ResolvedArg bound[] = { value, toValue };
    auto more = emitCall(compare, { bound, 2 }, source, module.scalar.bool_);
    if(!more) return;

    pending.push(PendingBranch { conditionBlock, convert(more, module.scalar.bool_, source), bodyBlock });

    // The body, with the counter bound. Scoped to the body the way a `while`'s bindings are: the
    // loop may run zero times, so the name does not dominate the code after it.
    BindingScope scope(*this);

    current = bodyBlock;
    bindIrrefutable(loop.pat, value,
                    "a `for` loop has no alternative to take for an element it does not match"_v);

    {
        LoopScope loopScope(*this, LoopTarget { nullptr, nullptr, &continues, &breaks });
        resolve(loop.body, nullptr, false);
    }

    scope.restore();

    auto tail = current;

    auto advanceBlock = addBlock();
    auto stepBlock = addBlock();
    auto exitBlock = addBlock();

    /*
     * The step, guarded by how far the counter still has to go.
     *
     * `to - i` ascending and `i - to` descending, each on the side the condition above has already
     * proved is the larger - so neither subtraction wraps, and comparing the distance against the
     * step is what stops a counter whose next value would leave the type rather than the range.
     * A closed range stops when the distance is *below* a step, a half-open one when it is at most
     * one, which is the whole of the difference between `..` and `..=` in the emitted code.
     */
    current = advanceBlock;
    auto atStep = load(placeFor(counter, source), source);

    ResolvedArg remaining[] = { ascending ? toValue : atStep, ascending ? atStep : toValue };
    auto distance = emitCall(Context::nameHash("-", 1), { remaining, 2 }, source, counterType);
    if(!distance) return;

    auto exhausted = (ascending && !loop.inclusive) ? Context::nameHash("<=", 2)
                                                    : Context::nameHash("<", 1);

    ResolvedArg left[] = { distance, stepValue };
    auto done = emitCall(exhausted, { left, 2 }, source, module.scalar.bool_);
    if(!done) return;

    terminate(emit<InstJe>(source, StringId(), module.scalar.unit, convert(done, module.scalar.bool_, source),
                           exitBlock, stepBlock));

    current = stepBlock;
    ResolvedArg moved[] = { atStep, stepValue };
    auto next = emitCall(ascending ? Context::nameHash("+", 1) : Context::nameHash("-", 1),
                         { moved, 2 }, source, counterType);
    if(!next) return;

    assign(placeFor(counter, source), convert(next, counterType, source), source);
    terminate(emit<InstJmp>(source, StringId(), module.scalar.unit, conditionBlock));

    // Everything that was waiting for a block that did not exist yet. Each branch falls through to
    // the exit when its condition does not hold, which is one shape for the two guards and the
    // loop's own test alike.
    for(auto& branch: pending) {
        current = branch.block;
        terminate(emit<InstJe>(source, StringId(), module.scalar.unit, branch.condition, branch.taken, exitBlock));
    }

    for(auto block: continues) {
        current = block;
        terminate(emit<InstJmp>(source, StringId(), module.scalar.unit, advanceBlock));
    }

    for(auto block: breaks) {
        current = block;
        terminate(emit<InstJmp>(source, StringId(), module.scalar.unit, exitBlock));
    }

    if(tail) {
        current = tail;
        terminate(emit<InstJmp>(loop.body.source, StringId(), module.scalar.unit, advanceBlock));
    }

    current = exitBlock;
}

void ExprResolver::resolveReturn(const ast::Expr& expr) {
    if(inThunk) {
        // Returning from the enclosing function out of a `@lazy` argument is a non-local exit
        // across the callee's live frame, which is Analysis-Lens.md §5.1's exit signal - the
        // callee has cleanup that would have to run on the way past. Rejected rather than left to
        // mean "return from the thunk", which is what it would otherwise silently become.
        context.diagnostics.error("`return` inside a `@lazy` argument is not available yet - it would have to leave the function through the callee's frame, which needs the exit signal"_v,
                                  expr.source);
        return;
    }

    if(function.funKind == ast::FunKind::Iter) {
        // An iterator ends by running out of values, and what it hands back then is the step signal
        // rather than anything the body has a name for. A `return` in it would have to produce that
        // signal, which is not a type the declaration wrote or the author could.
        context.diagnostics.error("an `iter fn` ends by running out of values rather than by `return` - what it produces is the loop's own signal, which is not something the body names"_v,
                                  expr.source);
        return;
    }

    if(resultInferred) {
        // Nothing has decided what this lambda returns yet, and `return` cannot be the thing that
        // decides it: a later `return` of a different type would have nothing to be checked
        // against, and the two would silently disagree.
        context.diagnostics.error("this lambda's result type is decided by its body, so it cannot use `return` - write it where a function type is expected"_v,
                                  expr.source);
    }

    /*
     * The function this `return` leaves, which is not always the one it is written in.
     *
     * Inside a lens continuation the block was split out of some enclosing function, and Design.md's
     * Leaving through a lens says a `return` there leaves *that* function - past the lens's own
     * frame, which runs its cleanup on the way. So the type it is checked against is the enclosing
     * one's, and what the departure compiles to is decided later - see emitFunctionReturn.
     */
    auto declared = enclosingResultType();

    ModulePtr<Value> value = nullptr;
    if(expr.ret) value = resolve(*parse[expr.ret], declared);

    if(isUnit(global, declared)) {
        if(value) context.diagnostics.error("unit function cannot return a value"_v, expr.source);
        value = nullptr;
    } else if(!value) {
        context.diagnostics.error("non-unit function must return a value"_v, expr.source);
    } else {
        value = convert(value, declared, expr.source);
    }

    emitFunctionReturn(value, expr.source);
}
