#include "resolve_internal.h"

namespace mir {

void Resolver::resolveStatement(Builder& builder, ast::Expr& expr) {
    switch(expr.kind) {
        case ast::Expr::Multi:
            resolveBlock(builder, expr, nullptr, false, inspectRequest());
            return;
        case ast::Expr::Nested:
            resolveStatement(builder, *parse[expr.nested]);
            return;
        case ast::Expr::Decl:
            resolveDeclaration(builder, expr);
            return;
        case ast::Expr::Assign:
            resolveAssign(builder, expr);
            return;
        case ast::Expr::If:
            resolveIf(builder, expr, nullptr, false, inspectRequest());
            return;
        case ast::Expr::While:
            resolveWhile(builder, expr);
            return;
        case ast::Expr::Ret:
            resolveReturn(builder, expr);
            return;
        case ast::Expr::Yield:
            resolveYield(builder, expr);
            return;
        case ast::Expr::Break:
        case ast::Expr::Continue:
            /*
             * The other two exits out of a continuation - Analysis-Lens.md §5.1.
             *
             * Refused by name rather than left to the expression walk, because they are not an
             * unknown construct: they are the loop-shaped instance of the mechanism `return` above
             * is the function-shaped one, which is §5.1's argument that they are not iterator
             * machinery. What they wait for is the loop side of it - `iter fn` and the `for` that
             * consumes one - rather than anything a lens needs.
             */
            unsupported(expr.source, expr.kind == ast::Expr::Break ? "`break`"_v : "`continue`"_v);
            builder.addUnreachable(expr.source);
            return;
        case ast::Expr::Dispose:
            resolveDispose(builder, expr);
            return;
        default: {
            /*
             * An expression in statement position: resolved, and its value discarded.
             *
             * Discarding an *owner* is where the teardown is owed, and the rule is the one `hold`
             * states: an owner lives in a place. Writing it into a temporary is what gives the drop
             * something to name, and the drop itself is placed by the ownership analysis on exactly
             * the terms every other local gets - which is why there is no "discard" concept here.
             */
            auto value = resolveValue(builder, expr, nullptr, inspectRequest());
            if(!value || builder.isTerminated()) return;

            if(valueOccurrence(*value).contract.isOwned() &&
               hasTeardown(decl(), valueType(*value))) {
                hold(builder, *value, expr.source);
            }

            return;
        }
    }
}

/*
 * `->x` - `doc/spec/ownership.md`'s explicit disposal.
 *
 * Three lines, and that is the argument for the design rather than an accident of it: a disposal is
 * an owned read of a place followed by nothing, and "followed by nothing" is a state the drop
 * placement already has a rule for. So there is no disposal in the IR, no disposal in the analysis
 * and no disposal in the backends - the temporary this writes into is dead from the next position
 * on, and the teardown lands there because that is where the value stops being live.
 *
 * The three consequences the spec lists all come from that shape rather than from separate rules:
 *
 *  - **It runs whatever the teardown is.** `hold` names the value and the placement asks the type,
 *    so an authored `Drop`, a derived walk over the members and a type with nothing to run are the
 *    same three cases they are everywhere else. A `drop(x)` spelling could not do this: `drop` is a
 *    class member, and a type whose teardown is derived has no instance to dispatch to.
 *  - **It keeps a value alive for longer.** The read is a use, so liveness carries the value to
 *    here. That is the only way to write "not before this point" in a language whose lifetimes end
 *    at last use, and it costs nothing to provide because it is what a use already means.
 *  - **It refuses a borrow.** `bindingRequest(Access::Owned)` is the same request `let ->x = e`
 *    makes, so `readPlace` reports storage this function does not own in the words it already has.
 *
 * Answers whether the disposal was resolved, for the value position in `resolveValue`: a disposal is
 * an expression of type `{}` there, and a unit produced after a diagnostic would carry the failure
 * on into whatever asked.
 */
bool Resolver::resolveDispose(Builder& builder, ast::Expr& expr) {
    auto value = resolveValue(builder, *parse[expr.dispose], nullptr, bindingRequest(Access::Owned));
    if(!value || builder.isTerminated()) return false;

    auto& occurrence = valueOccurrence(*value);

    /*
     * A borrow that `readPlace` had nothing to say about - a produced value that is not an owner.
     *
     * `readPlace` reports the *named* case, since a place is what it is given. What reaches here
     * instead is a call whose result borrows, and refusing it by name is worth one line: the value
     * is somebody else's and there is nothing to hand over, so a disposal of it would be a teardown
     * of the lender's object at a point the lender did not choose.
     */
    if(!occurrence.contract.isOwned()) {
        context.diagnostics.error("this borrows rather than owning, so there is nothing to dispose of"_v,
                                  expr.source);
        return false;
    }

    /*
     * A trivial value has nothing to dispose of, and saying so is better than accepting it.
     *
     * `readMode` answered `CopyTrivial` for it - the request asked for an owner and the type
     * duplicates freely, so `x` is still there afterwards - which makes `->x` a statement that reads
     * like a consumption and consumes nothing. The two useful readings of a disposal both need a
     * value that can actually end: an explicit release point, and a lifetime extended to here.
     */
    if(isTriviallyCopyable(decl(), occurrence.type)) {
        context.diagnostics.error(
            "%@ is copied rather than handed over, so there is nothing here to dispose of"_v,
            expr.source, typeText(context, decl(), occurrence.type));
        return false;
    }

    // The temporary's own local, marked so that the drop placement can tell the disposal the source
    // wrote from a value that merely stopped being used - see `Local::disposal`.
    auto place = hold(builder, *value, expr.source);
    builder.body.local(builder.base(), LocalId(place->payload))->disposal = true;
    return true;
}

/*
 * `let x = e`, `let &x = e` and `let ->x = e` - `doc/spec/ownership.md`'s two axes of a binding.
 *
 * Storage and an initialization, not a binding to a computation. That is what makes the
 * representation place-based: `x` names a place from here on, a later `x = e` is an assignment to
 * the same place, and both of those are things the ownership rules have opinions about. A `let` that
 * named a node instead would have to rediscover the storage the first time anything was written
 * through it.
 *
 * **What the binding holds is not decided here.** The spec's rule is that an initializer either
 * *produces* storage - a call result, a construction, a literal - or *names* storage that already
 * exists, and the binding owns the first and is a loan of the second at the capability the sigil
 * asked for. Both halves are `readMode`'s answer, because "does this name a place" is exactly the
 * question a read is: a produced value never went through one, so it arrives owned whatever the
 * sigil said, and a named one arrives as the copy, loan or move the pair decided. So the sigil
 * reaches this function as a `Request` and nothing else, and the local's occurrence is whatever the
 * initializer turned out to be.
 *
 * `->` is therefore not redundant on a produced value and not a second mechanism on a named one: it
 * is `Access::Owned` where the other two are `Shared` and `Unique`, and `readMode` answers all
 * three.
 */
void Resolver::resolveDeclaration(Builder& builder, ast::Expr& expr) {
    for(auto binding: expr.decl.contents(parse)) {
        /*
         * A name, or `_`.
         *
         * `_` is what the no-binding scoped form desugars to - `f(...):` is `let _ = f(...) in: ...`,
         * see `Parser::parseStatement` - and is a spelling in its own right for a value the source
         * evaluates and does not want. Everything else is a destructuring pattern, which needs the
         * match machinery rather than a local.
         */
        if(binding.pat.kind != ast::Pat::Var && binding.pat.kind != ast::Pat::Any) {
            unsupported(binding.pat.source, "destructuring a declaration"_v);
            continue;
        }

        /*
         * `| else ->` - the alternatives a *skipping* lens call needs, and nothing else writes.
         *
         * Milestone 6's, and named as such: the ability to skip is exactly the presence of a `Try`
         * wrapper on the callee's result (Analysis-Lens.md §7.1), which is instance selection over a
         * class that does not exist yet.
         */
        if(binding.alts.size()) {
            unsupported(binding.pat.source, "a declaration with alternatives"_v);
            continue;
        }

        // Before the `in` case, which reads the initializer: the grammar puts `in` inside the `=`
        // branch, so the two cannot come apart - and a check that relies on a production's shape is
        // one that stops holding when the production changes.
        if(!binding.content) {
            context.diagnostics.error("a local requires an initializer"_v, binding.pat.source);
            continue;
        }

        if(binding.in) {
            resolveScoped(builder, binding);
            continue;
        }

        auto writable = binding.bind == ast::BindType::Ref;
        auto access = binding.bind == ast::BindType::Sink ? Access::Owned
                    : writable                            ? Access::Unique
                                                          : Access::Shared;

        auto value = resolveValue(builder, *parse[binding.content], nullptr, bindingRequest(access));
        if(!value) return;

        /*
         * The binding's occurrence is the initializer's, whole.
         *
         * Not `owned(type)`: a loan bound to a name is a loan, and a local that recorded the type
         * and dropped the contract would be storage the ownership analysis believes this frame owns
         * - which is a teardown of the lender's value at the end of this one.
         */
        auto local = builder.addLocal(binding.pat.var, valueOccurrence(*value),
                                      binding.pat.source, writable);

        auto place = builder.localPlace(*local);
        builder.addWrite(binding.pat.source, *place, *value, Destination::Init);

        // In scope from *after* its own initializer, which is what makes `let x = x` read the outer
        // `x` rather than itself.
        bindings.push(Binding { binding.pat.var, local });
    }
}

/*
 * `let x = e in body` - a binding whose scope is written, and the one place a lens is called.
 *
 * Two forms of one statement, and Analysis-Lens.md §11.2 is why they are one: a lens's extent is
 * settled at the *call site*, because neither the signature nor the body can say how much of the
 * caller is meant to be inside it, and `in` is the spelling that already parses. Whether this is a
 * scoped `let` or a lens call is therefore the callee's declaration and nothing about the call.
 */
void Resolver::resolveScoped(Builder& builder, const ast::VarDecl& binding) {
    auto& initializer = *parse[binding.content];

    /*
     * A call of a `lens fn`, which is what makes the `in` body a continuation.
     *
     * Only a directly named one. A lens reached through a class member or a binding of lens type is
     * the erased callback ABI, which is milestone 6's; what is refused there is the *call*, in
     * `resolveCall`, so a name that turns out not to be a lens falls through to the ordinary scoped
     * binding below rather than being reported twice.
     */
    if(initializer.kind == ast::Expr::App) {
        auto& application = *parse[initializer.app];

        if(application.callee.kind == ast::Expr::Var && !findLocal(application.callee.var)) {
            if(auto found = program.findFunction(here(), application.callee.var)) {
                auto& callee = *decl()[found];
                if(callee.lens) {
                    resolveLensCall(builder, binding, initializer, callee);
                    return;
                }
            }
        }
    }

    /*
     * The ordinary scoped binding: the same `let` as any other, with its name removed afterwards.
     *
     * Deliberately not a block. A `let ... in` introduces one name over one statement and nothing
     * else - it opens no scope for the `in` body's own declarations to be trapped in, because the
     * body is resolved as a statement and a statement that declares does so in the enclosing scope.
     */
    auto writable = binding.bind == ast::BindType::Ref;
    auto access = binding.bind == ast::BindType::Sink ? Access::Owned
                : writable                            ? Access::Unique
                                                      : Access::Shared;

    auto value = resolveValue(builder, initializer, nullptr, bindingRequest(access));
    if(!value) return;

    // Storage even for `_`, because an owner lives in a place: the value is evaluated and its
    // teardown is owed, and a local is what gives the drop something to name. What `_` skips is the
    // *name*, which is the whole of what it says.
    auto named = binding.pat.kind == ast::Pat::Var;
    auto local = builder.addLocal(named ? binding.pat.var : StringId(), valueOccurrence(*value),
                                  binding.pat.source, writable);

    auto place = builder.localPlace(*local);
    builder.addWrite(binding.pat.source, *place, *value, Destination::Init);

    auto scope = bindings.size();
    if(named) bindings.push(Binding { binding.pat.var, local });
    resolveStatement(builder, *parse[binding.in]);
    bindings.resize(U32(scope));
}

/*
 * A lens call, which is not a call - Analysis-IR.md §10.1.
 *
 * The callee's body is resolved **into** this body, and everything the construct needs follows from
 * that one decision. The four exits out of the continuation are ordinary CFG edges, so liveness, the
 * loan extents and drop elaboration walk them without knowing what a lens is. The lens's frame
 * storage and the caller's are one frame with one set of places, so a borrow handed over is checked
 * against the storage it is really rooted in rather than against a summary of it. And the two scopes
 * - the callee's names and the caller's - are a floor on one binding array rather than two resolvers.
 *
 * What flattening costs is recorded rather than hidden: a lens body is resolved once per call site,
 * so a diagnostic inside one is reported once per call site, and a lens nothing calls is never
 * checked at all. Both go away at the opaque boundary, which is what gives a lens a body of its own.
 *
 * The shape emitted is:
 *
 *      ...arguments...              in the caller's region
 *      jump bb(frame entry)
 *    frame:  ...the callee, up to its `yield`...
 *      yield %v -> bb(continuation entry)
 *    continuation:  ...the `in` body...
 *      jump bb(resume)
 *    frame:  ...the callee, after its `yield`...
 *      jump bb(join)
 *    join:  the caller, again
 */
bool Resolver::resolveLensCall(Builder& builder, const ast::VarDecl& binding, ast::Expr& call,
                               Function& callee) {
    auto& written = *parse[call.app];
    auto source = call.source;

    /*
     * `let &x = f(...)` and `let ->x = f(...)`.
     *
     * Refused, and the message says where the convention does live: what the continuation receives
     * is the *declaration's* `-> &T` or `-> ->T` (Analysis-Language.md §3a's body-independence rule,
     * which the parser records as `retBind`), so a sigil here would be a second statement of one
     * fact and the two could disagree.
     */
    if(binding.bind != ast::BindType::Borrow) {
        context.diagnostics.error(
            "what a lens hands over is declared on %@ with `-> &T` or `-> ->T`, not written here"_v,
            binding.pat.source, context.findName(callee.name));
        return false;
    }

    ast::Expr* body = nullptr;
    for(auto& entry: lensBodies) {
        if(entry.function == &callee - *program.decl) body = entry.body;
    }

    if(!body) {
        unsupported(source, "calling a lens with no body"_v);
        return false;
    }

    /*
     * A lens inside itself, directly or through another.
     *
     * Flattening is a substitution, so a cycle in it does not terminate - and it is not a limitation
     * of the flattening: a lens's continuation runs inside its own frame, so a recursive one has no
     * frame to return to, which is the same reason Analysis-External-Iterators.md §3 bounds a child
     * stack. Refused here, where the cycle is visible, rather than by running out of stack.
     */
    for(U32 i = 0; i < lensFloor; i++) {
        if(lensFrames[i].callee != &callee) continue;

        context.diagnostics.error("%@ is being called inside itself, and a lens frame has no way to nest in its own"_v,
                                  source, context.findName(callee.name));
        return false;
    }

    if(!checkCallShape(written, callee, source)) return false;

    // In the caller's scope, before the frame is opened: the arguments are the caller's expressions
    // and the callee's names are not in scope for them.
    SmallArray<Node*, 8> arguments;
    if(!resolveArguments(builder, written, callee, arguments)) return false;

    /*
     * The parameters, as ordinary locals - **in the caller's block**, before the frame is entered.
     *
     * The storage is the frame's and the initialization is the call's, which is the same split an
     * ordinary call has: the arguments are evaluated where they are written and the callee starts
     * with them in hand. It is also the only place they can go, since a value operand names a node
     * of its own block (see `checkValueScope`) and the argument nodes are the caller's.
     *
     * The occurrence is the *argument's* rather than the declaration's, which is the same rule
     * `resolveDeclaration` follows and is what flattening buys: a borrow the caller granted is
     * rooted in the caller's storage, and recording the declaration's call-bounded contract instead
     * would throw away the one fact the loan checker can now see.
     */
    SmallArray<Local*, 8> parameters;

    // The region is opened *before* the parameters are made, so that their storage belongs to the
    // frame it is - see `Local::region`. The current block is unchanged by it, which is what keeps
    // the initializations below in the caller's block where their values are.
    auto frame = builder.enterRegion(Continuation::Frame, Continuation::Once);

    for(U32 i = 0; i < arguments.size(); i++) {
        auto& declared = *decl()[callee.args.get(decl(), i)];
        auto local = builder.addLocal(declared.name, valueOccurrence(*arguments[i]), declared.source,
                                      declared.occurrence.contract.access() == Access::Unique);

        auto place = builder.localPlace(*local);
        builder.addWrite(declared.source, *place, *arguments[i], Destination::Init);
        parameters.push(local);
    }

    auto entry = builder.addBlock(StringId());
    builder.addJump(source, *entry, nullptr, 0);
    builder.setBlock(*entry);

    LensFrame opened;
    opened.callee = &callee;
    opened.region = frame;
    opened.entry = entry->index;
    opened.declaration = &binding;
    opened.floor = U32(bindings.size());
    opened.callerFloor = bindingFloor;
    opened.callerLensFloor = lensFloor;
    opened.tailFloor = U32(tails.size());
    opened.source = source;

    auto depth = U32(lensFrames.size());
    lensFrames.push(opened);
    bindingFloor = opened.floor;
    lensFloor = depth + 1;

    // The names, once the floor is in place: they are the callee's, and the caller's are hidden
    // from here to the `yield`.
    for(auto local: parameters) bindings.push(Binding { local->name, local });

    resolveStatement(builder, *body);

    auto exit = builder.isTerminated() ? nullptr : builder.currentBlock();

    bindings.resize(lensFrames[depth].floor);
    bindingFloor = lensFrames[depth].callerFloor;
    lensFloor = lensFrames[depth].callerLensFloor;

    // Out of the frame first, so that the join is a block of the *caller's* region - which is what
    // makes the frame's last edge a rejoin rather than an edge inside it.
    builder.leaveRegion();
    auto join = builder.addBlock(StringId());
    builder.rejoinRegion(frame, *join);

    auto handedOver = checkFrameYield(builder, lensFrames[depth], exit);
    lensFrames.resize(depth);

    /*
     * Where the caller carries on - or does not.
     *
     * A frame with no exit is one whose continuation left on every path: the cleanup ran on those
     * exits and the join is entered from nothing. Closing it is what tells the *enclosing* walk that
     * control left, which matters as soon as lenses nest - a lens call whose join stayed open would
     * make the frame around it think its own continuation fell out, and it would resume at a cleanup
     * that has already run.
     */
    if(exit) builder.addJump(source, *join, nullptr, 0);

    builder.setBlock(*join);
    if(!exit) builder.addUnreachable(source);

    return handedOver;
}

/*
 * `yield` - the callee side of a lens call, and where the continuation is resolved.
 *
 * The continuation is resolved *here*, in the middle of the callee's body, and that ordering is the
 * construct rather than an implementation choice: what follows a `yield` is the cleanup, it runs
 * after the caller's block, and emitting the three in the order they run is what makes them edges.
 */
void Resolver::resolveYield(Builder& builder, ast::Expr& expr) {
    if(!lensFloor) {
        context.diagnostics.error(
            "`yield` hands a value to a lens's continuation, and this is not inside a `lens fn`"_v,
            expr.source);
        return;
    }

    // Copied out, not held by reference: resolving the continuation below may open lens frames of
    // its own, and the array that holds them may move.
    auto frame = lensFrames[lensFloor - 1];

    if(frame.stage == LensFrame::Unwinding) {
        context.diagnostics.error("a path through %@ hands over twice, and one frame runs one continuation"_v,
                                  expr.source, context.findName(frame.callee->name));
        return;
    }
    auto& handover = frame.callee->result;
    auto wanted = requestOf(handover.contract);

    Node* value = nullptr;

    if(expr.yield) {
        value = resolveValue(builder, *parse[expr.yield], handover.type, wanted);
        if(!value) return;
        if(!checkType(*value, handover.type, expr.source, "a handed-over value"_v)) return;

        // The same rule an argument filling a borrowed parameter follows, for the same reason: the
        // continuation does not take the value, so this frame still owes its teardown and a value
        // that is only a node result has no place for one to name. See `resolveArguments`.
        if(wanted.access != Access::Owned && valueOccurrence(*value).contract.isOwned() &&
           hasTeardown(decl(), handover.type)) {
            auto place = hold(builder, *value, expr.source);
            value = readPlace(builder, expr.source, *place, wanted);
            if(!value) return;
        }
    } else if(decl()[handover.type]->kind == Type::Unit) {
        value = builder.addConst(expr.source, unitConstant(program.types.unit));
    } else {
        context.diagnostics.error("%@ hands over %@, so this `yield` needs a value"_v, expr.source,
                                  context.findName(frame.callee->name),
                                  typeText(context, decl(), handover.type));
        return;
    }

    /*
     * The continuation region, and its entry parameter.
     *
     * The parameter's occurrence is what actually arrived rather than what the declaration asked
     * for, on the same terms `branchValue` gives a merge the arm's whole occurrence: a value handed
     * over by a flattened lens is rooted in real storage, and the declared contract is the summary
     * of it that an opaque boundary would need. The declaration is kept on the region, where it says
     * something the CFG does not.
     */
    auto region = builder.enterRegion(Continuation::Handover, Continuation::Once);
    auto entry = builder.addBlock(StringId());
    auto parameter = builder.addBlockArgument(*entry, valueOccurrence(*value), expr.source);

    builder.body.region(builder.base(), region)->handover = handover;

    auto argument = builder.resultOf(*value, argumentMode(valueOccurrence(*parameter).contract,
                                                          SourceKind::Result));
    builder.addYield(expr.source, region, *entry, &argument, 1);
    builder.setBlock(*entry);

    /*
     * The caller's names back, and the callee's aside.
     *
     * A swap rather than a push: the two scopes are siblings, and neither the callee's locals nor
     * the caller's are visible from the other. What the continuation adds on top is its own - the
     * name the `let` bound, and whatever its body declares - and all of that goes away again below.
     */
    SmallArray<Binding, 8> hidden;
    for(U32 i = frame.floor; i < bindings.size(); i++) hidden.push(bindings[i]);

    /*
     * The same names again, where an exit out of the continuation can find them - `runCleanup`.
     *
     * `hidden` is this call's, and a `return` written three continuations deep needs the callee
     * scope of every frame it is leaving rather than of the innermost one. So the parked names go on
     * the resolver, keyed by the frame, and the frame records where its own end.
     */
    auto& live = lensFrames[lensFloor - 1];
    live.stage = LensFrame::Continuation;
    live.tailTop = U32(tails.size());
    live.hiddenFirst = U32(hiddenBindings.size());
    live.hiddenCount = U32(hidden.size());
    for(auto& parked: hidden) hiddenBindings.push(parked);

    frame = live;

    bindings.resize(frame.floor);
    bindingFloor = frame.callerFloor;
    lensFloor = frame.callerLensFloor;

    /*
     * The name the `let` bound, holding what arrived.
     *
     * Writable exactly when what arrived is exclusive, which is the rule `addParameter` states about
     * a `&` parameter and is the same question: the declaration said `-> &T`, so the continuation
     * has write access and a binding that refused it would be a capability granted and then thrown
     * away. The sigil is not written at the call site - see `resolveLensCall`, which refuses one -
     * so this is the only place the answer can come from.
     */
    auto& declaration = *frame.declaration;
    auto handed = valueOccurrence(*parameter);
    auto named = declaration.pat.kind == ast::Pat::Var;

    auto local = builder.addLocal(named ? declaration.pat.var : StringId(), handed,
                                  declaration.pat.source,
                                  handed.contract.access() == Access::Unique);

    auto place = builder.localPlace(*local);
    builder.addWrite(declaration.pat.source, *place, *parameter, Destination::Init);
    if(named) bindings.push(Binding { declaration.pat.var, local });

    resolveStatement(builder, *parse[declaration.in]);
    auto left = builder.isTerminated();

    bindings.resize(frame.floor);
    for(auto entry_: hidden) bindings.push(entry_);
    bindingFloor = frame.floor;
    lensFloor = frame.callerLensFloor + 1;
    hiddenBindings.resize(frame.hiddenFirst);

    // Out of the continuation, so nothing below owes this frame's cleanup: the statements after the
    // `yield` *are* it, and an exit written among them leaves past the frames outside this one -
    // see `LensFrame::Stage`.
    lensFrames[frame.callerLensFloor].stage = LensFrame::Outside;

    // The cleanup, in the frame's own region. Made after the continuation closes, which is why
    // `rejoinRegion` is separate from `leaveRegion` - the block a region rejoins at does not exist
    // while the region is open.
    builder.leaveRegion();
    auto resume = builder.addBlock(StringId());
    builder.rejoinRegion(region, *resume);

    if(!left) builder.addJump(expr.source, *resume, nullptr, 0);
    builder.setBlock(*resume);

    /*
     * A continuation no path of which falls out, whose cleanup has therefore already run.
     *
     * Every exit ran its own copy - see `runCleanup` - so what is left here is the resume of a
     * hand-over nothing resumes from. Closing it is what stops the frame's remaining statements from
     * being resolved a second time into a block nothing enters, which would be a second copy of
     * their diagnostics for code that cannot run.
     */
    if(left) builder.addUnreachable(expr.source);
}

/*
 * Every path through a lens's frame hands over exactly once - Analysis-Lens.md §7.1.
 *
 * A forward walk rather than a count of `yield` statements, because both halves of the rule are
 * about paths. A lens that hands over on one arm of an `if` and not the other is a *skipping* lens,
 * whose call site has to say where the skip goes and whose result carries a `Try` - milestone 6's,
 * and silently accepting it here would mean a continuation that sometimes does not run at all. One
 * that hands over twice would run one continuation twice inside one frame, which the frame's own
 * storage has no second copy for.
 *
 * Two bits per block: reached without having handed over, and reached after having. A `yield` of
 * *this* frame is where the first becomes the second, and the walk does not follow it into the
 * continuation - the continuation is the caller's code, and where it comes back to is the region's
 * destination. A yield of a *nested* frame is an ordinary edge here, because that lens's
 * continuation is this lens's own body.
 */
bool Resolver::checkFrameYield(Builder& builder, LensFrame& frame, Block* exit) {
    auto base = builder.base();
    auto& body = builder.body;

    // Whether a region is this frame or something opened inside it. A block outside is one the walk
    // must not follow into: the caller's join, or a loop the continuation left by.
    auto inside = [&](ContinuationId id) {
        for(auto at = id; at.valid(); at = body.region(base, at)->parent) {
            if(at == frame.region) return true;
        }

        return false;
    };

    static constexpr U8 kBefore = 1;
    static constexpr U8 kAfter = 2;

    SmallArray<U8, 32> state;
    for(U32 i = 0; i < body.blocks.size(); i++) state.push(0);

    SmallArray<U32, 16> pending;

    auto mark = [&](BlockId block, U8 bits) {
        if(!block.valid() || !inside(body.block(base, block)->region)) return;
        if((state[block.index] & bits) == bits) return;

        state[block.index] = U8(state[block.index] | bits);
        pending.push(block.index);
    };

    mark(frame.entry, kBefore);

    auto twice = false;

    while(pending.size()) {
        auto index = pending[pending.size() - 1];
        pending.resize(U32(pending.size() - 1));

        auto block = body.block(base, BlockId(index));
        auto bits = state[index];
        auto count = block->nodes.size();
        if(!count) continue;

        auto last = base[block->nodes.get(base, count - 1)];
        if(!isTerminator(last->kind)) continue;

        if(last->kind == Node::Yield) {
            auto opened = body.region(base, ((NodeYield&)*last).region);

            if(opened->parent == frame.region) {
                if(bits & kAfter) twice = true;
                mark(opened->destination, kAfter);
                continue;
            }
        }

        forEachEdge(*last, [&](Edge& edge) { mark(base[edge.target]->index, bits); });
    }

    if(twice) {
        context.diagnostics.error("a path through %@ hands over twice, and one frame runs one continuation"_v,
                                  frame.source, context.findName(frame.callee->name));
        return false;
    }

    /*
     * The frame's exit, which is the one place the answer is read.
     *
     * Null where the frame's own walk left by itself: every path of the continuation exited, so the
     * resume is closed as unreachable and the statements after the `yield` ran on the exits instead.
     * There is no end to reach, and the hand-over the rule is about already happened.
     */
    if(!exit) return true;

    // Reached by neither is a block nothing enters, which `finish` closes as unreachable.
    if(!(state[exit->index.index] & kBefore)) return true;

    context.diagnostics.error("a path through %@ reaches the end without a `yield`, and a lens that may skip its continuation is not implemented"_v,
                              frame.source, context.findName(frame.callee->name));
    return false;
}

/*
 * One more copy of a frame's cleanup, for an exit out of its continuation that is not falling out -
 * Analysis-Lens.md §5.1.
 *
 * A copy rather than a shared block, and that is the decision this waypoint turns on. The cleanup
 * runs on every exit, but the exits do not merge: falling out carries on in the caller and a
 * `return` leaves the function. A single copy would therefore have to end in a branch on *which
 * exit brought us here*, and the returned value would have to survive the cleanup in storage the
 * branch does not explain - initialized on one incoming edge, read on the merge of both. That
 * correlation is invisible to every pass in this stage, which is exactly the class of thing §1.2
 * exists to keep out of the representation, so each exit gets its own copy and every path through it
 * is straight-line to the analyses.
 *
 * What it costs is duplicated code and duplicated diagnostics, on the same terms flattening already
 * pays them, and it is bounded by the exits the continuation actually writes. The opaque boundary is
 * where the other trade is taken: a callee that is not flattened runs one cleanup and propagates an
 * exit code, which is what §5.1's `Outcome` is for and what W7 builds.
 */
void Resolver::runCleanup(Builder& builder, U32 depth) {
    auto frame = lensFrames[depth];

    // The exit's own scope, put aside: the statements below are the callee's and are resolved with
    // the callee's names, and the walk carries on in the caller's afterwards.
    SmallArray<Binding, 16> caller;
    for(U32 i = frame.floor; i < bindings.size(); i++) caller.push(bindings[i]);

    auto savedFloor = bindingFloor;
    auto savedLens = lensFloor;

    bindings.resize(frame.floor);
    for(U32 i = 0; i < frame.hiddenCount; i++) bindings.push(hiddenBindings[frame.hiddenFirst + i]);
    bindingFloor = frame.floor;
    lensFloor = depth + 1;

    /*
     * The statements the frame's walk had not reached, outermost sequence first.
     *
     * Each step is copied out before its statements are resolved, because resolving one pushes steps
     * of its own and the array may move. The walk stops at the first sequence that leaves - a
     * cleanup ending in a `return` of its own is the enclosing function returning, and there is
     * nothing after it to run.
     */
    for(U32 i = frame.tailFloor; i < frame.tailTop && !builder.isTerminated(); i++) {
        auto step = tails[i];

        for(U32 j = step.next; j < step.statements.size() && !builder.isTerminated(); j++) {
            auto statement = step.statements.get(parse, j);
            resolveStatement(builder, statement);
        }
    }

    bindings.resize(frame.floor);
    for(auto& parked: caller) bindings.push(parked);
    bindingFloor = savedFloor;
    lensFloor = savedLens;
}

/*
 * Every frame an exit written here leaves, innermost first.
 *
 * The frames are marked before any of them runs rather than one at a time, and that is what makes a
 * `return` written *inside* one of these cleanup copies correct: it leaves past the frames further
 * out, whose cleanup this path has not entered, and not past these again. They go back to
 * `Continuation` afterwards because the next exit out of the same continuation - the other arm of an
 * `if` - needs its own copy of exactly the same set.
 */
void Resolver::unwindFrames(Builder& builder) {
    SmallArray<U32, 8> leaving;
    for(U32 i = 0; i < lensFrames.size(); i++) {
        if(lensFrames[i].stage == LensFrame::Continuation) leaving.push(i);
    }

    for(auto depth: leaving) lensFrames[depth].stage = LensFrame::Unwinding;
    for(U32 i = U32(leaving.size()); i-- > 0;) runCleanup(builder, leaving[i]);
    for(auto depth: leaving) lensFrames[depth].stage = LensFrame::Continuation;
}

void Resolver::resolveAssign(Builder& builder, ast::Expr& expr) {
    auto& written = *parse[expr.assign];

    auto place = resolvePlace(builder, written.target, true);
    if(!place) {
        if(!context.diagnostics.errorCount()) {
            unsupported(written.target.source, "assigning to this"_v);
        }

        return;
    }

    // Writability is checked here rather than at the builder because this is where the source
    // position is: the builder asserts the same thing, and an assertion is not a diagnostic.
    if(!place->writable) {
        context.diagnostics.error("this is not declared `&`, so it cannot be assigned"_v,
                                  written.target.source);
        return;
    }

    auto value = resolveValue(builder, written.value, place->occurrence.type,
                                  requestOf(ownedContract()));
    if(!value) return;
    if(!checkType(*value, place->occurrence.type, written.value.source, "an assigned value"_v)) return;

    /*
     * `Assign`, always, and `Reinit` never from here.
     *
     * Whether the old contents are there to be released is a dataflow answer and not a syntactic one
     * - the same `x = e` inside a loop replaces a value on the second iteration and fills a moved-out
     * hole on the first - so the resolver states what was written and `analyze_drop.cpp` normalizes
     * it where the state says otherwise. §5.5's "reinit can be a normalized form rather than a source
     * builder operation", read literally.
     */
    builder.addWrite(expr.source, *place, *value, Destination::Assign);
}

/*
 * `while c: body`.
 *
 * Three blocks and no parameters: the loop's state is the storage its locals already have, so
 * nothing has to be threaded around the back edge. That is a property of this milestone rather than
 * of the representation - a loop carrying an owned value will pass it as a block argument, which is
 * why the edge form exists before there is a loop that needs it.
 *
 * The condition is resolved *into* the header, which is what makes `while a && b:` correct: the
 * blocks `&&` builds sit between the header and the branch, and the back edge below returns to the
 * header rather than to wherever the condition ended.
 */
void Resolver::resolveWhile(Builder& builder, ast::Expr& expr) {
    auto& written = *parse[expr.whileLoop];

    auto header = builder.addBlock(StringId());
    auto body = builder.addBlock(StringId());
    auto exit = builder.addBlock(StringId());

    builder.addJump(expr.source, *header, nullptr, 0);

    builder.setBlock(*header);
    auto condition = resolveValue(builder, written.cond, program.types.boolean, inspectRequest());

    // A condition that did not resolve leaves three blocks that still have to be closed: the header
    // has no branch to give, and the body is entered from nothing. Both are `unreachable` and the
    // walk continues at the exit, so one bad condition produces one diagnostic rather than a cascade
    // about the statements after the loop.
    if(!condition ||
       !checkType(*condition, program.types.boolean, written.cond.source, "a condition"_v)) {
        if(!builder.isTerminated()) builder.addUnreachable(expr.source);
        builder.setBlock(*body);
        builder.addUnreachable(expr.source);
        builder.setBlock(*exit);
        return;
    }

    builder.addBranch(expr.source, builder.resultOf(*condition, UseMode::Inspect), *body, *exit);

    builder.setBlock(*body);
    auto scope = bindings.size();
    resolveStatement(builder, written.body);
    bindings.resize(U32(scope));

    if(!builder.isTerminated()) builder.addJump(expr.source, *header, nullptr, 0);

    builder.setBlock(*exit);
}

/*
 * `return`, which is also Analysis-Lens.md §5.1's second exit out of a continuation.
 *
 * With the lens flattened there is one function and one `return`, so what a `return` written inside
 * a continuation needs beyond an ordinary one is the *cleanups*: it leaves past every open lens
 * frame, and each of them has code after its `yield` that has not run. Running them is the whole
 * reason a scoped-acquisition API is correct on an early return with nothing written at either site.
 *
 * The order is the one the source implies. The returned expression is evaluated here, in the scope
 * it is written in; then the frames are unwound innermost first, each running its own copy of its
 * cleanup - see `runCleanup`; then the value leaves. So a `return` out of `withLock`'s body computes
 * its answer under the lock and unlocks before it goes.
 *
 * The value is parked in storage across the unwinding, and that is not an optimization to remove: a
 * cleanup emits blocks, an operand names a node of its own block (`checkValueScope`), and a value
 * that is still only a node result would have no place for the drops the cleanup runs to be ordered
 * against. Parking it is also what makes the escape rule ordinary - a returned borrow rooted in a
 * frame the cleanup tears down is a read of storage that has been released, which the loan pass
 * reports without knowing what a lens is.
 */
void Resolver::resolveReturn(Builder& builder, ast::Expr& expr) {
    auto& function = *currentFunction;

    // The frames this `return` leaves - `unwindFrames`, which is also what decides that there are
    // none. A `return` before a lens's own `yield` leaves a frame with no cleanup to run, and one
    // written inside a cleanup leaves past the frames outside it and not past that one again.
    auto leaving = false;
    for(auto& frame: lensFrames) leaving |= frame.stage == LensFrame::Continuation;

    if(!expr.ret) {
        if(decl()[function.result.type]->kind != Type::Unit) {
            context.diagnostics.error("this function answers %@, so `return` needs a value"_v,
                                      expr.source, typeText(context, decl(), function.result.type));
            builder.addUnreachable(expr.source);
            return;
        }

        unwindFrames(builder);
        if(builder.isTerminated()) return;

        builder.addReturnUnit(expr.source);
        return;
    }

    auto value = resolveValue(builder, *parse[expr.ret], function.result.type,
                              requestOf(function.result.contract));
    if(!value) {
        if(!builder.isTerminated()) builder.addUnreachable(expr.source);
        return;
    }

    if(!checkType(*value, function.result.type, expr.source, "a returned value"_v)) {
        builder.addUnreachable(expr.source);
        return;
    }

    if(!checkResultContract(*value, expr.source)) {
        builder.addUnreachable(expr.source);
        return;
    }

    if(leaving) {
        auto place = hold(builder, *value, expr.source);
        unwindFrames(builder);

        // A cleanup that left by itself - a `return` of its own, or a diagnostic that closed the
        // block. There is nothing after it, and the value parked above is dead there like any other
        // local the path stopped using.
        if(builder.isTerminated()) return;

        value = readPlace(builder, expr.source, *place, requestOf(function.result.contract));
        if(!value) {
            builder.addUnreachable(expr.source);
            return;
        }
    }

    builder.addReturn(expr.source, *value);
}

/*
 * What leaves a function, against what its result declared - the caller-side half of §2.2.
 *
 * Two questions and they are not the same one. Whether the value is an owner or a loan has to match
 * the declaration exactly: an owned result the caller receives is theirs and a loan is not, and
 * neither can stand in for the other. What *capability* the loan carries may be stronger than the
 * declaration asks - handing back a shared view of storage this frame holds exclusively is a
 * reborrow, which §2.5 permits - and may not be weaker.
 *
 * Where the loan is actually rooted is deliberately not asked here. That is the origin set, it is a
 * CFG answer, and `Analysis::checkReturnConformance` is where it is checked - this is the part that
 * can be decided from the value in hand, which is §2.1 against §2.2 read at one statement.
 */
bool Resolver::checkResultContract(Node& value, LocationId source) {
    auto declared = currentFunction->result.contract;
    auto got = valueOccurrence(value).contract;

    if(declared.isOwned() && !got.isOwned()) {
        context.diagnostics.error(
            "this function answers an owned %@, and this is borrowed - publish an origin on the result with `'`, or `copy` it"_v,
            source, typeText(context, decl(), currentFunction->result.type));
        return false;
    }

    if(!declared.isOwned() && got.isOwned()) {
        context.diagnostics.error(
            "the result names an origin, so it has to be a borrow of something the caller keeps"_v,
            source);
        return false;
    }

    if(declared.access() == Access::Unique && got.access() != Access::Unique) {
        context.diagnostics.error("the result is exclusive, and this is a shared borrow"_v, source);
        return false;
    }

    return true;
}

void Resolver::resolveGlobals(ast::Decl& declaration, Size& next) {
    if(declaration.stmt.kind != ast::Expr::Decl) return;

    for(auto binding: declaration.stmt.decl.contents(parse)) {
        if(next >= writtenGlobals.size()) return;

        auto pointer = writtenGlobals[next++];
        if(!pointer || !binding.content) continue;

        auto global = decl()[pointer];
        auto constant = resolveConstant(*parse[binding.content], nullptr);
        if(!constant.isSet()) continue;

        global->initial = constant;
        global->occurrence = owned(constant.type);
    }
}

/*
 * The linear members of a teardown's subject, given places so that the analysis can see them.
 *
 * `doc/spec/ownership.md`: **an authored `Drop` is answerable for its subject's linear members**.
 * The check is a dataflow one - "still owned when the body ends" - and it runs over move paths,
 * which exist for the places the body *interned*. A body that forgets a member never names it, so
 * the exact case the rule is for is the one with nothing for the walk to look at.
 *
 * Interning a place is not emitting anything: it adds a row to the body's place table and no node,
 * no local and nothing a dump prints. What it says is that these members are positions this body is
 * answerable for, which is true of a teardown's subject and of nothing else - so this runs for a
 * teardown and stops at the first field that holds no linear member.
 *
 * Recursion stops there for a second reason too: a field whose type is `@explicitDrop` in its own
 * right is a member the body has to dispose of *whole*, so what is inside it is that type's teardown
 * to answer for, not this one's.
 */
void Resolver::nameLinearMembers(Builder& builder, Place& place) {
    auto type = decl()[place.occurrence.type];
    if(type->kind != Type::Record) return;

    for(auto field: ((RecordType*)type)->fields.contents(decl())) {
        if(!ownsField(field)) continue;

        auto ownership = ownershipOf(decl(), field.occurrence.type);
        if(!ownership.explicitDrop) continue;

        auto member = builder.fieldPlace(place, field.index);
        if(!needsExplicitDisposal(decl(), field.occurrence.type)) nameLinearMembers(builder, *member);
    }
}

void Resolver::resolveBody(Function& function, ast::Decl& declaration) {
    auto& written = declaration.fun;
    if(!written.body) return;

    // A lens has no standalone body: it is resolved into each of its call sites - see
    // `resolveLensCall`, where the cost of that is written out. One consequence is here rather than
    // hidden: a lens nothing calls is never checked at all, and the opaque boundary is what ends it.
    if(function.lens) return;

    Builder builder(context, program, function);
    currentFunction = &function;
    bindings.clear();

    /*
     * The parameters, as storage and as names, before anything in the body can read one.
     *
     * A parameter is a place from the first statement, which is what every later milestone needs of
     * it: a field of one is projected, a borrow of one has an extent, and `&x: T` writes through it.
     * Making it a value the body copies into a local would be the same storage described twice, and
     * the second description is the one a borrow would have to be checked against.
     */
    for(auto argumentPointer: function.args.contents(decl())) {
        auto local = builder.addParameter(*decl()[argumentPointer]);
        bindings.push(Binding { local->name, local });

        // The subject is the first parameter of a teardown - `Function::teardown` - and this is the
        // one function whose members are followed whether or not the body mentions them.
        if(function.teardown && local->parameter == 0) {
            nameLinearMembers(builder, *builder.localPlace(*local));
        }
    }

    auto& expr = *parse[written.body];
    auto unit = decl()[function.result.type]->kind == Type::Unit;

    if(written.implicitReturn) {
        // `fn f(...) = expr` answers `expr`. A unit-returning one still evaluates it, and what it
        // returns is nothing rather than the value.
        auto value = resolveValue(builder, expr, unit ? nullptr : function.result.type,
                                  unit ? inspectRequest() : requestOf(function.result.contract));

        if(value && !builder.isTerminated()) {
            if(unit) {
                builder.addReturnUnit(expr.source);
            } else if(checkType(*value, function.result.type, expr.source, "a returned value"_v) &&
                      checkResultContract(*value, expr.source)) {
                builder.addReturn(expr.source, *value);
            }
        }
    } else {
        // `fn f(...): statements` answers through `return`, which is what the language says - a
        // block body's last statement is a statement and not a result.
        resolveStatement(builder, expr);
    }

    finish(builder, function, unit, declaration.source);
    builder.seal();
    currentFunction = nullptr;
}

// An instance member's body is an ordinary body: the declaration walk already checked that the
// signature is the one the class declares, so what is left is the same walk every other function
// gets. That it is the same walk is the point - a teardown that ran through a path of its own would
// be a second set of rules for the code the first set exists to place.
void Resolver::resolveInstanceBody(Instance& instance, ast::Decl& declaration) {
    auto member = declaration.instance.decls.get(parse, 0);
    resolveBody(*decl()[instance.function], member);
}

/*
 * Every block that has no terminator, given one.
 *
 * Falling off the end of a unit-returning function is a return; falling off the end of any other is
 * an error the *function* is reported for, once, however many paths reach the end. Blocks a
 * diagnostic already abandoned are closed with `unreachable` rather than reported again, so one
 * mistake produces one message.
 */
void Resolver::finish(Builder& builder, Function& function, bool unit, LocationId source) {
    auto& body = *(*program.mir)[function.mir];
    auto base = *program.mir;
    auto reported = false;

    for(U32 i = 0; i < body.blocks.size(); i++) {
        auto block = body.block(base, BlockId(i));
        auto count = block->nodes.size();

        if(count && isTerminator(base[block->nodes.get(base, count - 1)]->kind)) continue;

        builder.setBlock(*block);

        if(unit) {
            builder.addReturnUnit(source);
            continue;
        }

        // A block with no predecessors is one the arms of an `if` both left; it is unreachable
        // rather than a path that falls off the end, and reporting it would be reporting the shape
        // of the control flow rather than a mistake.
        if(i != 0 && !enteredFrom(body, base, *block)) {
            builder.addUnreachable(source);
            continue;
        }

        if(!reported && !context.diagnostics.errorCount()) {
            context.diagnostics.error("this function answers %@, and a path through it reaches the end without returning"_v,
                                      function.source,
                                      typeText(context, decl(), function.result.type));
            reported = true;
        }

        builder.addUnreachable(source);
    }
}

// Whether any terminator names this block. `Block::incoming` is `seal`'s answer and this runs before
// it, so the question is asked of the edges themselves.
bool Resolver::enteredFrom(Body& body, MirBase base, Block& block) {
    auto target = &block - base;

    for(U32 i = 0; i < body.blocks.size(); i++) {
        auto other = body.block(base, BlockId(i));
        auto count = other->nodes.size();
        if(!count) continue;

        auto last = base[other->nodes.get(base, count - 1)];
        if(!isTerminator(last->kind)) continue;

        auto found = false;
        forEachEdge(*last, [&](Edge& edge) { if(edge.target == target) found = true; });
        if(found) return true;
    }

    return false;
}

/*
 * Pass two, and it is two walks rather than one.
 *
 * Every initializer before any body, because a `let`'s *type* is what its initializer says and a body
 * that reads the name needs it. Declaring the name in pass one is not enough for that: what pass one
 * records is that the module has a global called `answer`, and what a read of it needs is what is in
 * it. One interleaved walk resolved them in source order, so a function above the `let` it read was
 * resolved against a global with no type at all.
 *
 * The counters are per file and per walk, for the reason `writtenGlobals` exists: they index the
 * *written* declarations including the rejected ones, so they have to advance in step with the same
 * traversal that filled the arrays.
 */
void Resolver::resolveInitializers(ast::Module& file) {
    Size next = 0;
    for(auto declaration: file.decls.contents(parse)) {
        if(declaration.kind == ast::Decl::Stmt) resolveGlobals(declaration, next);
    }
}

void Resolver::resolveBodies(ast::Module& file) {
    Size function = 0;
    Size instance = 0;

    for(auto declaration: file.decls.contents(parse)) {
        switch(declaration.kind) {
            case ast::Decl::Fun: {
                if(function >= writtenFunctions.size()) break;

                // Null where the first pass rejected it - see `writtenFunctions`. Its body is not
                // resolved, because there is no signature to resolve it against and every name in it
                // would be reported a second time.
                auto pointer = writtenFunctions[function++];
                if(pointer) resolveBody(*decl()[pointer], declaration);
                break;
            }
            case ast::Decl::Instance: {
                if(instance >= writtenInstances.size()) break;

                auto pointer = writtenInstances[instance++];
                if(pointer) resolveInstanceBody(*decl()[pointer], declaration);
                break;
            }
            default:
                break;
        }
    }
}

} // namespace mir
