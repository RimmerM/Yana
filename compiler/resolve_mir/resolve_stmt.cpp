#include "../parse/ast_visit.h"
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
        case ast::Expr::Match:
            resolveMatch(builder, expr, nullptr, false, inspectRequest());
            return;

        case ast::Expr::If:
            resolveIf(builder, expr, nullptr, false, inspectRequest());
            return;
        case ast::Expr::While:
            resolveWhile(builder, expr);
            return;
        case ast::Expr::For:
            resolveFor(builder, expr);
            return;
        case ast::Expr::Ret:
            resolveReturn(builder, expr);
            return;
        case ast::Expr::Yield:
            resolveYield(builder, expr);
            return;
        case ast::Expr::Break:
        case ast::Expr::Continue:
            resolveExit(builder, expr);
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

        /*
         * A flat lens call `resolveStatements` did not take, which is two shapes and one answer.
         *
         * A statement that binds *several* names - a `where` block, which is the only production
         * that makes one - cannot have an extent, because an extent belongs to the statement and
         * there would be two of them; and a `let` that is a whole body has nothing after it, so its
         * extent is empty however it is read. Both end here rather than in `resolveCall`, so that a
         * flat call is answered by the rule about extents wherever it is written rather than by the
         * one about value positions.
         */
        if(auto callee = lensCallee(*parse[binding.content])) {
            if(expr.decl.size() > 1) {
                unsupported(binding.pat.source, "a lens call written beside another binding"_v);
            } else {
                reportEmptyExtent(parse[binding.content]->source, *callee);
            }

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
 * Does this expression name any of these?
 *
 * A *syntactic* question, deliberately, and Analysis-Lens.md §11.1 is why it can be one: the extent
 * is settled at the call site, so what settles it is what the call site wrote. Anything else would
 * need liveness, which needs the block resolved, which needs the extent - the region has to exist
 * before the statements inside it are built.
 *
 * Over-approximating is safe and under-approximating is not: a name counted where it is shadowed
 * makes the extent longer, which is the extent every lens had before this and costs a frame held
 * open; a name missed would put a use of the hand-over after the frame that made it.
 *
 * The one child that is not a use is a field's name. `x.log` selects `log` from `x` and says nothing
 * about a local called `log`, which `resolveExpr` relies on too - a field is required to be a name
 * there - and counting it would tie an extent to a field name that happens to match.
 */
static bool mentions(ast::ParseBase parse, ast::Expr& expr, const SmallArray<StringId, 8>& names) {
    if(expr.kind == ast::Expr::Var) {
        for(auto name: names) {
            if(name == expr.var) return true;
        }

        return false;
    }

    if(expr.kind == ast::Expr::Field) return mentions(parse, parse[expr.field]->target, names);

    auto found = false;
    ast::visitChildren(parse, expr, [&](ast::Expr& child) {
        if(!found) found = mentions(parse, child, names);
    });

    return found;
}

/*
 * §11.3 - a flat call whose extent covers nothing.
 *
 * A hand-over with no live use is destroyed at the call, and there is no reading of that which was
 * intended: the author meant the scoped form, or meant a plain function. Stated over the *extent*
 * rather than over the result type, so that one rule covers `withLock(m)` written flat, a `let` of a
 * value nobody goes on to use, and a body that is one statement and has nothing after it at all.
 *
 * The one legitimate empty extent is §7.2's skipping lens called for its alternatives alone, which
 * needs the `Try` milestone 6 supplies - `resolveDeclaration` refuses `| else ->` before either
 * caller is reached, so there is nothing here to make an exception for yet.
 */
void Resolver::reportEmptyExtent(LocationId source, Function& callee) {
    context.diagnostics.error(
        "nothing uses what %@ hands over, so its continuation would be empty - write `%@(...):` "
        "with a block if the scope is what was meant"_v,
        source, context.findName(callee.name), context.findName(callee.name));
}

/*
 * Where the flat form's continuation stops - Analysis-Lens.md §11.2, and §11.4's fixpoint.
 *
 * "The continuation runs to the last use of what was handed over" is the sentence, and it is not the
 * whole rule, because a name *bound* inside the extent lives in the continuation. Under the CPS
 * lowering the continuation is a function and its locals are its own frame, so a binding made inside
 * the extent and read after it would be a read of storage that is gone; under the embed lowering it
 * would happen to work, and two lowerings that disagree is the one thing this construct may not do.
 *
 * So the rule is *the last use of the hand-over and of everything bound while it is live*, which is
 * one growing set of names rather than two rules. §11.4's nesting falls out of it rather than being
 * checked separately: a lens called inside this extent binds a name, so the name is tracked, so this
 * extent covers the inner one's last use - which is exactly "an extent is `max(its own last use, the
 * end of every extent opened within it)`", arrived at without a second pass over lens calls.
 *
 * The loop is a fixpoint because the set and the end grow together and each can reveal the other: a
 * statement skipped as outside the extent may be inside it once a later statement extends the end,
 * and the names it binds are then tracked too. Both grow monotonically and are bounded by the block,
 * so it terminates, and a block is small enough that the repeated pass is not worth avoiding.
 *
 * Getting it *short* is caught rather than miscompiled, which is worth knowing when reading the
 * scan: `resolveLensCall` pops the names the extent bound, so a statement wrongly left outside it
 * that reads one of them reports an unknown name. Getting it long costs a frame held open.
 *
 * What it does *not* follow is a value derived from the hand-over into a place of the caller's:
 * `let n = strlen(cstr)` is covered because `n` is bound here, and `out = view(cstr)` writing into a
 * local declared before the call is not. That case is a borrow that outlives its lender, which the
 * ownership passes report in the words they already have; it is not something an extent should
 * silently stretch to accommodate.
 */
U32 Resolver::flatExtent(ast::ParseList<ast::Expr> statements, U32 count, U32 at, StringId handed) {
    // Nothing was bound, so nothing can use it: `let _ = f(...)` and a bare `f(...)` are §11.3's
    // empty extent, which the caller reports.
    if(!handed) return at;

    SmallArray<StringId, 8> tracked;
    tracked.push(handed);

    auto last = at;

    for(auto grown = true; grown;) {
        grown = false;

        for(U32 index = at + 1; index < count; index++) {
            auto statement = statements.get(parse, index);

            if(index > last && mentions(parse, statement, tracked)) {
                last = index;
                grown = true;
            }

            // Past the end as it stands: the statement is outside the extent, so what it binds is
            // the caller's and not the continuation's. A later pass reconsiders it if the end moves.
            if(index > last || statement.kind != ast::Expr::Decl) continue;

            for(auto binding: statement.decl.contents(parse)) {
                // A `let ... in` scopes its name to its own body, so it binds nothing this block
                // goes on to see - and a name that leaves no trace after the statement cannot be
                // what a later one is reading.
                if(binding.in || binding.pat.kind != ast::Pat::Var) continue;

                auto known = false;
                for(auto name: tracked) known |= name == binding.pat.var;

                if(!known) {
                    tracked.push(binding.pat.var);
                    grown = true;
                }
            }
        }
    }

    return last;
}

/*
 * The function a directly named call names, and null for everything else.
 *
 * Only a directly named one, and that is what both callers are asking: a callee reached through a
 * class member or a binding of function type is the erased callback ABI, which is milestone 6's.
 * What a `for` and a lens call have in common is that they decide *what kind of construct they are*
 * before resolving anything, so the lookup is here rather than in `resolveCall`, which resolves
 * before asking.
 */
Function* Resolver::namedCallee(ast::Expr& expression) {
    if(expression.kind != ast::Expr::App) return nullptr;

    auto& application = *parse[expression.app];
    if(application.callee.kind != ast::Expr::Var || findLocal(application.callee.var)) {
        return nullptr;
    }

    auto found = program.findFunction(here(), application.callee.var, U32(application.args.size()));
    return found ? decl()[found] : nullptr;
}

/*
 * ... and null unless it is a `lens fn`.
 *
 * A name that turns out not to be a lens falls through to the ordinary statement it was written in
 * rather than being reported here, which is what keeps one diagnostic per mistake. A `for` asks the
 * question the other way round - see `resolveIteratorFor` - because a call in that position has no
 * other reading to fall through to.
 */
Function* Resolver::lensCallee(ast::Expr& expression) {
    auto callee = namedCallee(expression);
    return callee && callee->lens ? callee : nullptr;
}

/*
 * A run of statements - a block, or the slice of one a flat lens call covers.
 *
 * The flat form of a lens call is here rather than in `resolveDeclaration` because it is the one
 * statement whose meaning is not local to itself: what follows it in the block is its continuation,
 * up to where `flatExtent` says the extent ends, and the statements after that are the caller's
 * again. A walk that handed each statement to `resolveStatement` and no more could not express that.
 */
Node* Resolver::resolveStatements(Builder& builder, ast::ParseList<ast::Expr> statements, U32 from,
                                  U32 to, DeclPtr<Type> hint, bool wantValue, Request wanted) {
    for(U32 index = from; index < to; index++) {
        // A statement after control left is unreachable, and resolving it would append to a block
        // that already ends. Stopping is what the milestone needs; a warning about the code that was
        // dropped is a diagnostic question rather than a representation one.
        if(builder.isTerminated()) break;

        auto statement = statements.get(parse, index);

        /*
         * `let x = f(...)` with no `in` - the flat form, Analysis-Lens.md §11.2.
         *
         * One binding, because the extent is a property of the statement and a `let a = 1, b = f(x)`
         * would give one statement two of them. The other shapes a `let` can have - alternatives, a
         * destructuring pattern, a missing initializer - are `resolveDeclaration`'s to report, and
         * are left to it by falling through.
         */
        if(statement.kind == ast::Expr::Decl && statement.decl.size() == 1) {
            auto& binding = *statement.decl.pointer(parse);

            if(!binding.in && binding.alts.isEmpty() && binding.content &&
               (binding.pat.kind == ast::Pat::Var || binding.pat.kind == ast::Pat::Any)) {
                auto& initializer = *parse[binding.content];

                if(auto callee = lensCallee(initializer)) {
                    index = resolveFlatLensCall(builder, statements, to, index, binding, initializer,
                                                *callee, wantValue);
                    continue;
                }
            }
        }

        if(wantValue && index + 1 == to) return resolveValue(builder, statement, hint, wanted);

        resolveStatement(builder, statement);
    }

    return nullptr;
}

/*
 * The flat form, once the callee is known - Analysis-Lens.md §11.2 and §11.3.
 *
 * Answers the index of the last statement it consumed, so that the walk carries on with the
 * caller's own code after the extent rather than resolving the continuation a second time.
 */
U32 Resolver::resolveFlatLensCall(Builder& builder, ast::ParseList<ast::Expr> statements, U32 to,
                                  U32 at, const ast::VarDecl& binding, ast::Expr& call,
                                  Function& callee, bool wantValue) {
    auto named = binding.pat.kind == ast::Pat::Var;
    auto last = flatExtent(statements, to, at, named ? binding.pat.var : StringId());

    // §11.3 - an extent that covers nothing, which `reportEmptyExtent` states the rule of.
    if(last == at) {
        reportEmptyExtent(call.source, callee);
        return at;
    }

    /*
     * The block's own value, inside the extent.
     *
     * What a continuation answers with is `$r`, and today that is the exit discriminant alone
     * (`declareContinuation`) - so a block whose value is produced inside one has nowhere to put it.
     * Refused by name rather than mis-scoped: `$r` is where this goes, and it is the same slot a
     * skipping lens's `Try` wants, so the two arrive together.
     */
    if(wantValue && last + 1 == to) {
        unsupported(call.source, "a lens call whose extent covers the value of its block"_v);
        return at;
    }

    Extent extent;
    extent.statements = statements;
    extent.from = at + 1;
    extent.to = last + 1;

    resolveLensCall(builder, binding, call, callee, extent);
    return last;
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

    // A call of a `lens fn`, which is what makes the `in` body a continuation. Anything else - a
    // lens reached indirectly included, see `lensCallee` - is the ordinary scoped binding below.
    if(auto callee = lensCallee(initializer)) {
        resolveLensCall(builder, binding, initializer, *callee, Extent { parse[binding.in] });
        return;
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
 * A lens call - Analysis-IR.md §11.4's W-B, and `doc/spec/lenses.md`'s "Calling a lens".
 *
 * A lens *is* called. What makes it one construct rather than a callback convention is the shape the
 * caller takes: the block under `in:` is not code that runs after the call, it is the callee's
 * continuation, so it goes in a region of its own and the call is the terminator that enters it.
 *
 *      ...arguments...                  the caller's own region
 *      lens.call f(...) -> bb(entry)
 *    entry:  %h = argument              what the callee hands over
 *      ...the `in:` body...
 *      jump bb(join)                    falling out, which is one of the region's exits
 *    join:  the caller, again
 *
 * The callee is **opaque**, and that is the whole difference from the flattening this replaces. Its
 * body is resolved once, a diagnostic inside it is reported once, a lens nothing calls is still
 * checked, and a lens may call itself. What the caller reads of it is its signature: what it hands
 * over, under what contract, and what its arguments are lent for.
 *
 * §10.1's requirement is met here rather than in the conversion, and that is why this is a region
 * and not a closure. The continuation is the caller's code under *either* lowering, so its four
 * exits - falling out, `return`, `break`, `continue` - are ordinary CFG edges that liveness, the
 * loan extents and drop elaboration walk. Which form the call is given is
 * `resolve_ssa/convert.cpp`'s choice, and neither form is visible here.
 */
bool Resolver::resolveLensCall(Builder& builder, const ast::VarDecl& binding, ast::Expr& call,
                               Function& callee, const Extent& extent) {
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

    auto named = binding.pat.kind == ast::Pat::Var;

    return handoverCall(builder, call, callee, Continuation::Once,
                        named ? binding.pat.var : StringId(), binding.pat.source,
                        [&](ContinuationId, Block&) { resolveExtent(builder, extent); });
}

/*
 * A call that opens a continuation region - Analysis-IR.md §11.4's W-B, and `doc/spec/lenses.md`'s
 * "Calling a lens".
 *
 * A lens *is* called. What makes it one construct rather than a callback convention is the shape the
 * caller takes: the block under `in:` - or the body of a `for` - is not code that runs after the
 * call, it is the callee's continuation, so it goes in a region of its own and the call is the
 * terminator that enters it.
 *
 *      ...arguments...                  the caller's own region
 *      lens.call f(...) -> bb(entry)
 *    entry:  %h = argument              what the callee hands over
 *      ...the continuation...
 *      jump bb(join)                    falling out, which is one of the region's exits
 *    join:  the caller, again
 *
 * The callee is **opaque**, and that is the whole difference from the flattening this replaces. Its
 * body is resolved once, a diagnostic inside it is reported once, a lens nothing calls is still
 * checked, and a lens may call itself. What the caller reads of it is its signature: what it hands
 * over, under what contract, and what its arguments are lent for.
 *
 * §10.1's requirement is met here rather than in the conversion, and that is why this is a region
 * and not a closure. The continuation is the caller's code under *either* lowering, so its four
 * exits - falling out, `return`, `break`, `continue` - are ordinary CFG edges that liveness, the
 * loan extents and drop elaboration walk. Which form the call is given is
 * `resolve_ssa/convert.cpp`'s choice, and neither form is visible here.
 */
template<class F>
bool Resolver::handoverCall(Builder& builder, ast::Expr& call, Function& callee,
                            Continuation::Multiplicity multiplicity, StringId name,
                            LocationId nameSource, F&& inside) {
    auto& written = *parse[call.app];
    auto source = call.source;

    if(!checkCallShape(written, callee, source)) return false;

    /*
     * The arguments, in the caller's scope and in the caller's block, before the region is opened.
     *
     * `resolveArguments` is the one shared with every other call, which is Analysis-Lens.md §12.1's
     * sixth item asked for by name: the shipping stage has two argument loops, and rules added to
     * one of them went missing from the other twice. It walks the *written* arguments, so the
     * continuation parameter the desugaring appended is simply past the end of the list it reads.
     */
    SmallArray<Node*, 8> arguments;
    TypeArguments typeArguments(*this, callee.gen);
    if(!resolveArguments(builder, written, callee, arguments, typeArguments)) return false;

    /*
     * `$r`, the one type argument no written argument decides - `declareContinuation`.
     *
     * What a continuation answers with is a manifestation choice (Analysis-Lens.md §12.1's fifth
     * item), and the call site is where it is made: today every continuation answers with the exit
     * discriminant alone, because the continuation is a statement and has no value of its own. It is
     * bound *here* rather than left for the conversion because a specialization is keyed by its
     * argument list, and a callee whose result type the conversion decided would be one
     * specialization asked to be two things.
     *
     * An `iter fn` fixed the same type at its declaration (§7.3), so this binds nothing for one and
     * the two agree about the answer by construction rather than by a rule.
     *
     * Bound the way a result hint is bound at an ordinary call, and through the same three steps
     * after it, so that a lens with a functional dependency or an undecided variable reports in the
     * words every other call reports in.
     */
    typeArguments.bind(callee.result.type, program.types.exitCode());
    typeArguments.fillDetermined(source);

    if(auto open = typeArguments.undecided()) {
        context.diagnostics.error("no argument decides what %@ is in this call to %@"_v, source,
                                  context.findName(((VariableType*)decl()[open])->name),
                                  context.findName(callee.name));
        return false;
    }

    if(!dischargeRequirements(callee, typeArguments, source)) return false;

    /*
     * Where the caller carries on, made **before** the region so that the continuation can name it.
     *
     * A `break` is an edge to it written inside the region, so it has to exist while the body is
     * being built - and making it here rather than only for the loop is what keeps one shape: it is
     * a block of the enclosing region either way, which is what makes the region's last edge a
     * rejoin rather than an edge inside it.
     */
    auto join = builder.addBlock(StringId());

    /*
     * The region, opened before the call so that the call can name its entry.
     *
     * `Once` is a transparent lens - `checkHandover`'s rule over the callee's own blocks, and what
     * the marker means. `Many` is an iterator, whose body runs once per element. A skipping lens is
     * `AtMostOnce` and needs the `Try` milestone 6 supplies.
     */
    auto with = typeArguments.with();
    auto region = builder.enterRegion(Continuation::Handover, multiplicity);
    auto entry = builder.addBlock(StringId());

    // Where it rejoins, said now rather than after the body: an iterator departing inside the region
    // falls out of it, and what it needs to name is this block - see `emitDeparture`.
    builder.rejoinRegion(region, *join);

    /*
     * What arrives, read off the *continuation parameter's own type* - which is all an opaque callee
     * gives, and is the same position the callee's `yield` filled.
     *
     * `Continuation::handover` is kept on the region because that is what a later reader has instead
     * of the callee: the conversion builds the outlined continuation's parameter from it, and the
     * entry's block argument is what the ownership passes check against.
     */
    auto& shape = *(FunType*)decl()[decl()[callee.args.get(decl(), callee.continuationArg)]
                                       ->occurrence.type];

    auto handover = substituteOccurrence(context, program,
                                         shape.params.get(decl(), 0).occurrence, with);

    builder.body.region(builder.base(), region)->handover = handover;

    auto handed = builder.addBlockArgument(*entry, handover, source);

    builder.addLensCall(source, callee, region, *entry, arguments.pointer(), arguments.size(), with);
    builder.setBlock(*entry);

    /*
     * The name the call site bound, holding what arrived.
     *
     * Writable exactly when what arrived is exclusive, which is the rule `addParameter` states about
     * a `&` parameter and is the same question: the declaration said `-> &T`, so the continuation
     * has write access, and a binding that refused it would be a capability granted and then thrown
     * away. The sigil is not written at the call site - see `resolveLensCall`'s refusal - so this is
     * the only place the answer can come from.
     */
    auto local = builder.addLocal(name, handover, nameSource,
                                  handover.contract.access() == Access::Unique);

    builder.addWrite(nameSource, *builder.localPlace(*local), *handed, Destination::Init);

    auto scope = bindings.size();
    if(name) bindings.push(Binding { name, local });

    auto departures = handovers;

    inside(region, *join);
    auto fellOut = !builder.isTerminated();

    bindings.resize(U32(scope));

    /*
     * Where the caller carries on - or does not.
     *
     * Out of the region first, so that the jump below is emitted in the caller's own region. A
     * continuation no path of which falls out has a join nothing enters: it is closed as
     * unreachable, which is what tells an enclosing walk that control left.
     */
    builder.leaveRegion();

    if(fellOut) builder.addJump(source, *join, nullptr, 0);

    builder.setBlock(*join);

    // A join nothing enters is closed as unreachable, which is what tells the enclosing walk that
    // control left. A `break` is an edge into it, so the question is about the block rather than
    // about the last statement of the body.
    if(!fellOut && !enteredFrom(builder.body, builder.base(), *join)) {
        builder.addUnreachable(source);
    }

    // A departure inside the region left the region and no more, so the iterator asks again here -
    // one level of loops per rejoin, which is what makes a departure through three adaptors leave
    // three frames with no rule per frame. Only where the region handed over at all: a departure is
    // a `yield`'s doing, and a region with none cannot have one to resume.
    if(handovers != departures) resumeDeparture(builder, source);

    return true;
}

/*
 * The continuation's own code, which is the one thing the two call forms disagree about.
 *
 * The scoped form wrote it and the flat form derived it (`flatExtent`), and everything else about a
 * lens call - the arguments, the region, what arrives, how the caller rejoins - is above and is the
 * same either way. `inspectRequest` because a continuation is a statement: what it answers with is
 * `$r`, which the call site decided, and not the value of its last expression.
 */
void Resolver::resolveExtent(Builder& builder, const Extent& extent) {
    if(extent.written) {
        resolveStatement(builder, *extent.written);
        return;
    }

    resolveStatements(builder, extent.statements, extent.from, extent.to, nullptr, false,
                      inspectRequest());
}

/*
 * `yield` - a call of the continuation, and nothing else.
 *
 * The `yield` form's desugaring (`declareContinuation`) is what makes this ordinary: `lens fn f(a)
 * -> P` has a parameter `$body: (P) -> $r`, so `yield e` is `$body(e)` and everything after it in
 * the body is code that runs with the answer in hand. That answer is what the lens itself returns -
 * by falling off the end, or through a bare `return` written in the cleanup - so it is parked in
 * `$yielded` rather than left a node result, because a value that is only a node result cannot cross
 * a block (`checkValueScope`).
 *
 * What used to be here was the *caller's* continuation, spliced into the middle of this walk. That
 * it is gone is the milestone: a lens is a function, its body is walked once, and which lowering a
 * call gets is the conversion's choice rather than the resolver's.
 */
void Resolver::resolveYield(Builder& builder, ast::Expr& expr) {
    auto function = currentFunction;

    if(!function || !function->lens || !continuationLocal) {
        context.diagnostics.error(
            "`yield` hands a value to a lens's continuation, and this is not inside a `lens fn` or an `iter fn`"_v,
            expr.source);
        return;
    }

    /*
     * Has the consumer already left? - and the answer decides whether the hand-over happens at all.
     *
     * An iterator that departs carries on through whatever it has after its loops, and that trailing
     * code may itself `yield`: a consumer that has left must not be re-entered, so the call is
     * guarded rather than the statements around it. The value is resolved **inside** the guard for
     * the same reason - computing what to hand over is work done for a consumer that is gone.
     *
     * Only where a second `yield` can be reached at all, which `guardYields` answers off the source.
     */
    Block* skipped = nullptr;

    if(function->iterator && guardYields) {
        auto standing = readPlace(builder, expr.source, *builder.localPlace(*yieldedLocal),
                                  inspectRequest());
        if(!standing) return;

        auto zero = builder.addConst(expr.source, intConstant(program.types.exitCode(), 0));
        auto going = applyBinary(builder, expr.source, BinaryOp::Eq, *standing, *zero);
        if(!going) return;

        auto handing = builder.addBlock(StringId());
        skipped = builder.addBlock(StringId());

        builder.addBranch(expr.source, builder.resultOf(*going, UseMode::Inspect), *handing,
                          *skipped, nullptr, 0);

        builder.setBlock(*handing);
    }

    auto& handover = function->handover;
    auto wanted = requestOf(handover.contract);

    Node* value = nullptr;

    if(expr.yield) {
        value = resolveValue(builder, *parse[expr.yield], handover.type, wanted);
        if(!value) return;
        if(!checkType(*value, handover.type, expr.source, "a handed-over value"_v)) return;
    } else if(decl()[handover.type]->kind == Type::Unit) {
        value = builder.addConst(expr.source, unitConstant(program.types.unit));
    } else {
        context.diagnostics.error("%@ hands over %@, so this `yield` needs a value"_v, expr.source,
                                  context.findName(function->name),
                                  typeText(context, decl(), handover.type));
        return;
    }

    /*
     * The continuation, read out of the parameter it is.
     *
     * `inspectRequest` for the reason `addCallValue` states: calling a function value neither
     * consumes it nor takes a loan of anything inside it - what the call reads is the two words
     * copied out of the pair. It is also what keeps a lens from being able to *keep* its
     * continuation, which is the second of the three guarantees the marker makes.
     */
    auto continuation = readPlace(builder, expr.source,
                                  *builder.localPlace(*continuationLocal), inspectRequest());
    if(!continuation) return;

    Node* argument = value;
    auto answer = builder.addCallValue(expr.source, *continuation, &argument, 1);
    if(!answer) return;

    if(!function->iterator) {
        builder.addWrite(expr.source, *builder.localPlace(*yieldedLocal), *answer, Destination::Init);
        return;
    }

    // Counted so that `handoverCall` can tell whether the region it just built could have departed
    // at all - a departure is a `yield`'s doing, and there is no other way to set `$yielded`.
    handovers++;

    /*
     * The iterator half: what came back says whether to carry on.
     *
     * Falling out is the loop asking for the next element and is the fall-through. Anything else is
     * the loop leaving - a `break`, or a `return` travelling out of the enclosing function - and
     * what the iterator does with it is to keep it and hand it back **unexamined** when it is done:
     * which of the two it is belongs to the consumer, and every frame between here and the call site
     * passes it along by an ordinary return. That is why the step signal is fixed at the declaration
     * rather than free - a variable could be handed on but not compared, and the comparison is this.
     *
     * Kept in `$yielded` rather than returned here, because what the iterator has left to do is the
     * point: `emitDeparture` leaves the loops the `yield` was inside, the statements below them run,
     * and `finish` answers with what is in the storage. That is one local doing for an iterator what
     * it already does for a lens - the answer the continuation gave, which is the function's own.
     *
     * The comparison is skipped where a departure would go nowhere - no loop to leave and no region
     * to fall out of - because the answer then cannot change where control goes: the statements below
     * are already the trailing ones. `pair` in `Iterator.yana` is that shape, and it is why a
     * straight-line iterator pays for the guard above and for nothing here.
     */
    Node* going = nullptr;

    if(!departureFallsThrough(builder)) {
        auto zero = builder.addConst(expr.source, intConstant(program.types.exitCode(), 0));
        going = applyBinary(builder, expr.source, BinaryOp::Eq, *answer, *zero);
        if(!going) return;
    }

    builder.addWrite(expr.source, *builder.localPlace(*yieldedLocal), *answer, Destination::Assign);

    if(!going && !skipped) return;

    auto next = builder.addBlock(StringId());

    if(!going) {
        builder.addJump(expr.source, *next, nullptr, 0);
        builder.setBlock(*skipped);
        builder.addJump(expr.source, *next, nullptr, 0);
    } else {
        auto leaving = builder.addBlock(StringId());
        builder.addBranch(expr.source, builder.resultOf(*going, UseMode::Inspect), *next, *leaving,
                          nullptr, 0);

        // The guarded arm leaves by the same block: a `yield` skipped because the consumer is gone
        // is a departure that has already happened, and what it still owes is to leave the loops.
        if(skipped) {
            builder.setBlock(*skipped);
            builder.addJump(expr.source, *leaving, nullptr, 0);
        }

        builder.setBlock(*leaving);
        emitDeparture(builder, expr.source);
    }

    builder.setBlock(*next);
}

/*
 * Every path through a lens hands over exactly once - Analysis-Lens.md §7.1.
 *
 * A forward walk over the callee's own blocks rather than a count of `yield` statements, because
 * both halves of the rule are about paths, and **once per lens** rather than once per call site,
 * which is what W-A buys. A lens that hands over on one arm of an `if` and not the other is a
 * *skipping* lens, whose result carries a `Try` and whose call site says where the skip goes;
 * silently accepting it here would be a continuation that sometimes does not run at all. One that
 * hands over twice would run one continuation twice, which is the one thing the marker promises it
 * does not.
 *
 * Two bits per block: reached without having handed over, and reached after having. The `yield` is
 * an ordinary call now, so what the walk looks for is a write into `$yielded` rather than a
 * terminator - which is also what makes "twice" mean twice on one path rather than twice in the
 * text.
 */
bool Resolver::checkHandover(Builder& builder, Function& function, LocationId source) {
    auto base = builder.base();
    auto& body = builder.body;
    auto slot = yieldedLocal->index;

    static constexpr U8 kBefore = 1;
    static constexpr U8 kAfter = 2;

    SmallArray<U8, 32> state;
    for(U32 i = 0; i < body.blocks.size(); i++) state.push(0);

    SmallArray<U32, 16> pending;

    auto mark = [&](BlockId block, U8 bits) {
        if(!block.valid()) return;
        if((state[block.index] & bits) == bits) return;

        state[block.index] = U8(state[block.index] | bits);
        pending.push(block.index);
    };

    mark(BlockId(0), kBefore);

    auto twice = false;
    auto missing = false;

    while(pending.size()) {
        auto index = pending[pending.size() - 1];
        pending.resize(U32(pending.size() - 1));

        auto block = body.block(base, BlockId(index));
        auto bits = state[index];

        // The hand-over is the write that fills `$yielded`, and there is at most one per block
        // because a second one in the same block is the same defect the two-bit state reports.
        for(auto nodePointer: block->nodes.contents(base)) {
            auto& node = *base[nodePointer];
            if(node.kind != Node::Write) continue;

            auto destination = ((NodeWrite&)node).destination;
            if(destination.source() != SourceKind::Place) continue;

            auto place = body.place(base, destination.placeId());
            if(place->kind != Place::Local || place->payload != slot.index) continue;

            if(bits & kAfter) twice = true;
            bits = kAfter;
        }

        auto count = block->nodes.size();
        if(!count) continue;

        auto last = base[block->nodes.get(base, count - 1)];
        if(!isTerminator(last->kind)) continue;

        // A path that leaves without having handed over. A `return` is the only terminator that can
        // do it, since falling off the end is `finish`'s and is reported below by the same bit.
        if(last->kind == Node::Return && !(bits & kAfter)) missing = true;

        auto reached = bits;
        forEachEdge(*last, [&](Edge& edge) { mark(base[edge.target]->index, reached); });
    }

    if(twice) {
        context.diagnostics.error("a path through %@ hands over twice, and one continuation runs once"_v,
                                  source, context.findName(function.name));
        return false;
    }

    /*
     * Falling off the end without having handed over, which is the other half of the same rule.
     *
     * Read off the blocks `finish` will close rather than off a single exit, because a lens body has
     * as many of those as it has branches - and a block nothing enters is not a path, so a block the
     * walk never marked is not one either.
     */
    for(U32 i = 0; i < body.blocks.size(); i++) {
        auto block = body.block(base, BlockId(i));
        auto count = block->nodes.size();
        if(count && isTerminator(base[block->nodes.get(base, count - 1)]->kind)) continue;
        if(!(state[i] & kBefore)) continue;

        missing = true;
    }

    if(missing) {
        context.diagnostics.error("a path through %@ reaches the end without a `yield`, and a lens that may skip its continuation is not implemented"_v,
                                  source, context.findName(function.name));
        return false;
    }

    return true;
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

    // Storage that already exists says what may be in it, and a closure carries more than a plain
    // `(I32) -> I32` local was declared to hold - `checkCarriedOrigins`. A `let` of the same value
    // is accepted, because there the storage is minted from the value rather than the other way
    // round.
    if(!checkCarriedOrigins(*value, place->occurrence, written.value.source,
                            "this storage"_v)) {
        return;
    }

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
 * Three blocks. The loop's own state is the storage its locals already have, so nothing the *source*
 * declared is threaded around the back edge - but a value the enclosing expression is holding
 * crosses all three, and the header is where it becomes a parameter. That is `Carry`, and a loop
 * needs it in one more place than a branch does: the header is entered twice, so the back edge
 * supplies the same positions the entry did.
 *
 * The condition is resolved *into* the header, which is what makes `while a && b:` correct: the
 * blocks `&&` builds sit between the header and the branch, and the back edge below returns to the
 * header rather than to wherever the condition ended.
 */
/*
 * `while c:` and `while x is p:`.
 *
 * Not an `armsValue`, and the difference is the back edge: the alternatives of an `if` are two ways
 * forward from one point, and a loop's body ends by re-entering the header - so the header's own
 * parameters are what everything after it reads, and there is nothing to merge at the end. What it
 * does share is `resolveCondition`, so `while x is Full(v):` binds `v` for the body by the same rule
 * `if x is Full(v)` binds it for its arm, and a nested pattern's tests reach the exit the same way.
 */
void Resolver::resolveWhile(Builder& builder, ast::Expr& expr) {
    auto& written = *parse[expr.whileLoop];

    Carry carry(*this, expr.source);

    auto header = builder.addBlock(StringId());
    auto exit = builder.addBlock(StringId());

    carry.declare(builder, *header);
    carry.declare(builder, *exit);

    SmallArray<Operand, 8> entry;
    carry.edge(builder, entry);
    builder.addJump(expr.source, *header, entry.pointer(), entry.size());

    builder.setBlock(*header);
    carry.arrive(builder, *header, 0);

    // Resolved after the arrival, so that the condition reads the header's own parameters: a
    // condition that branches - `while a && b:` - carries them on again from there, and one that
    // named what stood before the loop would be naming a value the back edge never re-supplies.
    auto condition = resolveCondition(builder, written.cond);

    // A condition that did not resolve leaves the header with no branch to give. It is `unreachable`
    // and the walk continues at the exit, so one bad condition produces one diagnostic rather than a
    // cascade about the statements after the loop.
    SmallArray<PatternBinding, 4> made;
    if(!condition || !resolveTests(builder, condition, exit, carry, made)) {
        if(!builder.isTerminated()) builder.addUnreachable(expr.source);
        builder.setBlock(*exit);
        carry.arrive(builder, *exit, 0);
        return;
    }

    // The body runs in whatever block the tests left current, which is the one they proved. Its own
    // names - the pattern's, and whatever it declares - last exactly as long as it does.
    auto scope = bindings.size();
    bindPattern(builder, made);

    {
        // `continue` re-enters at the header, which re-tests the condition - so a `while` needs no
        // block a fall-through does not already use, and the two edges are the same edge.
        Loop frame(*this, &carry, *exit, *header, builder.region(), false);
        resolveStatement(builder, written.body);
    }

    bindings.resize(U32(scope));

    if(!builder.isTerminated()) {
        SmallArray<Operand, 8> back;
        carry.edge(builder, back);
        builder.addJump(expr.source, *header, back.pointer(), back.size());
    }

    builder.setBlock(*exit);
    carry.arrive(builder, *exit, 0);
}

Loop::Loop(Resolver& resolver, Carry* carry, Block& exit, Block& again, ContinuationId region,
           bool leavesRegion):
    resolver(resolver), outer(resolver.loops), carry(carry), exit(exit), again(again),
    region(region), leavesRegion(leavesRegion) {
    resolver.loops = this;
}

Loop::~Loop() { resolver.loops = outer; }

/*
 * `break` and `continue` - Analysis-Lens.md §5.1's loop-shaped exits.
 *
 * An ordinary edge, and that is the whole of what they are once a loop records its two blocks: the
 * §5.1 argument is that they are the loop-shaped instance of the mechanism `return` is the
 * function-shaped one, and *within one region* every instance of that mechanism is a jump. The
 * carry goes on the edge for the reason it goes on every other one - a value the enclosing
 * expression is holding crosses the loop's blocks whichever edge reaches them.
 *
 * The exception is the edge that leaves a continuation region, which is not an edge at all under
 * the CPS lowering: the block on the far side belongs to another function once the region is
 * outlined, so leaving is a row of the exit signal and there are two of them - falling out, and
 * `return`. That is the one refusal here, and it names what it waits for.
 */
void Resolver::resolveExit(Builder& builder, ast::Expr& expr) {
    auto leaving = expr.kind == ast::Expr::Break;

    if(!loops) {
        context.diagnostics.error("%@ is written outside a loop, so there is no loop for it to leave"_v,
                                  expr.source, leaving ? "`break`"_v : "`continue`"_v);
        builder.addUnreachable(expr.source);
        return;
    }

    /*
     * `break v`.
     *
     * A loop is a statement here, so there is nowhere for the value to go: what would hold it is
     * the loop's own result, which is Analysis-Lens.md's V3 and arrives with the `iter(Y <- R | E)`
     * result type rather than with the loop. Refused rather than evaluated and dropped, because a
     * value silently discarded is the one reading nobody wrote it for.
     */
    if(leaving && expr.breakValue) {
        unsupported(expr.source, "`break` carrying a value"_v);
        builder.addUnreachable(expr.source);
        return;
    }

    auto& loop = *loops;

    if(loop.region != builder.region()) {
        unsupported(expr.source, leaving ? "`break` out of a lens call"_v
                                         : "`continue` out of a lens call"_v);
        builder.addUnreachable(expr.source);
        return;
    }

    /*
     * The loop that *is* a continuation region - `for x in items(xs)`.
     *
     * Both exits leave the region for the block the caller carries on at, and what tells them apart
     * is the node: ending an iteration is falling out, which is what asks the iterator for the next
     * element, and `break` is the row that stops it. Nothing travels on either edge - see
     * `Builder::addBreak`.
     */
    if(loop.leavesRegion) {
        if(leaving) builder.addBreak(expr.source, loop.exit);
        else builder.addJump(expr.source, loop.again, nullptr, 0);

        return;
    }

    SmallArray<Operand, 8> arguments;
    loop.carry->edge(builder, arguments);

    builder.addJump(expr.source, leaving ? loop.exit : loop.again, arguments.pointer(),
                    arguments.size());
}

/*
 * The outermost loop of the region currently being emitted into, and null where it has none.
 *
 * Outermost and not innermost, which is the whole of what a departure is: `break` leaves the loop
 * the author wrote it in, and a consumer that has left leaves *every* loop the `yield` was inside,
 * because a loop whose hand-over has nowhere to go has nothing left to do. Bounded by the region
 * because a loop outside one is another function's block under the CPS lowering - the same bound
 * `resolveExit` refuses across.
 */
static Loop* outermostLoopIn(Loop* loops, ContinuationId region) {
    Loop* found = nullptr;
    for(auto frame = loops; frame && frame->region == region; frame = frame->outer) found = frame;
    return found;
}

bool Resolver::departureFallsThrough(Builder& builder) {
    if(outermostLoopIn(loops, builder.region())) return false;

    auto region = builder.body.region(builder.base(), builder.region());
    return !region->destination.valid();
}

/*
 * Leaving, once the consumer has - `doc/spec/lenses.md`'s cleanup rule, and the one place it is
 * written.
 *
 * Three shapes and one sentence: the departure resumes after the outermost loop of this region. An
 * ordinary loop is left by the edge its own `break` takes, so a value crossing it crosses on this
 * edge too; a `for` over another iterator is left by the row that stops it, which is what makes a
 * departure through three adaptors stop all three; and a region with no loop is left by *falling
 * out of it*, which is deliberate - a lens's continuation ending is the lens running its own
 * cleanup, so a departure inside `withLock(m):` unlocks on the way past exactly as a `return` does.
 *
 * Where the region is the function's own and holds no loop, the statements below the `yield` are
 * the trailing ones and control simply carries on. That is not a fourth case: it is this one with
 * nothing to leave, and `departureFallsThrough` is what lets a caller skip asking.
 */
void Resolver::emitDeparture(Builder& builder, LocationId source) {
    if(auto loop = outermostLoopIn(loops, builder.region())) {
        if(loop->leavesRegion) {
            builder.addBreak(source, loop->exit);
            return;
        }

        SmallArray<Operand, 8> arguments;
        loop->carry->edge(builder, arguments);
        builder.addJump(source, loop->exit, arguments.pointer(), arguments.size());
        return;
    }

    auto region = builder.body.region(builder.base(), builder.region());
    if(!region->destination.valid()) return;

    builder.addJump(source, *builder.body.block(builder.base(), region->destination), nullptr, 0);
}

/*
 * The test an iterator owes after any call that opened a region - `handoverCall`.
 *
 * A departure inside a region leaves that region and no more, because the loops outside it are
 * outside the block the departure could name. So the iterator asks again where the region rejoins,
 * and the answer carries it one level further out; the loop that ends up ended is the outermost one
 * either way, reached one region at a time rather than by an edge that crosses them.
 */
void Resolver::resumeDeparture(Builder& builder, LocationId source) {
    if(!currentFunction || !currentFunction->iterator || !yieldedLocal) return;
    if(builder.isTerminated() || departureFallsThrough(builder)) return;

    auto standing = readPlace(builder, source, *builder.localPlace(*yieldedLocal), inspectRequest());
    if(!standing) return;

    auto zero = builder.addConst(source, intConstant(program.types.exitCode(), 0));
    auto going = applyBinary(builder, source, BinaryOp::Eq, *standing, *zero);
    if(!going) return;

    auto next = builder.addBlock(StringId());
    auto leaving = builder.addBlock(StringId());

    builder.addBranch(source, builder.resultOf(*going, UseMode::Inspect), *next, *leaving,
                      nullptr, 0);

    builder.setBlock(*leaving);
    emitDeparture(builder, source);

    builder.setBlock(*next);
}

/*
 * `for pat in ...` - the two loops that share a spelling and nothing else.
 *
 * A counted `for` walks an interval and names no callee; an iterating one runs a `lens fn`'s
 * iterator sibling and its body is that callee's continuation. Which was written is `to`: an
 * interval is the only form with a far end, so the test is the presence of what the two forms
 * disagree about rather than a guess about the source expression.
 */
void Resolver::resolveFor(Builder& builder, ast::Expr& expr) {
    auto& loop = *parse[expr.forLoop];

    if(loop.to) {
        resolveCountedFor(builder, expr, loop);
        return;
    }

    resolveIteratorFor(builder, expr, loop);
}

/*
 * `for pat in f(...)` - Analysis-Lens.md §7.3, and the same call `resolveLensCall` makes.
 *
 * An iterator's continuation is the loop body, so the whole of what is different from a lens call
 * is written here and the rest is `handoverCall`: the multiplicity is `Many`, the name comes from a
 * pattern rather than a `let`, and the body is resolved inside a `Loop` frame so that `break` and
 * `continue` mean this loop.
 *
 * Nothing suspends. The iterator's frame stays live for the whole loop and each `yield` is a call
 * out and an ordinary return back in, which is what makes internal iteration ordinary inlinable
 * code rather than a second mechanism - Analysis-IR.md §10.2 is where the *external* form arrives,
 * and it is a second ABI rather than a change to this one.
 */
void Resolver::resolveIteratorFor(Builder& builder, ast::Expr& expr, ast::ForExpr& loop) {
    if(loop.step) {
        context.diagnostics.error(
            "`step` belongs to a counted `for` and needs the interval it steps over - write `a .. b step n`, or drop it to take what the call yields one at a time"_v,
            parse[loop.step]->source);
        return;
    }

    /*
     * Which iterator the loop runs, by name.
     *
     * A `for` reaches its iterator through a written call and nothing derives one from a type: a
     * container has several traversals and naming the one meant is the point, which is why
     * `for x in xs` over a container is a message rather than a lookup. An iterator travelling as a
     * *value* - which is what an adaptor chain's inner stage is - is the erased callback ABI and is
     * milestone 6's.
     */
    auto callee = namedCallee(loop.from);

    if(!callee) {
        context.diagnostics.error(
            "`for` runs a call of a named `iter fn`, and no rule finds one from the type of a value - name the traversal, as in `for x in items(xs)`"_v,
            loop.from.source);
        return;
    }

    // Named, and the wrong kind - which is worth telling apart from "not named", because the two
    // are answered differently: one is a traversal to write and the other is a declaration to fix.
    if(!callee->iterator) {
        auto name = context.findName(callee->name);

        if(callee->lens) {
            context.diagnostics.error(
                "%@ is a lens, so it hands over once and a `for` has no second element to ask for - write `let x = %@(...) in:` for the one it hands over"_v,
                loop.from.source, name, name);
        } else {
            context.diagnostics.error(
                "%@ is not an `iter fn`, so a `for` loop has nothing to be the body of"_v,
                loop.from.source, name);
        }

        return;
    }

    // No sigil check, unlike a lens call's `let`: a `for` binding has no place to write one, so what
    // the body receives under is the declaration's and there is nothing here that could disagree.
    if(loop.pat.kind != ast::Pat::Var && loop.pat.kind != ast::Pat::Any) {
        unsupported(loop.pat.source, "destructuring a `for` binding"_v);
        return;
    }

    auto named = loop.pat.kind == ast::Pat::Var;

    handoverCall(builder, loop.from, *callee, Continuation::Many,
                 named ? loop.pat.var : StringId(), loop.pat.source,
                 [&](ContinuationId region, Block& join) {
        Loop frame(*this, nullptr, join, join, region, true);
        resolveStatement(builder, loop.body);
    });
}

/*
 * `for i in a .. b`, `a ..= b`, `a downto b`, each optionally `step n`.
 *
 *      [order] [reach]  header  body...  advance  step  exit
 *
 * The header is the loop's one entry and tests the interval's far end, which is what makes an empty
 * range run no times. The **advance** is separate from the step because it is where `continue`
 * lands: a `continue` has to run the guard, or a loop whose last iteration was continued out of
 * would step past the end of its type rather than stopping.
 *
 * That guard is the one part of this that is not obvious. A counter is a value of its own type, so
 * `for i in 0 ..= 255` on a `U8` cannot step to 256 to discover it is done - the step is taken only
 * once the *distance still to go* is known to hold one, which is a subtraction on the side the
 * header has already proved is the larger and therefore cannot wrap. The ascending half-open loop
 * stepping by one is the exception and is nearly every counted loop written: the header proved
 * `i < to`, so `i + 1` is at most `to` and is a value of the type by construction. It steps
 * unguarded, which is what makes `for i in 0 .. n` and the hand-written `while` compile alike.
 *
 * The counter is storage rather than a block parameter, and the name the pattern binds is a
 * *second* local read out of it. One storage with two names is the defect
 * `Analysis-IR.md` §5.5 is written against; the copy is what a binding is, and the loop variable
 * genuinely is not the counter - assigning to `i` may not move the loop along.
 */
void Resolver::resolveCountedFor(Builder& builder, ast::Expr& expr, ast::ForExpr& loop) {
    auto source = expr.source;
    auto ascending = !loop.reverse;

    if(loop.pat.kind != ast::Pat::Var && loop.pat.kind != ast::Pat::Any) {
        unsupported(loop.pat.source, "destructuring a `for` binding"_v);
        return;
    }

    auto& toWritten = *parse[loop.to];

    /*
     * The counter's type, decided by the two bounds together.
     *
     * Neither end is more authoritative than the other, and this stage settles a literal from its
     * hint: so the *written* literal is the one that takes the other's type, and `for i in 0 .. n`
     * counts at `n`'s type rather than defaulting and then disagreeing with it. The order the two
     * are resolved in follows, which costs nothing - a literal builds a constant and runs no code,
     * so there is no evaluation to reorder.
     */
    Node* from = nullptr;
    Node* to = nullptr;

    if(ast::isLiteral(loop.from) && !ast::isLiteral(toWritten)) {
        to = resolveValue(builder, toWritten, nullptr, inspectRequest());
        if(!to) return;

        HeldValue held(*this, to);
        from = resolveValue(builder, loop.from, valueType(*to), inspectRequest());
    } else {
        from = resolveValue(builder, loop.from, nullptr, inspectRequest());
        if(!from) return;

        HeldValue held(*this, from);
        to = resolveValue(builder, toWritten, valueType(*from), inspectRequest());
    }

    if(!from || !to) return;

    auto counterType = valueType(*from);

    if(!sameType(counterType, valueType(*to))) {
        context.diagnostics.error("this `for` counts from %@ to %@, which are different types"_v,
                                  source, typeText(context, decl(), counterType),
                                  typeText(context, decl(), valueType(*to)));
        return;
    }

    // An interval is walked by adding to a counter and comparing it, so what it counts over is a
    // type those mean something on. Asked once, here, rather than arriving as `add cannot be
    // applied to String` from a subtraction the author did not write.
    if(!operandTypeAllowed(decl(), OpCategory::Integer, counterType)) {
        context.diagnostics.error("a `for` counts over an interval of whole numbers, and %@ is not one"_v,
                                  source, typeText(context, decl(), counterType));
        return;
    }

    /*
     * The step, at the counter's type, and one where none was written.
     *
     * A step of zero never reaches the far end, so it is a loop that does not stop. Rejected where
     * it can be seen, which is a written literal - a computed zero is the same loop and is not
     * something this can know.
     */
    Node* step = nullptr;

    if(loop.step) {
        auto& written = *parse[loop.step];

        if(ast::isLiteral(written) && written.lit.i() == 0) {
            context.diagnostics.error("a `for` step of zero never reaches the end of its range"_v,
                                      written.source);
            return;
        }

        HeldValue heldFrom(*this, from);
        HeldValue heldTo(*this, to);
        step = resolveValue(builder, written, counterType, inspectRequest());
    } else {
        step = builder.addConst(source, intConstant(counterType, 1));
    }

    if(!step) return;

    // Whether the advance needs its guard at all - see the header comment. Read off what was
    // written rather than off the resolved step, because a constant this stage folded is a fact
    // about the optimizer and the shape of the loop may not depend on one.
    auto steppingByOne = !loop.step ||
                         (ast::isLiteral(*parse[loop.step]) && parse[loop.step]->lit.i() == 1);
    auto guarded = !(ascending && !loop.inclusive && steppingByOne);

    /*
     * The far end and the step, which are values of the block the loop was written in and are read
     * inside it - so they cross, and crossing is `Held` and `Carry` exactly as it is for the left
     * operand of an addition. Registered before the carry is taken, which is what puts them in it.
     */
    HeldValue heldTo(*this, to);
    HeldValue heldStep(*this, step);

    Carry carry(*this, source);

    auto header = builder.addBlock(StringId());
    auto advance = builder.addBlock(StringId());
    auto exit = builder.addBlock(StringId());

    carry.declare(builder, *header);
    carry.declare(builder, *advance);
    carry.declare(builder, *exit);

    auto counter = builder.addLocal(StringId(), valueOccurrence(*from), source, true);
    builder.addWrite(source, *builder.localPlace(*counter), *from, Destination::Init);

    /*
     * Where a descending loop starts, which is **one stride below the bound written first**.
     *
     * `a downto b` is `0 .. a` walked backwards - `doc/spec/expressions.md`'s interval table - so it
     * yields `a-1` down to `b` and is empty when `a <= b`. The bound it excludes is the one written
     * first, which is what makes reversing a loop a one-token edit rather than arithmetic moved to
     * the other end.
     *
     * Two guards before the counter can be moved there, and both are about *wrapping* rather than
     * about emptiness. `a - b` is a subtraction on the side the first proved is the larger; the
     * second is what says the span holds a stride at all, so `a - step` lands on a value of the type
     * rather than below the bottom of it. `for i in 2 downto 0 step 5` is the case: ordered, and
     * still with no first element to produce.
     *
     * They read the counter rather than `from` for the reason the advance does: a value names the
     * block it was built in, and the storage is what crosses one. The header's own test is then
     * always true descending, and is emitted anyway so that the cycle has one header rather than
     * being entered at its body.
     */
    if(!ascending) {
        auto at = readPlace(builder, source, *builder.localPlace(*counter), inspectRequest());
        if(!at) return;

        auto ordered = applyBinary(builder, source, BinaryOp::Gt, *at, *to);
        if(!ordered) return;

        failIfNot(builder, *ordered, *exit, carry, source);

        auto held = readPlace(builder, source, *builder.localPlace(*counter), inspectRequest());
        if(!held) return;

        auto span = applyBinary(builder, source, BinaryOp::Sub, *held, *to);
        if(!span) return;

        auto fits = applyBinary(builder, source, BinaryOp::Ge, *span, *step);
        if(!fits) return;

        failIfNot(builder, *fits, *exit, carry, source);

        auto top = readPlace(builder, source, *builder.localPlace(*counter), inspectRequest());
        if(!top) return;

        auto first = applyBinary(builder, source, BinaryOp::Sub, *top, *step);
        if(!first) return;

        builder.addWrite(source, *builder.localPlace(*counter), *first, Destination::Assign);
    }

    SmallArray<Operand, 8> entry;
    carry.edge(builder, entry);
    builder.addJump(source, *header, entry.pointer(), entry.size());

    /*
     * The test that says whether this iteration runs at all.
     *
     * `<`, `<=` or `>=`, which is the whole of the difference between the three spellings of an
     * interval. `to` is read here rather than carried because it is a value of the block it was
     * built in - which is the enclosing one, so the loop's blocks name it through the carry the
     * `Held` frames above registered.
     */
    builder.setBlock(*header);
    carry.arrive(builder, *header, 0);

    auto value = readPlace(builder, source, *builder.localPlace(*counter), inspectRequest());
    if(!value) return;

    auto test = !ascending ? BinaryOp::Ge : loop.inclusive ? BinaryOp::Le : BinaryOp::Lt;

    auto more = applyBinary(builder, source, test, *value, *to);
    if(!more) return;

    failIfNot(builder, *more, *exit, carry, source);

    /*
     * The body, with the loop variable bound to what the counter holds.
     *
     * Read again rather than threaded from the header: `failIfNot` carried on in a block of its
     * own, and a value names the block it was built in. Scoped to the body the way a `while`'s
     * pattern names are - the loop may run no times, so the name does not stand after it.
     */
    auto element = readPlace(builder, source, *builder.localPlace(*counter), inspectRequest());
    if(!element) return;

    auto named = loop.pat.kind == ast::Pat::Var;
    auto bound = builder.addLocal(named ? loop.pat.var : StringId(), valueOccurrence(*element),
                                  loop.pat.source, false);

    builder.addWrite(loop.pat.source, *builder.localPlace(*bound), *element, Destination::Init);

    auto scope = bindings.size();
    if(named) bindings.push(Binding { loop.pat.var, bound });

    {
        Loop frame(*this, &carry, *exit, *advance, builder.region(), false);
        resolveStatement(builder, loop.body);
    }

    bindings.resize(U32(scope));

    if(!builder.isTerminated()) {
        SmallArray<Operand, 8> onward;
        carry.edge(builder, onward);
        builder.addJump(loop.body.source, *advance, onward.pointer(), onward.size());
    }

    /*
     * The advance, and the step it guards.
     *
     * `to - i` ascending and `i - to` descending, each on the side the header proved is the larger,
     * so neither subtraction wraps. A closed range has another iteration while that distance holds
     * a step; a half-open one while it holds more than one, which is the whole of the difference
     * between `..` and `..=` in the emitted code.
     */
    builder.setBlock(*advance);
    carry.arrive(builder, *advance, 0);

    auto current = readPlace(builder, source, *builder.localPlace(*counter), inspectRequest());
    if(!current) return;

    if(guarded) {
        auto distance = applyBinary(builder, source, BinaryOp::Sub, ascending ? *to : *current,
                                    ascending ? *current : *to);
        if(!distance) return;

        // A half-open interval has another iteration while the distance is *more* than a stride and a
        // closed one while it holds one, which is the whole of the difference between `..` and `..=`
        // in the emitted code. `downto` is neither spelling's business: it excludes the bound written
        // first, so what it walks is the closed end.
        auto room = applyBinary(builder, source,
                                (ascending && !loop.inclusive) ? BinaryOp::Gt : BinaryOp::Ge,
                                *distance, *step);
        if(!room) return;

        failIfNot(builder, *room, *exit, carry, source);

        // `failIfNot` carried on in a block of its own, so the read above is a value of a block this
        // one no longer is. Read again rather than threaded: a load of a local is what every other
        // block does, and adding the counter to the carry would make the loop's own storage
        // something the enclosing expression thinks it is holding.
        current = readPlace(builder, source, *builder.localPlace(*counter), inspectRequest());
        if(!current) return;
    }

    auto next = applyBinary(builder, source, ascending ? BinaryOp::Add : BinaryOp::Sub, *current,
                            *step);
    if(!next) return;

    builder.addWrite(source, *builder.localPlace(*counter), *next, Destination::Assign);

    SmallArray<Operand, 8> back;
    carry.edge(builder, back);
    builder.addJump(source, *header, back.pointer(), back.size());

    builder.setBlock(*exit);
    carry.arrive(builder, *exit, 0);
}

/*
 * `return` - an ordinary one everywhere, including inside a continuation.
 *
 * It is Analysis-Lens.md §5.1's second exit out of a continuation and needs nothing here for that,
 * which is the point W-B rests on: the continuation is a region of *this* function's blocks, so a
 * `return` written in one is this function returning, and the drop elaboration on the way out is the
 * ordinary one on an ordinary edge. What the two lowerings make of that edge is the conversion's -
 * the CPS form turns it into an exit code, the embed form into a jump - and the difference is
 * invisible here, which is what "observationally identical" has to mean somewhere.
 *
 * A lens's own body is where the one special case is, and it is a *narrowing* rather than an
 * addition. A lens answers what its continuation answered, so it has no value of its own to write:
 * `return e` is refused, and a bare `return` answers with what the `yield` was handed - which is
 * what makes a `return` in the cleanup skip the rest of it, exactly as a `return` skips the rest of
 * any block.
 */
void Resolver::resolveReturn(Builder& builder, ast::Expr& expr) {
    auto& function = *currentFunction;

    /*
     * An iterator ends by running out of values, and what it hands back then is the loop's own
     * signal rather than anything the body has a name for - so there is no value a `return` could
     * carry and no bare one that would mean something either. Falling off the end is what "ran out"
     * is written as, which is `finish`.
     */
    if(function.iterator) {
        context.diagnostics.error(
            "%@ ends by running out of values rather than by `return` - what it answers is the loop's own signal, which is not something the body names"_v,
            expr.source, context.findName(function.name));
        builder.addUnreachable(expr.source);
        return;
    }

    if(function.lens) {
        if(expr.ret) {
            context.diagnostics.error(
                "%@ answers what its continuation answered, so `return` here carries no value"_v,
                expr.source, context.findName(function.name));
            builder.addUnreachable(expr.source);
            return;
        }

        // Before the `yield`, `$yielded` holds nothing and the path is one that did not hand over -
        // which `checkHandover` reports over the whole body rather than at this one statement,
        // because it is a fact about the path and not about the `return`.
        auto value = readPlace(builder, expr.source, *builder.localPlace(*yieldedLocal),
                               requestOf(function.result.contract));

        if(!value) {
            builder.addUnreachable(expr.source);
            return;
        }

        builder.addReturn(expr.source, *value);
        return;
    }

    if(!expr.ret) {
        if(decl()[function.result.type]->kind != Type::Unit) {
            context.diagnostics.error("this function answers %@, so `return` needs a value"_v,
                                      expr.source, typeText(context, decl(), function.result.type));
            builder.addUnreachable(expr.source);
            return;
        }

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

/*
 * A value that refers to somebody's storage, filling a position that does not say whose.
 *
 * `own.cpp` refuses the *declared* form of this - `data Outer {inner: Reader}`, "which refers to
 * storage the record does not name" - and this is the same sentence about a value. It exists because
 * a **closure** is the first thing in the language whose interior shape is decided at the expression
 * rather than at the declaration: `(I32) -> I32` is one type whether or not the value of it captured
 * anything, so a field or a parameter typed with it cannot say, and the value has to be asked.
 *
 * Only an **owned** position, which is the whole of the rule. A borrowed one is bounded by the call
 * it was granted for and may hold a reference to this frame for exactly that long - which is what
 * passing a callback *is*, and refusing it would refuse the feature. What an owned position claims
 * is that the value is now the receiver's, and a receiver that outlives this frame would then hold a
 * reference into a frame that is gone.
 */
bool Resolver::checkCarriedOrigins(Node& value, const Occurrence& declared, LocationId source,
                                   StringView what) {
    if(!declared.contract.isOwned()) return true;

    auto& occurrence = valueOccurrence(value);
    auto carried = shapeOf(decl(), occurrence);

    /*
     * The destination's own shape is what discharges it, which is what makes this one rule rather
     * than an exemption per position. A `let` mints its local from the value it binds, so the two
     * shapes are the same handle and a binding is accepted; a field, a `->` parameter and an
     * assignment to storage that already exists all have a shape somebody else declared, and a value
     * carrying more than it says is a reference the receiver was not told about.
     */
    if(Shapes::isEmpty(carried) || carried == shapeOf(decl(), declared)) return true;

    context.diagnostics.error(
        "%@ refers to storage this frame owns, and %@ has no origin to say whose - a value that outlives this frame needs a heap-placed environment, which the ownership resolver does not implement yet"_v,
        source, typeText(context, decl(), occurrence.type), what);
    return false;
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

/*
 * How many `yield`s a body writes, which is what decides whether each of them is guarded.
 *
 * Syntactic, and it can be: a departure resumes after the outermost loop the `yield` was in, so the
 * only `yield` a departed iterator can reach again is one written below that loop - and a body with
 * a single one has none. Counting is therefore an over-approximation of "can this be re-reached"
 * that is exact on every iterator anyone writes, where the precise question is a CFG walk over a
 * body that does not exist yet.
 */
static U32 countYields(ast::ParseBase parse, ast::Expr& expr) {
    U32 found = expr.kind == ast::Expr::Yield ? 1 : 0;

    ast::visitChildren(parse, expr, [&](ast::Expr& child) { found += countYields(parse, child); });
    return found;
}

void Resolver::resolveBody(Function& function, ast::Decl& declaration) {
    auto& written = declaration.fun;
    if(!written.body) return;

    Builder builder(context, program, function);
    currentFunction = &function;
    bindings.clear();
    continuationLocal = nullptr;
    yieldedLocal = nullptr;
    guardYields = false;
    handovers = 0;

    /*
     * The body is resolved in the signature's own context, **once**, against its own variables -
     * Implementation-Generics.md §1's first invariant.
     *
     * It is not re-read per specialization and it is not resolved under any concrete type: what a
     * specialization does is substitute into the decisions this walk made. That is why the context
     * is closed by now: a body may name the signature's variables and may not add to them.
     */
    GenScope generics(*this, function.gen);

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

        /*
         * The continuation, which is a parameter and not a name.
         *
         * `$body` is the desugaring's, not the author's, so binding its symbol would put a name in
         * scope that no source can say - the same rule a lifted lambda's environment parameter is
         * under, and for the same reason. `yield` reaches it through this field instead.
         */
        if(local->parameter == function.continuationArg) {
            continuationLocal = local;
            continue;
        }

        bindings.push(Binding { local->name, local });

        // The subject is the first parameter of a teardown - `Function::teardown` - and this is the
        // one function whose members are followed whether or not the body mentions them.
        if(function.teardown && local->parameter == 0) {
            nameLinearMembers(builder, *builder.localPlace(*local));
        }
    }

    /*
     * Where a `yield`'s answer is parked - `resolveYield`, and the storage a lens returns from.
     *
     * Made before the first statement rather than at the `yield`, because more than one path may
     * hand over (`if c: yield 1 else: yield 2`) and every one of them fills the same storage, which
     * is also what makes "twice on one path" a question `checkHandover` can ask of a write.
     */
    if(function.lens) {
        yieldedLocal = builder.addLocal(context.addUnqualifiedName("$yielded", 8), function.result,
                                        function.source, function.iterator);
    }

    auto& expr = *parse[written.body];

    /*
     * An iterator starts out having been asked for an element, which is the fall-out row.
     *
     * Written here rather than left to the `yield`, because it is what every path that never reaches
     * one answers with: an `iter fn` whose body runs out of values without handing over is a filter
     * that accepted nothing, and `finish` reads this storage on that path exactly as it does on the
     * one that departed. A lens needs no such write - `checkHandover` proves every path fills it.
     */
    if(function.iterator) {
        builder.addWrite(function.source, *builder.localPlace(*yieldedLocal),
                         *builder.addConst(function.source,
                                           intConstant(program.types.exitCode(), 0)),
                         Destination::Init);

        guardYields = countYields(parse, expr) > 1;
    }
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

    // Over the sealed shape of the body rather than as the walk goes, because both halves of the
    // rule are about paths and `finish` is what closed the ones that fall off the end.
    // An iterator hands over any number of times, so none of the rule applies to it - not even the
    // zero case, since an iterator that yields nothing is a filter that accepted nothing.
    if(function.lens && !function.iterator) checkHandover(builder, function, declaration.source);

    builder.seal();
    currentFunction = nullptr;
    continuationLocal = nullptr;
    yieldedLocal = nullptr;
}

/*
 * A class's **defaults** - the members it wrote a body for.
 *
 * Ordinary generic bodies, resolved once and in the class's own context, which is what makes a
 * default and a hand-written implementation the same kind of thing: an instance that leaves the
 * member out gets this body specialized at its type, exactly as any other generic function is. A
 * default may call the class's other members and its superclasses' on the same subject, and it may
 * do so because the context it is resolved in requires them - see `dischargeRequirements`.
 */
void Resolver::resolveClassBody(Class& klass, ast::Decl& declaration) {
    for(auto member: declaration.trait.decls.contents(parse)) {
        if(member.kind != ast::Decl::Fun || !member.fun.body) continue;

        auto index = classMemberIndex(klass, member.fun.name, U32(member.fun.args.size()));
        if(index == klass.members.size()) continue;

        resolveBody(*decl()[klass.members.get(decl(), U32(index))], member);
    }
}

/*
 * An instance's members' bodies - ordinary bodies, one per written member.
 *
 * The declaration walk already checked that each signature is the one the class declares, so what is
 * left is the same walk every other function gets. That it is the same walk is the point: a teardown
 * that ran through a path of its own would be a second set of rules for the code the first set
 * exists to place.
 *
 * Matched to the declarations by *name* rather than by position, because a member the walk rejected
 * has no function to resolve a body into and the two lists would then be one entry out of step.
 */
void Resolver::resolveInstanceBody(Instance& instance, ast::Decl& declaration) {
    auto& klass = *decl()[instance.klass];

    for(auto member: declaration.instance.decls.contents(parse)) {
        if(member.kind != ast::Decl::Fun) continue;

        auto index = classMemberIndex(klass, member.fun.name, U32(member.fun.args.size()));
        if(index == klass.members.size()) continue;

        auto pointer = instance.members.get(decl(), U32(index));
        if(pointer && !decl()[pointer]->klass) resolveBody(*decl()[pointer], member);
    }
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

        /*
         * A lens falling off the end answers what its continuation answered, which is the `yield`
         * form's desugaring read at its last line: `... yield e ... return $v`.
         *
         * Not a path that reaches the end without returning, therefore, and the rule it *is* subject
         * to - that it handed over first - is `checkHandover`'s over every path rather than this
         * block's alone.
         */
        /*
         * An iterator falling off the end answers the same storage, and for the same reason: what it
         * holds is what its continuation last said. Running out of values leaves the fall-out row
         * `resolveBody` wrote there, and a departure leaves the row the consumer sent - so "ran out"
         * and "was left" are one path here, which is what makes the trailing statements below a
         * `yield` ordinary code rather than a second exit.
         */
        if(yieldedLocal) {
            auto value = readPlace(builder, source, *builder.localPlace(*yieldedLocal),
                                   requestOf(function.result.contract));

            if(value) builder.addReturn(source, *value);
            else builder.addUnreachable(source);

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
    for(auto declaration: file.decls.contents(parse)) {
        if(declaration.kind == ast::Decl::Stmt) resolveGlobals(declaration, nextGlobalInit);
    }
}

void Resolver::resolveBodies(ast::Module& file) {

    for(auto declaration: file.decls.contents(parse)) {
        switch(declaration.kind) {
            case ast::Decl::Fun: {
                if(nextFunctionBody >= writtenFunctions.size()) break;

                // Null where the first pass rejected it - see `writtenFunctions`. Its body is not
                // resolved, because there is no signature to resolve it against and every name in it
                // would be reported a second time.
                auto pointer = writtenFunctions[nextFunctionBody++];
                if(pointer) resolveBody(*decl()[pointer], declaration);
                break;
            }
            case ast::Decl::Instance: {
                if(nextInstanceBody >= writtenInstances.size()) break;

                auto pointer = writtenInstances[nextInstanceBody++];
                if(pointer) resolveInstanceBody(*decl()[pointer], declaration);
                break;
            }
            case ast::Decl::Trait: {
                if(nextClassBody >= writtenClasses.size()) break;

                auto pointer = writtenClasses[nextClassBody++];
                if(pointer) resolveClassBody(*decl()[pointer], declaration);
                break;
            }
            default:
                break;
        }
    }
}

} // namespace mir
