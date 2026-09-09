#include "../parse/ast_visit.h"
#include "resolve_internal.h"
#include "name.h"
#include "own.h"
#include "intrinsic/link.h"

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
        case ast::Expr::MultiIf:
            resolveMultiIf(builder, expr, nullptr, false, inspectRequest());
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
    if(isTriviallyCopyableAt(decl(), occurrence)) {
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
/*
 * The attributes written in front of one binding - `@heap`, and nothing else today.
 *
 * A walk of its own rather than a case in `readAttributes`, because the position is different in the
 * one way that matters: a declaration's attributes are read while a *declaration* is being built and
 * these are read while a *statement* is, so what they can be about is a local rather than a function
 * or a type. `DeclAttribute::Heap` is still the shared spelling, which is what makes `@heap fn f()`
 * the ordinary misplacement message instead of an unknown attribute.
 *
 * Reporting an unknown one is the half this stage did not have at all: `VarDecl::attributes` was
 * parsed and dropped, so `@hep let x = 0` compiled and did nothing.
 */
void Resolver::readBindingAttributes(const ast::VarDecl& binding, Local* local, bool placed) {
    // Copied out first because `contents` is not const, which is the same step every other walk over
    // a parsed list of attributes makes.
    auto attributes = binding.attributes;

    for(auto attribute: attributes.contents(parse)) {
        auto which = findAttribute(stringView(context.findName(attribute.name)));

        if(which != DeclAttribute::Heap) {
            context.diagnostics.error("%@ is not an attribute a binding takes"_v, attribute.source,
                                      context.findName(attribute.name));
            continue;
        }

        if(attribute.args.size()) {
            context.diagnostics.error("`@heap` takes no arguments"_v, attribute.source);
            continue;
        }

        // A form this walk was not given a local for - a pattern, or an extent's binding. Neither is
        // one object, so the message below would be describing the wrong thing.
        if(!placed) {
            unsupported(attribute.source, "`@heap` on a binding that is not one name"_v);
            continue;
        }

        /*
         * A binding that owns nothing has no object to place - a borrow names storage somebody else
         * owns, and a name for a constant names none at all. Saying so beats minting an object so
         * that the attribute has something to be about, which is the shape the shipping stage's own
         * "nothing to place" message is in.
         */
        if(!local || !local->object.valid()) {
            context.diagnostics.error(
                "`@heap` has nothing to place - this binding names storage this frame does not own"_v,
                attribute.source);
            continue;
        }

        local->demandsHeap = true;
    }
}

/*
 * A local for a plain binding's resolved value - the flat form's `let x = e` and the scoped form's
 * `let x = e in ...` build exactly this, and used to build it twice: `@heap` was read after the
 * write in one copy and not the other, so the same attribute got a different answer depending on
 * which statement shape asked. One helper called from both is what keeps that from happening again -
 * whatever this sequence does next, both forms do it identically.
 *
 * `name` is `StringId()` for `_`, which is `addLocal`'s own spelling for a local with no name: the
 * storage a discarded value's teardown is owed still needs a place, and it is only the *name* that a
 * caller may have left out.
 */
Local* Resolver::bindPlainLocal(Builder& builder, const ast::VarDecl& binding, StringId name,
                                Node& value, bool writable) {
    /*
     * The binding's occurrence is the initializer's, whole.
     *
     * Not `owned(type)`: a loan bound to a name is a loan, and a local that recorded the type
     * and dropped the contract would be storage the ownership analysis believes this frame owns
     * - which is a teardown of the lender's value at the end of this one.
     */
    auto local = builder.addLocal(name, valueOccurrence(value), binding.pat.source, writable);

    /*
     * And the constant it was established with, where it was one - see `Local::initial`. Only
     * for a binding nothing may assign to, which is what makes the value it holds permanent.
     */
    if(!writable && value.kind == Node::Const) {
        local->initial = builder.body.constant(builder.base(), ((NodeConst&)value).constant);
    }

    auto place = builder.localPlace(*local);
    builder.addWrite(binding.pat.source, *place, value, Destination::Init);

    // After the write, because that is what unites the initializer's lineage into this local -
    // and the attribute is about the object rather than about the name.
    readBindingAttributes(binding, local, true);

    return local;
}

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
        // The grammar puts `in` inside the `=` branch, so the two cannot come apart - and a check
        // that relies on a production's shape is one that stops holding when the production changes.
        if(!binding.content) {
            context.diagnostics.error("a local requires an initializer"_v, binding.pat.source);
            continue;
        }

        // `resolveScoped` reads the binding's own attributes - it is also reached directly from a
        // value position (`resolve_expr.cpp`'s `Decl` case), which this statement form is not the
        // only door to, so the read has to live where both doors lead rather than behind one of them.
        if(binding.in) {
            resolveScoped(builder, binding, nullptr, false, inspectRequest());
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
                reportEmptyExtent(parse[binding.content]->source, *callee.function);
            }

            continue;
        }

        /*
         * A pattern, or a name with alternatives - the guard form, which is `resolveBinding`.
         *
         * Below both lens dispatches and not above them, which is the whole of what makes the
         * message right: the alternatives of a *skipping* call are `handoverCall`'s, checked there
         * against the callee's own `| E`, and a lens call that gets no further than the two reports
         * above has already been told the thing that is actually wrong with it. What is left here is
         * the other reading of the same grammar - a pattern with arms - and it is told from the
         * first by the initializer rather than by the `|`.
         *
         * The names go into the enclosing scope and stay there, which is the flat form's whole
         * point: an alternative has to leave the block, so the code below the declaration is reached
         * only where the pattern matched.
         */
        if(destructures(binding)) {
            readBindingAttributes(binding, nullptr, false);

            auto pivot = destructuringPivot(builder, binding);
            if(!pivot) continue;

            SmallArray<PatternBinding, 4> made;
            if(!resolveBinding(builder, binding, *pivot, made)) continue;

            bindPattern(builder, made);
            continue;
        }

        /*
         * The two halves of the sigil, and `let &->x` is the one that writes both.
         *
         * `VarDecl::sink` is `->` written beside a `&` - "the initializer is a destructive read of
         * a place that has another name, and the storage it lands in is writable through this
         * one" - so it decides the *access*, and `&` alone decides whether the local is assignable.
         * Reading `Ref` for the access without it asked the initializer for a mutable borrow, which
         * is a different request and refused of anything not declared `&`: `let &->acc = seed` in
         * `lib/Core/Join.yana` was "seed is not declared `&`, so it cannot be handed out for
         * writing" where the program asked to move it.
         */
        auto writable = binding.bind == ast::BindType::Ref;
        auto sunk = binding.bind == ast::BindType::Sink || binding.sink;
        auto access = sunk      ? Access::Owned
                    : writable  ? Access::Unique
                                : Access::Shared;

        auto value = resolveValue(builder, *parse[binding.content], nullptr,
                                  bindingRequest(access, writable));

        if(!value) {
            // The name is in scope from here whether or not it has storage - see `Binding`. Pushed
            // rather than returned on, so that `let a = broken, b = 1` still declares `b` and a
            // later read of `a` is silent instead of reported a second time.
            bindings.push(Binding { binding.pat.var, nullptr });
            continue;
        }

        auto local = bindPlainLocal(builder, binding, binding.pat.var, *value, writable);

        // In scope from *after* its own initializer, which is what makes `let x = x` read the outer
        // `x` rather than itself.
        bindings.push(Binding { binding.pat.var, local });
    }
}

/*
 * Every name this expression reads, each once.
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
 *
 * The *list* of names rather than a yes to a list of names, because `flatExtent` asks about every
 * statement after the call and used to walk each one again per round of its fixpoint with a linear
 * scan of the tracked names inside the walk - rounds times statements times names. Walked once per
 * statement here and answered from a set there.
 */
static void mentioned(ast::ParseBase parse, ast::Expr& expr, HashMap<U32, U8>& seen,
                      SmallArray<StringId, 8>& into) {
    if(expr.kind == ast::Expr::Var) {
        auto entry = seen.add(U32(expr.var));
        if(!entry.existed) into.push(expr.var);
        return;
    }

    if(expr.kind == ast::Expr::Field) return mentioned(parse, parse[expr.field]->target, seen, into);

    ast::visitChildren(parse, expr, [&](ast::Expr& child) {
        mentioned(parse, child, seen, into);
    });
}

/*
 * §11.3 - a flat call whose extent covers nothing.
 *
 * A hand-over with no live use is destroyed at the call, and there is no reading of that which was
 * intended: the author meant the scoped form, or meant a plain function. Stated over the *extent*
 * rather than over the result type, so that one rule covers `withLock(m)` written flat, a `let` of a
 * value nobody goes on to use, and a body that is one statement and has nothing after it at all.
 *
 * §7.2's exception - a skipping lens called for its alternative alone - is not one this rule has to
 * make room for. That shape hands over *nothing*, so what it wants is `f(...) | else -> ...:` with a
 * block, and the extent is then written rather than derived; an empty extent here is still a call
 * whose hand-over nobody reads, whether or not it can fail.
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
 * The set and the end grow together and each can reveal the other: a statement skipped as outside
 * the extent is inside it once a later statement extends the end, and the names it binds are then
 * tracked too. That used to be a fixpoint of passes over the block, each pass walking every
 * remaining statement against a list of names - rounds times statements times names, cubic in a
 * chain that reveals one earlier binding per round, and "a block is small" is not a thing a
 * generated body or an inlined one promises. It is one pass now, and the reason one is enough is
 * the direction things move in: a statement mentioning a tracked name moves the end *forward* to
 * itself, everything between the old end and the new one is then inside and binds into the set, and
 * every statement those bindings could matter to is past the new end and has not been visited yet.
 * A statement before the new end that reads one of them was inside already. So each statement is
 * walked once for the names it reads, its binding is taken once, and the set is a hash set.
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

    HashMap<U32, U8> tracked;
    auto track = [&](StringId name) { (void)tracked.add(U32(name)); };
    track(handed);

    // What a statement binds for the block after it - null where it binds nothing. A `let ... in`
    // scopes its name to its own body, so it binds nothing this block goes on to see, and a name
    // that leaves no trace after the statement cannot be what a later one is reading.
    auto bound = [&](ast::Expr& statement, SmallArray<StringId, 4>& into) {
        if(statement.kind != ast::Expr::Decl) return;

        for(auto binding: statement.decl.contents(parse)) {
            if(binding.in || binding.pat.kind != ast::Pat::Var) continue;
            into.push(binding.pat.var);
        }
    };

    auto last = at;

    for(U32 index = at + 1; index < count; index++) {
        auto statement = statements.get(parse, index);

        HashMap<U32, U8> seen;
        SmallArray<StringId, 8> names;
        mentioned(parse, statement, seen, names);

        auto reads = false;
        for(auto name: names) reads |= tracked.get(U32(name)).isJust();
        if(!reads) continue;

        // The end moves here, and everything between the old end and this statement is inside the
        // extent now: what each of them binds is the continuation's.
        for(U32 inside = last + 1; inside <= index; inside++) {
            SmallArray<StringId, 4> binds;
            auto within = statements.get(parse, inside);
            bound(within, binds);
            for(auto name: binds) track(name);
        }

        last = index;
    }

    return last;
}

/*
 * The function a call names, written plainly or with a dot, and nothing for everything else.
 *
 * Only a *named* one, and that is what both callers are asking: a callee reached through a binding
 * of function type is the erased callback ABI, which is milestone 6's. What a `for` and a lens call
 * have in common is that they decide *what kind of construct they are* before resolving anything, so
 * the lookup is here rather than in `resolveCall`, which resolves before asking.
 *
 * `x.f(...)` answers the same way with the receiver carried back rather than resolved - see
 * `NamedCall`, and the ordering note below.
 */
NamedCall Resolver::namedCallee(ast::Expr& expression, NamedCallSite* site) {
    if(expression.kind != ast::Expr::App) return NamedCall();

    auto& application = *parse[expression.app];

    StringId name;
    ast::FieldExpr* dotted = nullptr;
    auto arity = U32(application.args.size());

    if(application.callee.kind == ast::Expr::Field) {
        /*
         * `for x in xs.items()` - the dot form, whose name is asked **before** its receiver exists.
         *
         * The order is load-bearing rather than an economy. `upTo(n).twice()` names nothing, and
         * resolving its receiver to discover that would emit the receiver and then report "upTo is
         * an iterator, write it as the source of a `for` loop" - which is what the author did
         * write. So the name decides whether there is a traversal here at all, and only then does
         * `handoverCall` resolve the receiver into the caller's block.
         */
        dotted = parse[application.callee.field];
        if(dotted->field.kind != ast::Expr::Var) return NamedCall();

        name = dotted->field.var;
        arity += 1;
    } else {
        if(application.callee.kind != ast::Expr::Var || findLocal(application.callee.var)) {
            return NamedCall();
        }

        name = application.callee.var;
    }

    // What the call site said, before any lookup - `NamedCallSite`, and `iteratorCallee`, which is
    // the one caller that needs the question asked one step earlier than a declaration.
    if(site) *site = NamedCallSite { name, arity, dotted };

    // A probe rather than a decision - the caller is asking whether this name is a lens, and the
    // ordinary call path reports if it is not. `report` is false for that reason: an ambiguity has
    // to be said once, where the call is resolved, rather than here and there.
    auto found = mir::findFunction(context, program, here(), name, arity, expression.source, false);
    if(!found) return NamedCall();

    return NamedCall { decl()[found], dotted };
}

// See the declaration. R5 over the iterators of one name, and the ordinary lookup where the filter
// leaves exactly one - a set whose only traversal is a class member is not a choice, it is an answer
// `findFunction` could not give because a plain function of the name was standing beside it.
NamedCall Resolver::iteratorCallee(Builder& builder, ast::Expr& expression,
                                   const NamedCallSite& site, OverloadChoice& into) {
    OverloadSet set;
    mir::findOverloads(context, program, here(), site.name, site.arity, expression.source, set,
                       false);

    DeclPtr<Function> sole = nullptr;
    U32 traversals = 0;

    for(Size i = 0; i < set.size(); i++) {
        if(!decl()[set.at(i)]->iterator) continue;
        if(!traversals++) sole = set.at(i);
    }

    if(!traversals) return NamedCall();
    if(traversals == 1) return NamedCall { decl()[sole], site.dotted };

    /*
     * Several, so the arguments decide - and the **dot** form is refused rather than selected.
     *
     * A receiver is argument zero of the mapping and is resolved before a candidate is known, so
     * selecting one needs the receiver resolved *here* and `handoverCall` then not resolving it
     * again. That is a second seam through the same walk for a spelling no candidate needs today;
     * `xs.chunks()` reaches the one-iterator path above unchanged, and only a name that is several
     * *and* written with a dot arrives here.
     */
    if(site.dotted) {
        context.diagnostics.error(
            "%@ names several iterators, and a receiver written with a dot is resolved before one of them is chosen - write it as %@(...)"_v,
            expression.source, context.findName(site.name), context.findName(site.name));
        return NamedCall();
    }

    if(!selectOverload(builder, *parse[expression.app], set, site.name, expression.source,
                       expression.source, Receiver(), CandidateKind::Iterator, into)) {
        return NamedCall();
    }

    return NamedCall { decl()[into.function], nullptr };
}

/*
 * ... and null unless it is a `lens fn`.
 *
 * A name that turns out not to be a lens falls through to the ordinary statement it was written in
 * rather than being reported here, which is what keeps one diagnostic per mistake. A `for` asks the
 * question the other way round - see `resolveIteratorFor` - because a call in that position has no
 * other reading to fall through to.
 */
NamedCall Resolver::lensCallee(ast::Expr& expression) {
    auto callee = namedCallee(expression);
    return callee && callee->lens ? callee : NamedCall();
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
         * would give one statement two of them. The other shapes a `let` can have - a destructuring
         * pattern, a missing initializer - are `resolveDeclaration`'s to report, and are left to it
         * by falling through.
         *
         * **Alternatives are not one of those shapes.** They were, when no lens could fail and a
         * `|` beside a `let` could only be the match form; a skipping call writes them here and the
         * two spellings are one construct, so a flat call carries them exactly as the scoped one
         * does. What still falls through is a `|` beside an initializer that is not a lens call,
         * which `lensCallee` answers below and `resolveDeclaration` reports.
         */
        if(statement.kind == ast::Expr::Decl && statement.decl.size() == 1) {
            auto& binding = *statement.decl.pointer(parse);

            if(!binding.in && binding.content &&
               (binding.pat.kind == ast::Pat::Var || binding.pat.kind == ast::Pat::Any)) {
                auto& initializer = *parse[binding.content];

                if(auto callee = lensCallee(initializer)) {
                    index = resolveFlatLensCall(builder, statements, to, index, binding, initializer,
                                                callee, wantValue);
                    continue;
                }
            }
        }

        /*
         * A bare `f(...)` - the flat form of a lens that hands nothing over, which is what a lock is
         * written as. Asked after the `let` above so that a binding still decides the extent where
         * there is one, and before `resolveStatement`, which has no block to split.
         */
        if(auto callee = lensCallee(statement)) {
            index = resolveBareLensCall(builder, statements, to, index, statement, callee,
                                        wantValue);
            continue;
        }

        if(wantValue && index + 1 == to) return resolveValue(builder, statement, hint, wanted);

        resolveStatement(builder, statement);
    }

    return nullptr;
}

/*
 * See the declaration. Whether a call of this name leaves one of its arguments unevaluated.
 *
 * Asked before an overload has been chosen, so it is asked of the *set*: if any declaration at this
 * arity defers a position, no call written with this name offers a hoist. Conservative in the one
 * direction it can afford to be, because all that turns on the answer is whether the lift is
 * offered - the ordinary spelling still reaches every eager position.
 *
 * `&&` and `||` are what it is for. Their right operand is a thunk the expansion runs under a
 * branch, so a lens lifted out of one would run where the program does not, and would take the rest
 * of the block as its extent on a path that was never going to reach it.
 */
bool Resolver::defersArguments(ast::Expr& callee, U32 arity) {
    auto name = &callee;
    while(name->kind == ast::Expr::Nested) name = parse[name->nested];
    if(name->kind != ast::Expr::Var) return false;

    OverloadSet set;
    findOverloads(context, program, here(), name->var, arity, name->source, set, false);

    for(Size i = 0; i < set.size(); i++) {
        for(auto argPointer: decl()[set.at(i)]->args.contents(decl())) {
            if(decl()[argPointer]->lazy) return true;
        }
    }

    return false;
}

// See the declaration. Evaluation order, stopping at the first call a hoist would move.
ast::Expr* Resolver::hoistableLensCall(ast::Expr& expr, bool root) {
    auto through = [&](ast::ParsePtr<ast::Expr> held) -> ast::Expr* {
        return held ? hoistableLensCall(*parse[held], false) : nullptr;
    };

    auto arguments = [&](ast::ParseList<ast::TupArg> args) -> ast::Expr* {
        auto written = args.contents(parse);

        for(Size i = 0; i < written.size(); i++) {
            if(auto found = hoistableLensCall(written.pointerAt(i)->value, false)) return found;
        }

        return nullptr;
    };

    switch(expr.kind) {
        case ast::Expr::Nested:
            return through(expr.nested);
        case ast::Expr::Ret:
            return through(expr.ret);
        case ast::Expr::Yield:
            return through(expr.yield);
        /*
         * `g(h())`: a call is itself a candidate (`lensCallee` below), but it is also a position
         * whose callee and arguments run before it does, and either may hold a lens call of its
         * own - `h()` here. Those are asked first and win when found, since they evaluate earlier
         * than the call they are nested in; only once neither turns up anything does `expr` get to
         * match itself. Checking self first, as this used to, would hoist `g(h())` whole and leave
         * `h()` behind as a plain argument for `checkCallable` to refuse.
         */
        case ast::Expr::App:
        case ast::Expr::Sub: {
            auto application = parse[expr.app];
            if(auto found = hoistableLensCall(application->callee, false)) return found;

            if(!defersArguments(application->callee, U32(application->args.size()))) {
                if(auto found = arguments(application->args)) return found;
            }

            return !root && lensCallee(expr) ? &expr : nullptr;
        }
        /*
         * `a + b` and `-x`, whose operands are ordinary positions - and the one thing that keeps
         * them from being quite ordinary.
         *
         * The parser nests an infix chain to the right without regard for precedence and the
         * operator walk re-associates it, so where an operand stands in the *written* chain is all
         * this can read. That is enough, because re-association regroups the operands without
         * reordering them: everything before the first operator that defers is evaluated whatever
         * the grouping turns out to be, and everything after it may land inside that operator's
         * thunk. So the walk runs left to right and stops there.
         */
        case ast::Expr::Infix: {
            auto infix = parse[expr.infix];
            if(auto found = hoistableLensCall(infix->lhs, false)) return found;
            if(defersArguments(infix->op, 2)) return nullptr;

            return hoistableLensCall(infix->rhs, false);
        }
        case ast::Expr::Prefix: {
            auto prefix = parse[expr.prefix];
            if(defersArguments(prefix->op, 1)) return nullptr;

            return hoistableLensCall(prefix->on, false);
        }
        case ast::Expr::Con:
            return arguments(parse[expr.con]->args);
        case ast::Expr::Tup:
            return arguments(expr.tup);
        case ast::Expr::Field:
            return hoistableLensCall(parse[expr.field]->target, false);
        case ast::Expr::Coerce:
            return hoistableLensCall(parse[expr.coerce]->target, false);
        case ast::Expr::Assign:
            return hoistableLensCall(parse[expr.assign]->value, false);
        case ast::Expr::Is:
            return hoistableLensCall(parse[expr.is]->value, false);
        case ast::Expr::Array: {
            auto items = expr.arr.contents(parse);

            for(Size i = 0; i < items.size(); i++) {
                if(auto found = hoistableLensCall(*items.pointerAt(i), false)) return found;
            }

            return nullptr;
        }
        case ast::Expr::Format: {
            auto chunks = expr.format.contents(parse);

            for(Size i = 0; i < chunks.size(); i++) {
                if(auto found = through(chunks.pointerAt(i)->format)) return found;
            }

            return nullptr;
        }
        case ast::Expr::Decl: {
            /*
             * A declaration's initializer, and the exclusion that makes this terminate: the
             * initializer that *is* the call is the flat form's own case, so hoisting it would
             * rewrite `let x = trim(y)` into `let $l = trim(y); let x = $l` forever.
             *
             * Only for the shape that form accepts, which is where the two differ. A flat call has
             * to *be* the statement - anything else falls through to an ordinary expression, and a
             * lens call inside one of those is a subexpression like any other. A `let` that wrote an
             * `in` is exempt wherever it stands, because the exemption is not about position: it
             * named its extent, and lifting the call out would silently give it the rest of the
             * enclosing block instead.
             */
            auto declarations = expr.decl.contents(parse);

            /*
             * A statement binding **several** names offers nothing, and that is not caution: the
             * extent belongs to the statement, so a lens lifted out of `let a = 1, n = f(x)` would
             * be given an extent covering the other binding's initializer as well - and the form is
             * already refused by name, which is a better report than a rewrite that changes what the
             * statement means. `where` blocks are this shape.
             */
            if(declarations.size() != 1) return nullptr;

            auto declaration = declarations.pointerAt(0);
            if(!declaration->content) return nullptr;

            return hoistableLensCall(*parse[declaration->content], root || declaration->in);
        }
        default:
            return nullptr;
    }
}

void Resolver::hoistBlockLenses(ast::Expr& block) {
    if(block.kind != ast::Expr::Multi) return;

    // The size is re-read every turn: a statement inserted here is examined in its own right, which
    // is what lifts the second lens call written in one statement.
    for(Size i = 0; i < block.multi.size(); i++) hoistLensCall(block, i);
}

bool Resolver::hoistLensCall(ast::Expr& block, Size index) {
    auto statements = block.multi.contents(parse);
    if(index >= statements.size()) return false;

    auto site = hoistableLensCall(*statements.pointerAt(index), true);
    if(!site) return false;

    auto& arena = context.parseRegion;

    /*
     * The call moved out, and the position it was written in left naming the binding.
     *
     * The node is rewritten *in place* rather than replaced, because what points at it is whatever
     * held it - an argument list, a format hole, a field target - and none of those are reachable
     * from here. A copy of the call goes into the declaration; the original becomes the `Var` that
     * reads it, at the same source location, so every diagnostic about the value still points at
     * the call the author wrote.
     */
    auto moved = new (arena) ast::Expr(*site);

    char digits[12];
    Size written = 0;

    for(auto counter = hoistedLenses++;;) {
        digits[written++] = char('0' + counter % 10);
        counter /= 10;
        if(!counter) break;
    }

    /*
     * The bytes are the region's and not this frame's, and that is what makes the name usable after
     * this returns: `addUnqualifiedName` interns the pointer it is handed rather than a copy, so a
     * name built in a local buffer dangles the moment the frame goes.
     */
    auto length = written + 5;
    auto text = (char*)arena.alloc(length);

    text[0] = '$'; text[1] = 'l'; text[2] = 'e'; text[3] = 'n'; text[4] = 's';
    for(Size i = 0; i < written; i++) text[5 + i] = digits[written - i - 1];

    auto name = context.addUnqualifiedName(text, length);

    site->kind = ast::Expr::Var;
    site->var = name;

    ast::VarDecl declaration {};
    declaration.pat.kind = ast::Pat::Var;
    declaration.pat.var = name;
    declaration.pat.source = site->source;
    declaration.content = moved - *arena;
    declaration.bind = ast::BindType::Borrow;

    ast::Expr let {};
    let.kind = ast::Expr::Decl;
    let.source = site->source;
    let.decl = ast::ParseList<ast::VarDecl>();
    let.decl.push(arena, declaration);

    // `SmallList` grows at the end and has no insert, so the entry is pushed and then walked back
    // into place - one move per following statement, once per hoisted call.
    auto& list = block.multi;
    auto count = list.size();
    list.push(arena, let);

    auto entries = list.contents(parse);
    for(Size i = count; i > index; i--) *entries.pointerAt(i) = *entries.pointerAt(i - 1);

    *entries.pointerAt(index) = let;
    return true;
}

/*
 * The bare form - see the declaration. §11.2's extent rule with nothing handed over.
 *
 * The extent is **the rest of the block**, and it is not a guess: `flatExtent` bounds a `let` by the
 * last use of what was bound, and a call that binds nothing has no last use to be bounded by. What
 * the author wrote is a lock, a directory, a signal mask - something that holds while the code below
 * it runs - so "below it" is the whole of the statement's meaning, and a shorter extent would be a
 * lock released at a line nothing marks.
 *
 * §11.3's empty extent is still an error, and it is the same error: a bare call at the *end* of its
 * block splits nothing, so what it acquires it releases immediately.
 *
 * The binding handed to `resolveLensCall` is `_` and not a name, because that is exactly what this
 * form is - `let _ = f(...)` with the `let` left out, which is the reading the two share and the
 * reason there is one function under them.
 */
U32 Resolver::resolveBareLensCall(Builder& builder, ast::ParseList<ast::Expr> statements, U32 to,
                                  U32 at, ast::Expr& call, const NamedCall& callee,
                                  bool wantValue) {
    if(at + 1 == to) {
        reportEmptyExtent(call.source, *callee.function);
        return at;
    }

    // The block's own value, produced inside the extent - `resolveFlatLensCall`'s refusal, for the
    // same reason: what a continuation answers with is `$r`, which the resume channel fills.
    if(wantValue) {
        unsupported(call.source, "a lens call whose extent covers the value of its block"_v);
        return at;
    }

    ast::VarDecl binding {};
    binding.pat.kind = ast::Pat::Any;
    binding.pat.source = call.source;
    binding.bind = ast::BindType::Borrow;

    Extent extent;
    extent.statements = statements;
    extent.from = at + 1;
    extent.to = to;

    resolveLensCall(builder, binding, call, callee, extent);
    return to - 1;
}

/*
 * The flat form, once the callee is known - Analysis-Lens.md §11.2 and §11.3.
 *
 * Answers the index of the last statement it consumed, so that the walk carries on with the
 * caller's own code after the extent rather than resolving the continuation a second time.
 */
U32 Resolver::resolveFlatLensCall(Builder& builder, ast::ParseList<ast::Expr> statements, U32 to,
                                  U32 at, const ast::VarDecl& binding, ast::Expr& call,
                                  const NamedCall& callee, bool wantValue) {
    auto named = binding.pat.kind == ast::Pat::Var;
    auto last = flatExtent(statements, to, at, named ? binding.pat.var : StringId());

    // §11.3 - an extent that covers nothing, which `reportEmptyExtent` states the rule of.
    if(last == at) {
        reportEmptyExtent(call.source, *callee.function);
        return at;
    }

    /*
     * The block's own value, inside the extent.
     *
     * What a continuation answers with is `$r`, and today that is the exit discriminant alone
     * (`declareContinuation`) - so a block whose value is produced inside one has nowhere to put it.
     * Refused by name rather than mis-scoped: `$r` is where this goes, and what fills it is the
     * resume channel `<- R`, which is the same wrapper `| E` builds turned round.
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
Node* Resolver::resolveScoped(Builder& builder, const ast::VarDecl& binding, DeclPtr<Type> hint,
                              bool wantValue, Request wanted) {
    auto& initializer = *parse[binding.content];

    // A call of a `lens fn`, which is what makes the `in` body a continuation. Anything else - a
    // lens reached indirectly included, see `lensCallee` - is the ordinary scoped binding below.
    if(auto callee = lensCallee(initializer)) {
        /*
         * The `in` body of a lens call is the *callee's* continuation, so what it answers is
         * answered inside the region rather than here - which is the same sentence
         * `resolveFlatLensCall` makes about a flat call whose extent reaches the end of its block,
         * and it has the same one cause: what a continuation answers with is `$r`, and a value
         * produced inside one has nowhere else to go.
         */
        if(wantValue) {
            unsupported(initializer.source,
                        "a lens call whose extent covers the value of its block"_v);
            return nullptr;
        }

        resolveLensCall(builder, binding, initializer, callee, Extent { parse[binding.in] });
        return nullptr;
    }

    /*
     * The names, and then the body they are in scope for.
     *
     * Deliberately not a block. A `let ... in` introduces its names over one statement and nothing
     * else - it opens no scope for the body's own declarations to be trapped in, because the body is
     * resolved as a statement and a statement that declares does so in the enclosing scope. What is
     * scoped here is exactly what this declaration bound, which is the `resize` at the end.
     */
    auto scope = bindings.size();

    if(destructures(binding)) {
        // Not one object, so `@heap` would be describing the wrong thing - the same refusal the
        // flat form's own destructuring branch gives.
        readBindingAttributes(binding, nullptr, false);

        /*
         * A pattern, which is the guard form with a scope written on it - `resolveBinding`.
         *
         * The alternatives still have to leave the block rather than the `in` body, and that is not
         * a weaker rule than the flat form's: what they leave is the enclosing block either way, and
         * the `in` only shortens how long the names the pattern bound are visible for.
         */
        auto pivot = destructuringPivot(builder, binding);
        if(!pivot) return nullptr;

        SmallArray<PatternBinding, 4> made;
        if(!resolveBinding(builder, binding, *pivot, made)) return nullptr;

        bindPattern(builder, made);
    } else {
        // The same two halves the flat form reads above, where `let &->x` is argued.
        auto writable = binding.bind == ast::BindType::Ref;
        auto sunk = binding.bind == ast::BindType::Sink || binding.sink;
        auto access = sunk      ? Access::Owned
                    : writable  ? Access::Unique
                                : Access::Shared;

        auto value = resolveValue(builder, initializer, nullptr, bindingRequest(access, writable));
        if(!value) return nullptr;

        // Storage even for `_`, because an owner lives in a place: the value is evaluated and its
        // teardown is owed, and a local is what gives the drop something to name. What `_` skips is
        // the *name*, which is the whole of what it says.
        auto named = binding.pat.kind == ast::Pat::Var;
        auto local = bindPlainLocal(builder, binding, named ? binding.pat.var : StringId(),
                                    *value, writable);

        if(named) bindings.push(Binding { binding.pat.var, local });
    }

    auto& body = *parse[binding.in];
    Node* result = nullptr;

    if(wantValue) result = resolveValue(builder, body, hint, wanted);
    else resolveStatement(builder, body);

    bindings.resize(U32(scope));
    return result;
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
                               const NamedCall& callee, const Extent& extent) {
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
            binding.pat.source, context.findName(callee->name));
        return false;
    }

    auto named = binding.pat.kind == ast::Pat::Var;

    return handoverCall(builder, call, callee, Continuation::Once,
                        named ? binding.pat.var : StringId(), binding.pat.source, binding.alts,
                        [&](ContinuationId, Block&, Local&) { resolveExtent(builder, extent); });
}

/*
 * The alternatives a call site wrote against the failure channel the callee declared.
 *
 * Both directions, because both are mistakes the author can make and neither is one the other's
 * message describes. A missing alternative is a traversal whose skip path has nowhere to go; a
 * written one against a traversal that cannot fail is code that can never run, and saying so is
 * better than compiling it into a block nothing enters.
 *
 * One alternative and not a list. What the skip carries is the failure type and nothing else - the
 * continuation did not run, so nothing of its shape exists on that path - which makes the pattern
 * irrefutable and a second arm unreachable by construction. Analysis-Lens.md's V3 note records the
 * same conclusion reached from the other end.
 */
bool Resolver::checkAlternatives(Function& callee, ast::ParseList<ast::Alt> alternatives,
                                 LocationId source) {
    if(!callee.failure) {
        if(!alternatives.size()) return true;

        context.diagnostics.error(
            "%@ cannot fail, so there is nothing for an alternative to handle - a traversal that can says so with `| E` in its result"_v,
            source, context.findName(callee.name));
        return false;
    }

    if(!alternatives.size()) {
        context.diagnostics.error(
            "%@ can fail with %@, so this call needs `| reason -> ...` for the path where it never hands over"_v,
            source, context.findName(callee.name),
            typeText(context, decl(), callee.failure));
        return false;
    }

    if(alternatives.size() > 1) {
        context.diagnostics.error(
            "a call of %@ takes one alternative - what a failure carries is %@ and nothing else, so the first one already matches everything"_v,
            source, context.findName(callee.name),
            typeText(context, decl(), callee.failure));
        return false;
    }

    auto only = alternatives.get(parse, 0);
    if(only.pat.kind != ast::Pat::Var && only.pat.kind != ast::Pat::Any) {
        unsupported(only.pat.source, "destructuring what a traversal failed with"_v);
        return false;
    }

    return true;
}

// The alternative's own code, with the reason bound. A statement and not a value: the block below
// the call is what produces the block's value, and this arm reaches it by falling through to the
// join exactly as the continuation does.
void Resolver::resolveAlternative(Builder& builder, ast::Alt& alternative, Node& reason) {
    auto named = alternative.pat.kind == ast::Pat::Var;
    auto local = builder.addLocal(named ? alternative.pat.var : StringId(),
                                  valueOccurrence(reason), alternative.pat.source, false);

    builder.addWrite(alternative.pat.source, *builder.localPlace(*local), reason, Destination::Init);

    auto scope = bindings.size();
    if(named) bindings.push(Binding { alternative.pat.var, local });

    resolveStatement(builder, alternative.expr);

    bindings.resize(U32(scope));
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
bool Resolver::handoverCall(Builder& builder, ast::Expr& call, const NamedCall& called,
                            Continuation::Multiplicity multiplicity, StringId name,
                            LocationId nameSource, ast::ParseList<ast::Alt> alternatives,
                            F&& inside, OverloadChoice* chosen) {
    auto& written = *parse[call.app];
    auto source = call.source;
    auto& callee = *called.function;

    /*
     * The mapping, which a selection has already made where there was one to make.
     *
     * `checkCallShape` is what builds it, and it is exactly what `selectOverload` ran silently over
     * every candidate - so re-running it on the winner would be the same walk twice and, at a
     * candidate whose shape only just admitted the call, a second chance to report it.
     */
    ArgMap own;
    if(!chosen && !checkCallShape(written, callee, source, own, true, called.leading(),
                                  called.dotted ? called.dotted->target.source : kNullLocation)) {
        return false;
    }

    const ArgMap& map = chosen ? chosen->map : own;
    if(!checkAlternatives(callee, alternatives, source)) return false;

    /*
     * `xs.items()` - the receiver, resolved here because here is where the caller's block is.
     *
     * After the shape check and before the arguments, which is exactly where a written first
     * argument would have been: the receiver is argument 0 of the same mapping, so evaluation stays
     * left to right and `resolveArguments` reads it at whatever the declared position asks for.
     */
    Receiver from;

    if(called.dotted) {
        from.place = projectionTarget(builder, called.dotted->target, Access::Shared);
        if(!from.place) return false;

        from.source = called.dotted->target.source;

        /*
         * The same choice `resolveDotCall` makes, asked here because the receiver only exists now:
         * a field wins over a dot-call.
         *
         * Where it wins there is nothing for *this* form to be. A field holds a function value, and
         * a value is never a traversal - reaching a lens or an iterator through one is the erased
         * callback ABI - so the message says that rather than calling through the field and handing
         * it a continuation it has no parameter for.
         */
        if(hasMemberNamed(from.place->occurrence, called.dotted->field.var)) {
            context.diagnostics.error(
                "%@ is a field of %@, so this calls what the field holds - and a value is never a traversal, which is what this position needs"_v,
                called.dotted->field.source, context.findName(called.dotted->field.var),
                typeText(context, decl(), from.place->occurrence.type));
            return false;
        }
    }

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

    if(chosen) {
        /*
         * Already resolved, by the selection that needed them to choose - `prepareResolvedArguments`,
         * the same loop `finishOverloadedCall` runs. Walking them again here would evaluate the
         * call's arguments twice, which is the whole reason the choice carries its values.
         */
        typeArguments.unifies = true;
        for(U32 i = 0; i < map.count(); i++) arguments.push(nullptr);

        HeldList<SmallArray<Node*, 8>> held(*this, arguments);
        if(!prepareResolvedArguments(builder, written, callee, map, source, chosen->values,
                                     arguments, typeArguments, from)) {
            return false;
        }
    } else if(!resolveArguments(builder, written, callee, map, source, arguments, typeArguments,
                                nullptr, from)) {
        return false;
    }

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
    typeArguments.bind(callee.continuationResult.type, program.types.exitCode());
    typeArguments.fillDetermined(source);

    if(typeArguments.reportUndecided(source, "argument"_v,
                                     Tritium::format("call to %@", context.findName(callee.name)))) {
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
     * the marker means. `Many` is an iterator, whose body runs once per element. `AtMostOnce` is a
     * *skipping* lens, which is the multiplicity this field was declared for: the caller's own CFG
     * shows why, because the skip edge below reaches the join without passing through the region.
     */
    if(callee.failure && multiplicity == Continuation::Once) multiplicity = Continuation::AtMostOnce;

    auto with = typeArguments.with();

    /*
     * Where the caller resumes when the callee failed, and what it failed with.
     *
     * Made **before** the region, beside the join and for the same reason: it is a block of the
     * *enclosing* region, and that is the whole content of the skip - control never entered the
     * continuation, so there is nothing of it to be inside. What it failed with arrives as this
     * block's one parameter, produced inside the call and named by no operand at the call.
     */
    Block* skipped = nullptr;
    Node* reason = nullptr;

    if(callee.failure) {
        skipped = builder.addBlock(StringId());
        reason = builder.addBlockArgument(*skipped,
                                          owned(substituteType(context, program, callee.failure, with)),
                                          source);
    }

    auto region = builder.enterRegion(Continuation::Handover, multiplicity);
    auto entry = builder.addBlock(StringId());

    // Nothing the enclosing expression is holding crosses into the region - `Resolver::heldFloor`,
    // where the reason is written. Restored below, immediately after `leaveRegion`, so that the
    // alternative and the join carry what they always did.
    auto outerHeld = heldFloor;
    heldFloor = held;

    if(skipped) builder.body.region(builder.base(), region)->otherwise = skipped->index;

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

    builder.addLensCall(source, callee, region, *entry, arguments.pointer(), arguments.size(), with,
                        skipped);
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

    inside(region, *join, *local);
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
    heldFloor = outerHeld;

    if(fellOut) builder.addJump(source, *join, nullptr, 0);

    /*
     * The alternative, in the caller's own region and reaching the join the same way the
     * continuation does.
     *
     * After `leaveRegion`, because this arm is not inside the continuation - it is what runs when
     * there was no continuation. What it produces is nothing: the block below the call is the same
     * block either way, so the two arms join at the *statement* rather than at a value, which is
     * what makes the arm writable at all (Analysis-Lens.md's V3 note, from its other end).
     */
    if(skipped) {
        builder.setBlock(*skipped);
        auto alternative = alternatives.get(parse, 0);
        resolveAlternative(builder, alternative, *reason);
        if(!builder.isTerminated()) builder.addJump(source, *join, nullptr, 0);
    }

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
    auto wanted = requestOf(handover);

    Node* value = nullptr;

    if(expr.yield) {
        value = resolveValue(builder, *parse[expr.yield], handover.type, wanted);
        if(!value) return;

        value = adaptToView(builder, *value, handover, expr.source);
        if(!value) return;

        value = coerce(builder, *value, handover.type, expr.source, "a handed-over value"_v,
                       handover.contract.access());
        if(!value) return;
    } else if(decl()[handover.type]->kind == Type::Unit) {
        value = builder.addConst(expr.source, unitConstant(program.types.unit));
    } else {
        context.diagnostics.error("%@ hands over %@, so this `yield` needs a value"_v, expr.source,
                                  context.findName(function->name),
                                  typeText(context, decl(), handover.type));
        return;
    }

    /*
     * The temporary a **lending** hand-over needs - the same one a borrowing argument gets, and the
     * last step of `prepareArgument` reached from the one position that is a parameter without
     * having been written as an argument.
     *
     * `yield e` is a call of `$body`, and the hand-over is that call's one parameter: a convention
     * that lends leaves this frame owing the value's teardown, and a value that is only a node
     * result has no place for one to name. `iter fn lent(n) -> Held: yield make()` is the shape -
     * the temporary was owned, borrowed by the consumer, and released by nobody, which the SSA
     * verifier reports as an owner reaching `return` unconsumed.
     *
     * A hand-over convention that *takes* - `-> ->T` - is `holdBorrowed`'s first line and passes
     * through untouched: the consumer owns what it received, and this frame stops owing anything the
     * moment the argument is a move.
     */
    value = holdBorrowed(builder, *value, wanted, handover.type, expr.source);
    if(!value) return;

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
 * Whether a `return` is the failure exit rather than the ordinary one.
 *
 * Read off the returned value rather than remembered from where it was built, on the same terms the
 * walk below reads the hand-over off the write that fills `$yielded`: the aggregate's constructor is
 * what the exit *is*, and a second record of it kept beside the node would be a fact that can go out
 * of step with the IR it describes.
 */
static bool isFailureReturn(Body& body, MirBase base, Node& node) {
    auto& written = (NodeReturn&)node;
    if(!written.hasValue || written.value.source() != SourceKind::Result) return false;

    auto producer = base[body.nodes.get(base, written.value.nodeId().index)];
    if(producer->kind != Node::Aggregate) return false;

    return ((NodeAggregate*)producer)->constructor == kOutcomeExit;
}

/*
 * The forward walk both "once" rules are read off - the lens's hand-over and the `@lazy` force.
 *
 * "Every path hands over exactly once" and "no path forces twice" are two readings of one walk, and
 * the two bits below are what makes them separable: `kBeforeEvent` says a path reached this block
 * without having done the thing, `kAfterEvent` says one reached it having done it, and both set is a
 * merge. From there, *twice* is a block that sees the event with `kAfterEvent` already on, and
 * *never* is an exit reached with `kBeforeEvent` still on - so a count of the events in the text,
 * which would report `if c: v else: v` and accept a `v` inside a loop, is not what either rule asks.
 *
 * `event` says which node is the thing being counted, and is the whole of what the two callers
 * disagree about: a write into `$yielded` for one, a read of a deferred parameter's storage for the
 * other. `exit` is handed each terminator with the bits the path *leaves* under, which is not what
 * the block was entered in and is deliberately not written back - the worklist's condition is the
 * entry state, and a block whose exit state was stored would re-enter itself through a back edge
 * already marked as having done the thing and report a second one no path makes.
 *
 * `state` is left holding those entry bits, so a caller with a second question about the same walk -
 * which blocks fall off the end without having done it - asks it of the same answer rather than of a
 * second walk.
 *
 * Answers where a path did it a **second** time, and `kNullLocation` where none did. The second and
 * not the first, because the first is the one that is allowed.
 */
static constexpr U8 kBeforeEvent = 1;
static constexpr U8 kAfterEvent = 2;

template<class Event, class Exit>
static LocationId walkOnce(Builder& builder, SmallArray<U8, 32>& state, Event&& event, Exit&& exit) {
    auto base = builder.base();
    auto& body = builder.body;

    state.clear();
    for(U32 i = 0; i < body.blocks.size(); i++) state.push(0);

    SmallArray<U32, 16> pending;

    auto mark = [&](BlockId block, U8 bits) {
        if(!block.valid()) return;
        if((state[block.index] & bits) == bits) return;

        state[block.index] = U8(state[block.index] | bits);
        pending.push(block.index);
    };

    mark(BlockId(0), kBeforeEvent);

    auto twice = kNullLocation;

    while(pending.size()) {
        auto index = pending[pending.size() - 1];
        pending.resize(U32(pending.size() - 1));

        auto block = body.block(base, BlockId(index));
        auto bits = state[index];

        for(auto nodePointer: block->nodes.contents(base)) {
            auto& node = *base[nodePointer];
            if(!event(node)) continue;

            if((bits & kAfterEvent) && twice == kNullLocation) twice = node.source;
            bits = kAfterEvent;
        }

        auto count = block->nodes.size();
        if(!count) continue;

        auto last = base[block->nodes.get(base, count - 1)];
        if(!isTerminator(last->kind)) continue;

        exit(*last, bits);

        auto reached = bits;
        forEachEdge(base, *last, [&](Edge& edge) { mark(base[edge.target]->index, reached); });
    }

    return twice;
}

/*
 * Every path through a lens hands over exactly once - Analysis-Lens.md §7.1.
 *
 * A forward walk over the callee's own blocks rather than a count of `yield` statements, because
 * both halves of the rule are about paths, and **once per lens** rather than once per call site,
 * which is what W-A buys. A lens that hands over on one arm of an `if` and not the other is a
 * *skipping* lens, which declares `| E` and whose call site says where the skip goes; silently
 * accepting it here would be a continuation that sometimes does not run at all. One that
 * hands over twice would run one continuation twice, which is the one thing the marker promises it
 * does not.
 *
 * `walkOnce` is the walk; what is here is which node is the event and what each of the two answers
 * means for a lens.
 */
bool Resolver::checkHandover(Builder& builder, Function& function, LocationId source) {
    auto base = builder.base();
    auto& body = builder.body;
    auto slot = yieldedLocal->index;

    auto missing = false;
    auto failedLate = kNullLocation;

    // The hand-over is the write that fills `$yielded`. The `yield` is an ordinary call now, so what
    // marks it is the write of the answer rather than a terminator - which is also what makes
    // "twice" mean twice on one path rather than twice in the text.
    SmallArray<U8, 32> state;
    auto twice = walkOnce(builder, state, [&](Node& node) {
        if(node.kind != Node::Write) return false;

        auto destination = ((NodeWrite&)node).destination;
        if(destination.source() != SourceKind::Place) return false;

        auto place = body.place(base, destination.placeId());
        return place->kind == Place::Local && place->payload == slot.index;
    }, [&](Node& last, U8 bits) {
        /*
         * The two things an *exit* says, read off the state the path left under.
         *
         * A `return` is the only terminator that can leave without having handed over, since falling
         * off the end is `finish`'s and is read off the entry bits below.
         *
         * Failing is the one `return` this does not say it about, and is the whole of what `| E`
         * relaxes: a traversal that failed is *why* it did not hand over, so a path reaching that
         * exit is the rule being satisfied rather than broken. The same path in the other direction
         * is refused below - having handed over and then failing is a shape the caller's control
         * flow cannot show, so it is not something to be quiet about.
         */
        if(last.kind != Node::Return) return;

        auto failing = isFailureReturn(body, base, last);

        if(!(bits & kAfterEvent) && !failing) missing = true;
        if((bits & kAfterEvent) && failing) failedLate = last.source;
    });

    if(twice != kNullLocation) {
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
        if(!(state[i] & kBeforeEvent)) continue;

        missing = true;
    }

    /*
     * Failing *after* having handed over - refused, and the message says which of the two shapes the
     * author probably meant.
     *
     * A lens's alternative runs **instead of** its continuation, which is what `| E` means at the
     * call site and what makes the arm writable: the block below the call is the same block either
     * way, so nothing of the continuation's exists on the failure path. A lens that yielded and then
     * failed would break that, and the caller's CFG cannot even show it - the skip edge leaves the
     * call, so a path through the continuation and out the failure exit is not one the drop and loan
     * walks can see. An `iter fn` is the shape that *can* fail after handing over, because its
     * consumer's loop ending is an ordinary thing to happen and `Many` carries the edge for it.
     */
    if(failedLate != kNullLocation) {
        context.diagnostics.error(
            "%@ hands over and can then fail, and a lens's alternative runs instead of its continuation rather than after it - fail before the `yield`, or make it an `iter fn`"_v,
            failedLate, context.findName(function.name));
        return false;
    }

    if(missing) {
        if(function.failure) {
            context.diagnostics.error(
                "a path through %@ reaches the end without a `yield` - a traversal that may skip its continuation says so by failing, so this path needs a `return`"_v,
                source, context.findName(function.name));
        } else {
            context.diagnostics.error(
                "a path through %@ reaches the end without a `yield` - a lens that may skip its continuation declares what it fails with, as `-> (T | E)`"_v,
                source, context.findName(function.name));
        }

        return false;
    }

    return true;
}

/*
 * A `@lazy` parameter is forced **at most once on any path** - `doc/spec/functions.md`.
 *
 * `walkOnce`, which is the whole of the rule: the lens's hand-over and this are one statement about
 * paths asked of two different events, and the only thing said here is which node is the event.
 *
 * What it looks for is a **read of the parameter's storage**, which is every force there is - a read
 * of a deferred name is a call of the thunk (`forceDeferred`), and handing the thunk on to another
 * deferred position is a read that calls nothing here and spends the same permission, which is what
 * makes travelling and forcing one rule rather than two.
 *
 * The rule is what makes this call-by-name and not call-by-need: forcing twice is *rejected* rather
 * than supported, so there is no memoization slot, no flag and no allocation anywhere in the
 * lowering. It is the one place that difference is stated.
 */
void Resolver::checkForced(Builder& builder, Function& function) {
    auto base = builder.base();
    auto& body = builder.body;

    for(auto argumentPointer: function.args.contents(decl())) {
        auto& argument = *decl()[argumentPointer];
        if(!argument.lazy) continue;

        Local* parameter = nullptr;
        for(auto localPointer: body.locals.contents(base)) {
            auto local = base[localPointer];
            if(local->parameter != argument.index) continue;

            parameter = local;
            break;
        }

        // No storage, which is a body that reported before the parameters were declared. Nothing to
        // walk, and nothing this walk could say that the earlier message does not.
        if(!parameter) continue;

        auto slot = parameter->index;

        SmallArray<U8, 32> state;
        auto twice = walkOnce(builder, state, [&](Node& node) {
            if(node.kind != Node::Read) return false;

            auto read = ((NodeRead&)node).place;
            if(read.source() != SourceKind::Place) return false;

            auto place = body.place(base, read.placeId());
            return place->kind == Place::Local && place->payload == slot.index;
        }, [](Node&, U8) {});

        if(twice != kNullLocation) {
            context.diagnostics.error(
                "%@ is `@lazy`, and a path through %@ reads it twice - a deferred argument runs where it is read, so a second read would run it again"_v,
                twice, context.findName(argument.name), context.findName(function.name));
        }
    }
}

void Resolver::resolveAssign(Builder& builder, ast::Expr& expr) {
    auto& written = *parse[expr.assign];

    ErrorMark mark(context);

    /*
     * A subscript target, in two halves - `Resolver::Subscript`. The container and the index first,
     * then the value, then the accessor: `xs[i] = xs[i] + 1` on a window reads its right-hand side
     * through `get` and writes through `getMut`, and the exclusive loan the second takes has to
     * start after the first has ended. A fixed array's element is a lazy place and the order costs
     * it nothing.
     *
     * **A path of fields over the subscript is the same statement one projection out** -
     * `xs[i].f = xs[i].f + n`, which is `lib/Test/Console.yana`'s tally and was the one shape of
     * this that had to be written with a local in between. The destination decides the accessor
     * (`getMut`) and the right-hand side reads through `get`, so the two are the same pair; what
     * differs is only that the loan has to be taken after the whole right-hand side rather than
     * after the container and the index. So the steps are collected here, the accessor is chosen
     * where it is chosen for a bare subscript, and `fieldStep` applies them to what comes back -
     * the same walk `resolvePlace` would have made.
     *
     * The right-hand side needs an expected type before the place exists, and `fieldPathType` is
     * that and only that: where it cannot answer - a positional field, a pointee, a view - the
     * target is built first exactly as it was before.
     */
    Place* place = nullptr;
    Subscript subscript;
    Node* value = nullptr;

    SmallArray<ast::Expr*, 4> steps;
    auto under = &written.target;

    while(under->kind == ast::Expr::Field) {
        steps.push(under);
        under = &parse[under->field]->target;
    }

    auto subscripted = under->kind == ast::Expr::Sub && !parse[under->sub]->args.isEmpty() &&
                       parse[under->sub]->args.get(parse, 0).value.kind != ast::Expr::Range;

    // The element type the right-hand side is read against, which for a bare subscript is the
    // element's own. Null where the steps say nothing this walk can follow.
    DeclPtr<Type> expected = nullptr;

    if(subscripted) {
        if(!resolveSubscriptTarget(builder, *under, true, Access::Unique, subscript)) {
            if(!mark.reported()) unsupported(written.target.source, "assigning to this"_v);
            return;
        }

        expected = steps.isEmpty()
            ? subscript.element
            : fieldPathType(subscript.element, steps.pointer(), steps.size());

        // A bare subscript is split whatever `expected` came to - the element type is the hint it
        // always was, null included, and `m[k] = v` reaching `insertAt` depends on it. A path is
        // split only where the walk answered, since there the hint is what the split is *for*.
        if(!subscript.fixed && (steps.isEmpty() || expected)) {
            value = resolveValue(builder, written.value, expected, requestOf(ownedContract()));
            if(!value) return;

            /*
             * `m[k] = v` **inserts** where the container has an `IndexInsert` instance -
             * Implementation-Map.md §7's second decision, and `known.def`'s `indexInsert` row.
             *
             * `getMut` cannot express it: it has to answer a borrow, and an absent key has none, so
             * an assignment written through it traps on exactly the key an author is inserting.
             * Preferred over `getMut` rather than replacing it, so `Array`'s assignment - which has
             * no such instance - is unchanged. A field of the element is not an insert: what is
             * written is part of a value the container already holds.
             */
            if(steps.isEmpty() &&
               resolveIndexInsert(builder, subscript, *value, written.target.source)) {
                return;
            }
        }

        place = elementAccess(builder, subscript, Access::Unique, written.target.source);

        for(Size i = steps.size(); i > 0 && place; i--) {
            place = fieldStep(builder, *place, *steps[i - 1]);
        }
    } else {
        place = resolvePlace(builder, written.target, true, Access::Unique);
    }

    if(!place) {
        if(!mark.reported()) {
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

    if(!value) {
        value = resolveValue(builder, written.value, place->occurrence.type,
                             requestOf(ownedContract()));
        if(!value) return;
    }

    value = coerce(builder, *value, place->occurrence.type, written.value.source,
                   "an assigned value"_v);
    if(!value) return;

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
    Chain chain;
    auto condition = resolveCondition(builder, written.cond, chain);

    // The names the condition binds last exactly as long as the body does. Recorded before the
    // tests, because a conjunction binds between its own spans - `resolveTests`.
    auto scope = bindings.size();

    // A condition that did not resolve leaves the header with no branch to give. It is `unreachable`
    // and the walk continues at the exit, so one bad condition produces one diagnostic rather than a
    // cascade about the statements after the loop.
    SmallArray<PatternBinding, 4> made;
    if(!condition || !resolveTests(builder, condition, exit, carry, made)) {
        bindings.resize(U32(scope));
        if(!builder.isTerminated()) builder.addUnreachable(expr.source);
        builder.setBlock(*exit);
        carry.arrive(builder, *exit, 0);
        return;
    }

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
        /*
         * `for i in 0 .. n | else -> ...` - an alternative on a loop with nothing to fail.
         *
         * Refused here rather than dropped, and here rather than in `resolveCountedFor`, because it
         * is a fact about which of the two `for`s this is: an interval always produces every value
         * it said it would, so there is no failure for an alternative to be the alternative *to*.
         * `checkAlternatives` says the same thing about an iterator that declared no `| E`.
         */
        if(loop.alts.size()) {
            context.diagnostics.error(
                "a `for` over an interval cannot fail, so there is nothing for an alternative to handle"_v,
                loop.pat.source);
        }

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
     * Which iterator the loop runs, by name - written `items(xs)` or `xs.items()`, which are one
     * call and select one declaration.
     *
     * A `for` reaches its iterator through a written call and nothing derives one from a type: a
     * container has several traversals and naming the one meant is the point, which is why
     * `for x in xs` over a container is a message rather than a lookup. An iterator travelling as a
     * *value* - which is what an adaptor chain's inner stage is - is the erased callback ABI and is
     * milestone 6's, so `xs.filter(p).map(f)` is still blocked by its outer receiver and not by the
     * dot.
     */
    NamedCallSite site;
    auto callee = namedCallee(loop.from, &site);

    /*
     * A name several declarations answer - R5 for a traversal, `iteratorCallee`.
     *
     * `namedCallee` answers nothing where the set has several, which is R1's rule and is right for
     * every other reader of it; a `for` is the position where "several" is a question the arguments
     * answer. So the second attempt narrows the set to the iterators and puts it through the same
     * R2-R5 a call goes through, and a plain `iter fn` declared beside a class member is a member of
     * that set rather than a shadow over it.
     *
     * The values it resolved travel to `handoverCall`, held across everything between, because they
     * were emitted into this block and this block is about to grow a region.
     */
    OverloadChoice chosen;
    HeldList<SmallArray<Node*, 8>> held(*this, chosen.values);

    auto before = context.diagnostics.errorCount();
    if(!callee && site) callee = iteratorCallee(builder, loop.from, site, chosen);

    if(!callee) {
        // Reported already where the selection failed on the arguments - "no `chunks` accepts these
        // arguments", which names the candidates. The message below is for the other failure, where
        // nothing the name reaches is a traversal at all.
        if(context.diagnostics.errorCount() != before) return;

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
    auto named = loop.pat.kind == ast::Pat::Var;

    /*
     * `for {a, b} in pairs(m)` - a pattern over what the iterator handed across.
     *
     * The hand-over is storage of the continuation either way, so a pattern here is the same
     * construct a `let` over any other place is: `bindIrrefutable` takes it apart by reading through
     * that local, and what each name it binds takes - a shared borrow, or the owned part where the
     * declaration hands over with `->` - is the pattern's own rule and not a second one written
     * here. Bound inside the region rather than at the call, because the names belong to the body.
     */
    handoverCall(builder, loop.from, callee, Continuation::Many,
                 named ? loop.pat.var : StringId(), loop.pat.source, loop.alts,
                 [&](ContinuationId region, Block& join, Local& bound) {
        Loop frame(*this, nullptr, join, join, region, true);

        if(!named && loop.pat.kind != ast::Pat::Any) {
            if(!bindIrrefutable(builder, loop.pat, *builder.localPlace(bound), "a `for` binding"_v)) {
                return;
            }
        }

        resolveStatement(builder, loop.body);
    }, chosen ? &chosen : nullptr);
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

    /*
     * What an interval counts over is anything that can be compared and stepped - `Ord` and `Num`
     * at the counter's type, which is what every comparison and every addition below is a call of
     * (`applyBinary`), and the stride of one is the type's own `FromInt`. So a program's `Money`
     * counts as a width does, and a type with neither is reported at the first member the loop
     * reaches for, naming the class it lacks.
     */
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
        // The literal `1` at the counter's type - the same call every written literal is, so a
        // type the program declared `FromInt` for gets the one its instance builds.
        WrittenLiteral one { intrinsic::Known::fromInt, 1, 1, 1.0, false };
        step = literalNode(builder, one, counterType, source);
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

    // A pattern over the counter, on the iterator loop's own terms: an interval yields one value per
    // step and taking it apart is the same reading through the storage a name would have named. Very
    // nearly always a mistake at an integer, and what says so is the pattern's own space rather than
    // a rule about counted loops.
    if(!named && loop.pat.kind != ast::Pat::Any) {
        // A pattern that fails to bind leaves the names it would have bound out of scope, so
        // resolving the body against them would report one "unknown name" per read rather than the
        // one pattern diagnostic - the same reason `resolveIteratorFor` checks this result above.
        if(!bindIrrefutable(builder, loop.pat, *builder.localPlace(*bound), "a `for` binding"_v)) {
            return;
        }
    }

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
     * `return e` inside a traversal that declared `| E` - Analysis-External-Iterators.md §2.2's
     * "`return Err(error)` finishes it with failure", written as the failure type directly because
     * `| E` already said which type that is.
     *
     * The spelling is `return` and not a word of its own, and that is the whole of what the failure
     * channel adds to a body: a `return` here was refused for both kinds below, so nothing that used
     * to mean something means something else now. What it produces is the wrapper's other arm, which
     * is the one place in the body that knows the traversal is skipping.
     */
    if(function.failure) {
        if(!expr.ret) {
            context.diagnostics.error(
                "%@ can fail with %@, so `return` here needs the value it fails with"_v,
                expr.source, context.findName(function.name),
                typeText(context, decl(), function.failure));
            builder.addUnreachable(expr.source);
            return;
        }

        auto reason = resolveValue(builder, *parse[expr.ret], function.failure,
                                   requestOf(ownedContract()));
        if(!reason) {
            if(!builder.isTerminated()) builder.addUnreachable(expr.source);
            return;
        }

        reason = coerce(builder, *reason, function.failure, expr.source, "a failure value"_v);
        if(!reason) {
            builder.addUnreachable(expr.source);
            return;
        }

        Node* only = reason;
        auto wrapped = builder.addAggregate(expr.source, function.result.type, kOutcomeExit,
                                            &only, 1);
        if(!wrapped) {
            builder.addUnreachable(expr.source);
            return;
        }

        builder.addReturn(expr.source, *wrapped);
        return;
    }

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
                               requestOf(function.continuationResult));

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

    // The result's own interior shape travels with it - `-> Maybe('v)` is a construction whose
    // payload position is a reference, and the expression filling it is the only thing that can be
    // told so. Null for every result whose value owns what is inside it.
    auto value = resolveValue(builder, *parse[expr.ret], function.result.type,
                              requestOf(function.result));
    if(!value) {
        if(!builder.isTerminated()) builder.addUnreachable(expr.source);
        return;
    }

    // A view record built by this body, at the result that is a view - `adaptToView`, the same
    // step an argument takes, because a result is the other position a body hands a window to.
    value = adaptToView(builder, *value, function.result, expr.source);
    if(!value) {
        builder.addUnreachable(expr.source);
        return;
    }

    value = coerce(builder, *value, function.result.type, expr.source, "a returned value"_v,
                   function.result.contract.access());
    if(!value) {
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
 * Only an **owned** position, which is the whole of the extent half. A borrowed one is bounded by
 * the call it was granted for and may hold a reference to this frame for exactly that long - which
 * is what passing a callback *is*, and refusing it would refuse the feature. What an owned position
 * claims is that the value is now the receiver's, and a receiver that outlives this frame would
 * then hold a reference into a frame that is gone.
 *
 * The **structural** half above it is asked at every convention, and the reason it cannot take the
 * same exemption is that it is not a statement about how long the value lives. A qualified type
 * argument replaces a member with a reference to somebody else's storage, so `Entry('k, 'v)` and
 * `Entry(k, v)` are one logical type laid out two ways - `membersOf` reads the shape through
 * `fieldContract` - and are two different answers to what the value owes. A callee handed the first
 * where it declared the second reads a pointer as the value it points at, for the duration of the
 * call, which is long enough; and the whole-value operations its type offers, `copy` at the head of
 * them, duplicate the storage behind the borrow rather than the borrow. That last sentence is why
 * `residualObligation` does *not* count a declared `Copy`: the call is a position like any other,
 * and this is where such a position is checked.
 */
bool Resolver::checkCarriedOrigins(Node& value, const Occurrence& declared, LocationId source,
                                   StringView what) {
    auto& occurrence = valueOccurrence(value);

    /*
     * Both directions, because the lie is symmetric: a member handed where a borrow is declared
     * reads storage this value owns as somebody else's, exactly as the other way round. One test -
     * `qualifiesInterior` - for both ends and for the two questions above this one.
     */
    if(qualifiesInterior(decl(), occurrence) || qualifiesInterior(decl(), declared)) {
        if(shapeOf(decl(), occurrence) != shapeOf(decl(), declared)) {
            context.diagnostics.error(
                "%@ and %@ disagree about which positions inside it are somebody else's, so the two are laid out differently - write the same qualifier on both"_v,
                source, typeText(context, decl(), occurrence.type), what);
            return false;
        }
    }

    if(!declared.contract.isOwned()) return true;

    /*
     * A **raw pointer** is exempt, and that is `doc/spec/native.md`'s line rather than a gap. The
     * pointee origin a `%source'a` carries is liveness information for the loan analysis - it is
     * what roots `borrow(advance(values(xs), i))` in `xs` - and not a capability the pointer's
     * holder is answerable for: nothing about a pointer is checked once it is stored, and a
     * `Run {items: p}` or a heap cursor written into a global is exactly the unchecked storage the
     * module exists to write. A closure's environment and a borrowed field are the positions this
     * check is about, and both are records.
     */
    if(decl()[occurrence.type]->kind == Type::Pointer) return true;

    auto carried = shapeOf(decl(), occurrence);

    /*
     * The destination's own shape is what discharges it, which is what makes this one rule rather
     * than an exemption per position. A `let` mints its local from the value it binds, so the two
     * shapes are the same handle and a binding is accepted; a field, a `->` parameter and an
     * assignment to storage that already exists all have a shape somebody else declared, and a value
     * carrying more than it says is a reference the receiver was not told about.
     */
    if(Shapes::isEmpty(carried) || carried == shapeOf(decl(), declared)) return true;

    /*
     * A function value with **no** environment, which says so in the one vocabulary a value has -
     * `Shapes::noEnvironment`, and D17's reason for there being two marks rather than one.
     *
     * The second word is null, so there is nothing behind it for a receiver to be surprised by, and
     * `let f = twice` may be stored, returned and copied exactly as it could before a `Fun` owed a
     * teardown. What still cannot is the *other* mark: a closure whose environment is a local of
     * this frame, which is the whole of what is left of this check for closures - a lambda written
     * directly at an owned position is heap-placed and carries no mark at all.
     */
    if(carried == program.shapes.noEnvironment()) return true;

    context.diagnostics.error(
        "%@ refers to storage this frame owns, and %@ has no origin to say whose - write the lambda at the position that takes it, so its environment can be placed where the value goes"_v,
        source, typeText(context, decl(), occurrence.type), what);
    return false;
}

void Resolver::resolveGlobals(ast::Decl& declaration, Size& next) {
    if(declaration.stmt.kind != ast::Expr::Decl) return;

    // The root module's top level is a program point, so an initializer that is not a constant there
    // is a **dynamic** global rather than a mistake - Analysis-Initialization.md stage B, and
    // `resolveConstant`'s `notConstant` is how the two are told apart. A `.test.yana` file's module
    // level is a program point too, under `-test` - `atProgramPoint`, and `declareGlobals`, where
    // that is argued.
    auto startup = atProgramPoint();

    for(auto binding: declaration.stmt.decl.contents(parse)) {
        if(next >= writtenGlobals.size()) return;

        auto pointer = writtenGlobals[next++];
        if(!pointer || !binding.content) continue;

        auto global = decl()[pointer];
        auto notConstant = false;
        auto constant = resolveConstant(*parse[binding.content], nullptr, &notConstant);

        /*
         * A **string**, whose value is the one thing here that cannot be settled yet - at the root
         * or at a leaf, which is why the test is `holdsText` rather than a comparison of the kind:
         * `let names = ["a", "b", "c"] :: [String *3]` is one global with three of them in it, and
         * a walk that only looked at the root left `Constant::Text` in the aggregate for
         * `materialize` to assert on.
         *
         * `Constant::Text` says what the value *is* and defers how it is laid out. Building it
         * means minting the bytes as a global of their own and spelling the three records over that
         * address - `Resolver::stringConstant` - and neither the byte global nor the record rows it
         * reads may be reached while an initializer is first read. `resolveTextGlobals` is the
         * drain, and `PendingTextGlobal` is where the ordering is argued.
         *
         * The global is a constant either way, which is the part that matters at this point: its
         * type is settled here, it is not `dynamic`, and nothing puts it in the entry sequence. A
         * library module therefore has a string global for the same reason it has a numeric one -
         * there is nothing to run - which is what `Core/Show.yana`'s digit table needs.
         */
        if(holdsText(decl(), constant)) {
            global->occurrence = owned(constant.type);
            pendingTextGlobals.push(PendingTextGlobal {
                pointer, here(), constant, current->activeFile,
                parse[binding.content]->source
            });

            continue;
        }

        /*
         * And an aggregate holding a **box**, for the same sentence with a different missing half:
         * what belongs in the field is an allocation, and static data has no way to hold one.
         * `constantNeedsAllocation` is where that is argued.
         */
        if(constantNeedsAllocation(decl(), constant)) notConstant = true;

        if(notConstant) {
            /*
             * A library module has nothing that would run this, and saying so names the two things
             * an author can do about it rather than the form that was written: make it a constant,
             * or move it into the module being compiled.
             */
            if(!startup) {
                context.diagnostics.error(
                    "a global's initializer must be a literal, a constructor or an aggregate of those, optionally written `constant :: Type` - there is no program point at which a declaration's own code would run"_v,
                    parse[binding.content]->source);
                continue;
            }

            // The type is what the initializer turns out to be, and that is settled by the entry
            // sequence - `resolveEntryBody`, which runs before any other body reads the name.
            global->dynamic = true;
            continue;
        }

        if(!constant.isSet()) continue;

        global->initial = constant;
        global->occurrence = owned(constant.type);

        /*
         * And, where this global is one the compiler supplies, the target's answer written over
         * what the declaration said - `bindSuppliedGlobal`, immediately rather than in a later
         * pass, so that the next constant may read it. `let mask = pageBytes - 1` is the shape that
         * makes the ordering matter: a supply step that ran after every initializer would have
         * handed that reader the zero `pageBytes` is required to be written as.
         */
        bindSuppliedGlobal(context, program, pointer);
    }
}

/*
 * The strings, once every body is in - `PendingTextGlobal`, where the ordering is argued.
 *
 * The module scope is restored per entry because the evaluation places a global: the bytes of the
 * literal belong to the module that wrote it, and a drain that ran them all in whatever scope was
 * last open would file them under the wrong one and name them after it.
 */
void Resolver::resolveTextGlobals() {
    for(auto& pending: pendingTextGlobals) {
        ModuleScope scope(*this, pending.module, pending.file);

        auto value = textsResolved(pending.value, pending.source);
        if(!value.isSet()) continue;

        auto global = decl()[pending.global];
        global->initial = value;

        // The type the *evaluation* produced, which is the one the bytes are laid out with. It is
        // the role's result type and therefore the one already on the global; asserting rather than
        // assigning is how a library whose builder answers something else is caught here instead of
        // as a wrong-sized global.
        assertTrue(sameType(value.type, global->occurrence.type));
    }

    pendingTextGlobals.clear();
}

bool Resolver::runsAtStartup() {
    for(auto& statement: topLevel) {
        // A statement that is not a `let` is code, and code runs.
        if(statement.stmt.kind != ast::Expr::Decl) return true;

        for(U32 i = 0; i < statement.count; i++) {
            auto pointer = writtenGlobals[statement.first + i];
            if(pointer && decl()[pointer]->dynamic) return true;
        }
    }

    return false;
}

/*
 * One statement of the root module's top level, emitted into the entry being built.
 *
 * A `let` fills the storage its own declaration made, which is why this is not `resolveDeclaration`:
 * the name is a **global** and was declared by the first pass, so there is no local to introduce and
 * nothing to put in scope. What runs here is the initializer, and only for a global whose value is
 * not a constant - a constant one holds its value before the program starts and has no place in the
 * sequence at all.
 *
 * The write is an `Init` and not an `Assign`, which is the fact the rest of the rule hangs off: this
 * is where the global's storage *begins*, so nothing before it is a re-initialization and the drop
 * placement has nothing to pre-drop.
 */
void Resolver::resolveTopLevelBinding(Builder& builder, const TopLevelStmt& statement) {
    auto written = statement.stmt.decl;
    auto index = U32(0);

    for(auto binding: written.contents(parse)) {
        auto at = index++;
        if(at >= statement.count) break;

        auto pointer = writtenGlobals[statement.first + at];
        if(!pointer || !binding.content) continue;

        auto& global = *decl()[pointer];

        /*
         * A constant global, whose declaration is a **position** in the sequence and not a step of
         * it: it holds its value before the program starts, so there is nothing here to run and
         * nothing to take out of the uninitialized set - only a dynamic global was ever put in one.
         *
         * What the position is still worth is the order around it. A dynamic global declared below
         * this one reads what this declaration said wherever the declaration stands, which is what
         * makes "written order" a statement about the whole top level rather than about its code.
         */
        if(!global.dynamic) continue;

        auto value = resolveValue(builder, *parse[binding.content], nullptr,
                                  bindingRequest(Access::Owned));

        // Out of the set whether or not the initializer resolved: the statement has had its turn,
        // and a second message about every later read would be about this failure rather than about
        // them.
        for(Size i = 0; i < uninitialized.size(); i++) {
            if(uninitialized[i] != pointer) continue;

            uninitialized.remove(U32(i));
            break;
        }

        if(!value) continue;

        /*
         * The global's type is the initializer's, which is why the entry is resolved before every
         * other body: a function above the `let` it reads needs the answer, and this walk is what
         * produces it.
         *
         * **Owned**, and a loan is refused rather than recorded. A global's storage outlives every
         * frame, so a reference into one is a reference to storage that stops existing the moment
         * the program starts - which is the same rule a record field is under, said about the one
         * position whose lifetime is the program.
         */
        auto occurrence = valueOccurrence(*value);

        if(!occurrence.contract.isOwned()) {
            context.diagnostics.error(
                "a global cannot hold a borrow - its storage outlives every frame, so there is nothing for the reference to refer to"_v,
                global.source);
            continue;
        }

        global.occurrence = owned(occurrence.type);

        auto place = builder.globalPlace(pointer);
        if(!place) continue;

        builder.addWrite(binding.pat.source, *place, *value, Destination::Init);
    }
}

/*
 * The resolver state one synthesized entry starts from - the two entries share it because they are
 * the same body with a different tail.
 *
 * A function rather than a copy in each, since what it is is "no enclosing body's state is in
 * scope": a stale `continuationLocal` or a leftover binding would be a name the entry resolves
 * against a frame that no longer exists.
 */
void Resolver::beginEntry(Function& function) {
    currentFunction = &function;
    bindings.clear();
    continuationLocal = nullptr;
    yieldedLocal = nullptr;
    guardYields = false;
    handovers = 0;
}

/*
 * The root module's top level, as a run of statements in the body it is given - Analysis-
 * Initialization.md stage B.
 *
 * Its own function because **both** entries begin with it: the ordinary one runs it before `main`,
 * and the synthesized test entry runs it before the first case, for the same reason - a program's
 * startup is not a property of what comes after it. Design-Test.md §3.3's rule is about `main`, not
 * about startup.
 */
void Resolver::resolveRootTopLevel(Builder& builder) {
    /*
     * Every dynamic global, uninitialized - and each one removed as its own statement runs.
     *
     * Filled from the module's list rather than from `topLevel`, because that is the order the
     * *declarations* are in and the set is about declarations. What orders the removals is the walk
     * below, which is the sequence.
     */
    for(auto pointer: decl()[program.root()]->globals.contents(decl())) {
        if(decl()[pointer]->dynamic) uninitialized.push(pointer);
    }

    ModuleScope scope(*this, program.root(), 0);

    for(Size i = 0; i < topLevel.size(); i++) {
        auto statement = topLevel[i];

        // The file the statement was written in, so that its imports are the ones in scope - an
        // import is file-scoped, and the top level of a module with two files has two sets.
        current->activeFile = statement.file;

        // A statement after control left is unreachable, and resolving it would append to a block
        // that already ends - `resolveStatements`' rule, at the one other place a run of statements
        // is walked.
        if(builder.isTerminated()) break;

        if(statement.stmt.kind == ast::Expr::Decl) resolveTopLevelBinding(builder, statement);
        else resolveStatement(builder, statement.stmt);
    }

    // Nothing after the top level reads a global before it is initialized, because nothing after it
    // reads one before anything at all: every other body runs after the whole sequence.
    uninitialized.clear();
}

/*
 * The ordinary entry - the top level in written order and then `main`.
 *
 * The call is emitted here rather than left to a backend's entry wrapper because it is a fact about
 * the **program**: a program whose top level ran and then called `main` did those two things in that
 * order on every target.
 */
void Resolver::resolveEntryBody(Function& function, DeclPtr<Function> main) {
    ErrorMark mark(context);

    Builder builder(context, program, function);
    beginEntry(function);

    resolveRootTopLevel(builder);

    Node* status = nullptr;

    if(main && !builder.isTerminated()) {
        auto& declared = *decl()[main];

        Substitution none;
        status = builder.addCall(function.source, declared, nullptr, 0, none);
    }

    /*
     * What the entry answers is what `main` answered.
     *
     * Whether a status of that shape can leave through a register on the way to the host's own entry
     * is the *native* entry's question and not this stage's - a logical type says nothing about how
     * a result travels. What is decided here is only that the two results are the same result.
     *
     * **Asked of the declared result and not of the call**, which is the bug that stood here: a
     * `fn main():` answers `{}` and its call still produces a node, so returning "the status where
     * there is one" handed a value back out of a function whose signature says there is none.
     * `verifyBody` reported it as an internal error at the first top-level statement, and every
     * program with a `main` of that shape and one dynamic global was refused for it - a global
     * written `-(21)` being the way it was found. There is no `main` at all in the other direction,
     * and then the entry's own result is `{}` by construction.
     *
     * The guard is the same one `resolveTestEntry` writes: a top level that left - a call that
     * diverges, a `return` in it - has already terminated this block, and appending a second
     * terminator to one is not a thing a builder does.
     */
    auto unit = decl()[function.result.type]->kind == Type::Unit;

    if(!builder.isTerminated()) {
        if(status && !unit) builder.addReturn(function.source, *status);
        else builder.addReturnUnit(function.source);
    }

    finish(builder, function, unit, function.source, mark);
    builder.seal();
    currentFunction = nullptr;
}

void Resolver::resolveProgramEntry() {
    /*
     * `main` where the **root** module declares one, and only there: every other module of the
     * program is something the root imported, and a `main` in one of those is an ordinary function
     * of that name.
     */
    // The spelling is `intrinsic/role.h`'s, because the shipping stage looks up the same name and
    // the two have to move together - see `kEntryPointName`, where that argument is written.
    auto name = context.addUnqualifiedName(intrinsic::kEntryPointName);

    // Arity zero, since a program's entry takes no arguments and a `main` that took some is a
    // function of that name - which the "no entry point" message is the right thing to say about.
    DeclPtr<Function> main = nullptr;

    if(auto found = program.findFunction(program.root(), name, 0)) {
        // A lens answers what its continuation answered, and a program's entry has no caller to
        // supply one. Reported here rather than left to "there is no entry point", which is what a
        // module with no `main` at all is told and is not what happened.
        if(decl()[found]->lens) {
            context.diagnostics.error(
                "a lens answers what its continuation answered, so it cannot be a program's entry point"_v,
                decl()[found]->source);
        } else {
            main = found;
        }
    }

    // `fn main() = 0` - the entry's own result is what `main` answers, and an `=` form with no
    // result type answers what its body decided. Demanded here because the synthesized entry below
    // copies the type, and because nothing has resolved a body yet at this point in the order.
    if(main) requireResult(*decl()[main], decl()[main]->source);

    /*
     * A module whose every top-level `let` is a constant, which is every program that predates
     * stage B: there is nothing to run before `main`, so nothing is synthesized and `main` is the
     * entry itself. The degenerate case is the general one with an empty sequence rather than a case
     * of its own.
     */
    if(!runsAtStartup()) {
        program.entry = main;
        return;
    }

    // Anonymous, because nothing in the source can name it: it is reached from `Program::entry` and
    // from the reachability walk that reads it, which is what `anonymous` means everywhere else.
    auto symbol = program.uniqueSymbol(program.root(), context, "main$"_v);
    auto entry = new (program.decl) Function(symbol, program.root());
    entry->source = topLevel[0].source;
    entry->summary.bodyKnown = true;
    entry->result = owned(main ? decl()[main]->result.type : program.types.unit);

    auto pointer = DeclPtr<Function>(entry - *program.decl);
    decl()[program.root()]->functions.push(program.decl, pointer);
    program.entry = pointer;

    resolveEntryBody(*entry, main);
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

    /*
     * Once, whichever walk got here first.
     *
     * The body walk reaches every declaration in order, and `requireResult` reaches an inferring one
     * ahead of that walk when a caller needs its result - so a body can be asked for twice, and
     * building it twice would be two sets of blocks in one function. `Function::bodyState` is the
     * three-colour record of which; `Reading` in particular is what a *recursive* demand sees.
     */
    if(function.bodyState != Function::BodyState::Unread) return;
    function.bodyState = Function::BodyState::Reading;

    // What this body reports, told apart from what the file reported before it - see `ErrorMark`.
    ErrorMark mark(context);

    /*
     * A declaration with no body, and no role saying who supplies one.
     *
     * The parser accepts the shape everywhere - "may this have no body" is a question about the
     * declaration rather than about which directory it is in, and only this stage can see the
     * `@builtin(role)` that answers it. So the refusal is here, and it names the thing that is
     * missing rather than the token the parser wanted next.
     *
     * A **class member** is exempt: a signature with no body is what a class *is*, and what runs is
     * the instance's. It is the one form where a missing body is the declaration's whole content.
     */
    if(!written.body) {
        if(function.intrinsic == intrinsic::Op::Count && !function.klass) {
            context.diagnostics.error(
                "%@ has no body, and no `@builtin(role)` says the compiler supplies one"_v,
                declaration.source, context.findName(function.name));
        }

        function.bodyState = Function::BodyState::Read;
        return;
    }

    Builder builder(context, program, function);

    /*
     * The walk's own state, saved rather than reset, because this body may be **inside** another.
     *
     * `requireResult` builds a callee's body in the middle of the caller's - which is what makes an
     * inferred result independent of declaration order - so everything the walk carries about "where
     * am I" belongs to a function and has to be put back. It is the same set and the same argument
     * as `resolveLiftedBody`'s, one level up: a `break` naming a loop of another function, a
     * temporary held by another walk, a `yield` reaching another continuation. `bindings` is
     * truncated rather than cleared for the same reason, and `lambdaFloor` is the environments' half
     * of the names' floor.
     */
    auto outerFunction = currentFunction;
    auto outerHeld = held;
    auto outerLoops = loops;
    auto outerContinuation = continuationLocal;
    auto outerYielded = yieldedLocal;
    auto outerGuard = guardYields;
    auto outerHandovers = handovers;
    auto outerBindingFloor = bindingFloor;
    auto outerLambdaFloor = lambdaFloor;
    auto outerBindings = U32(bindings.size());

    currentFunction = &function;
    held = nullptr;
    loops = nullptr;
    continuationLocal = nullptr;
    yieldedLocal = nullptr;
    guardYields = false;
    handovers = 0;
    bindingFloor = outerBindings;
    lambdaFloor = U32(lambdaFrames.size());

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
        // The *continuation's* answer and not the function's - `Function::continuationResult`. The
        // two differ exactly where the traversal can fail, and what is parked here is what the
        // `yield` was handed back, which the failure exit never produces.
        yieldedLocal = builder.addLocal(context.addUnqualifiedName("$yielded", 8),
                                        function.continuationResult, function.source,
                                        function.iterator);
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
    // `fn f(...) = expr` with no `-> T` - what the body answers is the result, so there is nothing
    // to check it against and nothing yet to say it is unit. See `Function::inferResult`.
    auto infer = function.inferResult;
    auto inferred = infer;
    auto unit = !infer && decl()[function.result.type]->kind == Type::Unit;

    if(written.implicitReturn) {
        // `fn f(...) = expr` answers `expr`. A unit-returning one still evaluates it, and what it
        // returns is nothing rather than the value.
        auto value = resolveValue(builder, expr, (unit || infer) ? nullptr : function.result.type,
                                  unit ? inspectRequest() : requestOf(function.result));

        // Settled before anything reads it, and before the `ret` below is checked against it. A
        // second demand from inside this very body is what the cycle guard caught, so by here the
        // answer is this body's alone.
        if(infer) {
            unit = inferResultType(function, value, expr.source);
            infer = false;
        }

        if(value && !builder.isTerminated()) {
            if(unit) {
                builder.addReturnUnit(expr.source);
            } else if((value = adaptToView(builder, *value, function.result, expr.source)) &&
                      (value = coerce(builder, *value, function.result.type, expr.source,
                                      "a returned value"_v,
                                      function.result.contract.access())) &&
                      checkResultContract(*value, expr.source)) {
                builder.addReturn(expr.source, *value);
            }
        }

        /*
         * An `=` function that produces nothing is written in the wrong form.
         *
         * The `=` form says "this function *is* this expression", so a body with no value is a
         * statement wearing an expression's syntax - `fn bump(&x: I32) = x = x + 1` reads as though
         * it returned something. The block form is how that is said, and it is what this points at.
         *
         * Only where the unit result was **not written down**, which is what `inferResult` already
         * says: `-> {}` is the author saying the same thing the warning would, and an instance
         * member's result came from its class rather than from this line. A body that reported is
         * left alone too - the missing value is a consequence of the mistake it already named.
         */
        if(inferred && unit && !function.resultUnknown && !mark.reported()) {
            context.diagnostics.warning("`%@` is written with `=` but its body produces no value, so it returns `{}` - use the `:` block form for a function that runs statements rather than producing a result"_v,
                                        declaration.source, context.findName(function.name));
        }
    } else {
        // `fn f(...): statements` answers through `return`, which is what the language says - a
        // block body's last statement is a statement and not a result.
        resolveStatement(builder, expr);
    }

    finish(builder, function, unit, declaration.source, mark);

    if(function.alwaysInline) checkAlwaysInline(function, declaration.source);

    // Over the sealed shape of the body rather than as the walk goes, because both halves of the
    // rule are about paths and `finish` is what closed the ones that fall off the end.
    // An iterator hands over any number of times, so none of the rule applies to it - not even the
    // zero case, since an iterator that yields nothing is a filter that accepted nothing.
    if(function.lens && !function.iterator) checkHandover(builder, function, declaration.source);

    // The same walk over the same sealed body, and for the same reason: at most once *on a path* is
    // not a property of the text.
    checkForced(builder, function);

    builder.seal();

    bindings.resize(outerBindings);
    bindingFloor = outerBindingFloor;
    lambdaFloor = outerLambdaFloor;
    currentFunction = outerFunction;
    held = outerHeld;
    loops = outerLoops;
    continuationLocal = outerContinuation;
    yieldedLocal = outerYielded;
    guardYields = outerGuard;
    handovers = outerHandovers;

    function.bodyState = Function::BodyState::Read;
}

/*
 * What an `=` body decided its function answers - the second half of `Function::inferResult`.
 *
 * Answers whether the result turned out to be unit, which is what the `ret` and `finish` below need
 * and is the one thing the caller cannot read off `result` alone: a body that reported has no value
 * to read a type from, and leaving that as "some type" would be a second message per path.
 *
 * **What is inferred is an owned result, and nothing else.** A borrowing one would have to name an
 * origin slot, and there is nothing in a body that says which - so the only thing inference could do
 * is promote a parameter's group to satisfy it, which is exactly what §2.2 refuses at a *written*
 * signature "rather than promoting the parameter", and for a reason that applies twice as hard here:
 * a caller's argument would change meaning because somebody edited a body. So the body is resolved
 * under an owning request, which is what makes `fn pass(p: Point) = p` a copy and what refuses a
 * hand-over of something the function only borrows - at the read, where the message is about the
 * value. The test below is the residue neither that nor `checkReturnConformance` reaches: a value
 * whose root is a loan, or whose *interior* publishes an origin, arriving at a result that publishes
 * none. Both are told to write the result out, where a `'` can name a group the signature has.
 */
bool Resolver::inferResultType(Function& function, Node* value, LocationId source) {
    function.inferResult = false;

    // The body reported, so there is no type to read and the result stays the `{}` the signature
    // left. Nothing further is said about it - the missing value is a consequence of what was.
    if(!value) return true;

    auto answered = valueOccurrence(*value);
    if(!answered.type) return true;

    if(!answered.contract.isOwned() || answered.shape) {
        context.diagnostics.error("%@ answers a borrow, and the `=` form cannot infer which origin it is in - write the result out, as in `-> 'T`, naming the parameter's group"_v,
                                  source, context.findName(function.name));
        return true;
    }

    function.result = owned(answered.type);
    return decl()[answered.type]->kind == Type::Unit;
}

/*
 * The result of an `=` function whose body decides it, made available to whoever needs it now.
 *
 * **This is the whole of the order-independence.** A call reads its callee's result, so an answer
 * that arrived when the body walk happened to reach the declaration would depend on which function
 * came first in the file. So the demand resolves the body at the call instead - the same shape as
 * `aliasTarget`, and for the same reason: both are declarations whose meaning is read out of another
 * declaration, and neither has an order that could be given to a cycle.
 *
 * The cycle is the one thing inference cannot do, and this is the one place that can say so: the
 * callee's body is on the stack, so the type being asked for is the type it is about to produce, and
 * only a written `-> T` breaks that. It is reported at the call that closed the cycle rather than
 * once per function in it, and the callee stops being callable rather than being recorded as unit -
 * see `Function::resultUnknown`.
 *
 * Asked by `checkCallable`, which every call form goes through, and by the two positions that read a
 * signature without being a call: a function *value*, and the program's entry.
 */
bool Resolver::requireResult(Function& callee, LocationId source) {
    if(callee.resultUnknown) return false;
    if(!callee.inferResult) return true;

    if(callee.bodyState == Function::BodyState::Reading) {
        context.diagnostics.error("%@ is recursive, so the `=` form cannot infer its result type from its body - write it out, as in `-> Int`"_v,
                                  source, context.findName(callee.name));
        callee.inferResult = false;
        callee.resultUnknown = true;
        return false;
    }

    // The position the callee was written at, not the caller's: an import is scoped to the file that
    // wrote it, so a body read on another declaration's behalf reads its own names - `scopeOf`.
    ModuleScope scope = scopeOf(callee.module, callee.source);

    /*
     * And not the entry sequence's set of globals that have not been initialized yet.
     *
     * A demand can fire from inside `resolveEntryBody` - a top-level statement calling an `=`
     * function - and the body it builds is not the sequence: every function other than the entry
     * runs after the whole of it, which is the sentence that set is kept under. Left standing, a
     * read of a later global would be reported or not depending on whether the entry sequence
     * happened to be the first caller, which is the very ordering this function exists to remove.
     */
    Array<DeclPtr<Global>> pending(move(uninitialized));

    resolveBody(callee, *inferringDecls[callee.written]);

    uninitialized = move(pending);
    return !callee.resultUnknown;
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
 * What an `@inline(always)` body may not be - the whole of the restriction, and it is short.
 *
 * The graft in `resolve_ssa/convert.cpp` converts the callee's blocks into the caller's function and
 * turns its `return` into a jump, and that is a complete treatment of an ordinary body: locals,
 * branches, loops, drops and calls all convert exactly as they would in a function of their own,
 * because the conversion reads the callee's own analysis. What it does not treat is a body that is
 * itself a protocol - a lens or an iterator hands over to a continuation rather than returning, and
 * a body with a skipping call in it outlines a region into a function of its own. Both are refused
 * at the declaration, so that the attribute is a directive: a body that carries it grafts at every
 * call, or does not compile.
 */
void Resolver::checkAlwaysInline(Function& function, LocationId source) {
    auto refuse = [&](StringView what) {
        context.diagnostics.error("`@inline(always)` cannot expand %@ where it is called"_v,
                                  source, what);
        function.alwaysInline = false;
    };

    if(function.lens) return refuse("a lens"_v);
    if(function.iterator) return refuse("an iterator"_v);
    if(!function.mir) return;

    auto& body = *(*program.mir)[function.mir];
    auto base = *program.mir;

    for(U32 i = 0; i < body.blocks.size(); i++) {
        auto block = body.block(base, BlockId(i));
        for(auto pointer: block->nodes.contents(base)) {
            auto kind = base[pointer]->kind;
            if(kind == Node::LensCall || kind == Node::LensTry) {
                return refuse("a body that calls a lens"_v);
            }
        }
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
void Resolver::finish(Builder& builder, Function& function, bool unit, LocationId source,
                      const ErrorMark& mark) {
    auto& body = *(*program.mir)[function.mir];
    auto base = *program.mir;

    // Whether *this body* already reported, which is what suppresses a second message about a path
    // it abandoned. `mark` is taken where the body's walk began, since asking the program-wide count
    // here would suppress the message in every function after the first mistake in the file.
    auto reported = mark.reported();

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
         * A lens or an iterator falling off the end answers what its continuation answered, which is
         * the `yield` form's desugaring read at its last line: `... yield e ... return $v`.
         *
         * Not a path that reaches the end without returning, therefore, and the rule it *is* subject
         * to - that it handed over first - is `checkHandover`'s over every path rather than this
         * block's alone. For an iterator the same storage says the same thing for the same reason:
         * running out of values leaves the fall-out row `resolveBody` wrote there, and a departure
         * leaves the row the consumer sent, so "ran out" and "was left" are one path here - which is
         * what makes the trailing statements below a `yield` ordinary code rather than a second exit.
         */
        if(yieldedLocal) {
            auto value = readPlace(builder, source, *builder.localPlace(*yieldedLocal),
                                   requestOf(function.continuationResult));

            if(!value) {
                builder.addUnreachable(source);
                continue;
            }

            /*
             * A traversal that can fail wraps it, because reaching the end is the *other* answer -
             * `declareOutcome`. Falling off the end is what "the traversal completed" is spelled as,
             * so this is `Proceed` by construction and the failure arm is `resolveReturn`'s.
             */
            if(function.failure) {
                auto wrapped = builder.addAggregate(source, function.result.type, kOutcomeProceed,
                                                    &value, 1);
                if(!wrapped) {
                    builder.addUnreachable(source);
                    continue;
                }

                value = wrapped;
            }

            builder.addReturn(source, *value);
            continue;
        }

        if(!reported) {
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
        forEachEdge(base, *last, [&](Edge& edge) { if(edge.target == target) found = true; });
        if(found) return true;
    }

    return false;
}

// See the declaration, where the two ways out are argued.
bool Resolver::departed(Builder& builder) {
    if(builder.isTerminated()) return true;

    auto block = builder.currentBlock();
    if(!block) return true;

    auto base = builder.base();

    // The entry block is entered by being the entry, which no edge says. Every other block that
    // nothing branches to is one the walk arrived at by leaving everything that could reach it.
    if(block == builder.body.block(base, BlockId(0))) return false;

    return !enteredFrom(builder.body, base, *block);
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
    for(auto& declaration: eachDeclaration(file)) {
        if(declaration.kind == ast::Decl::Stmt) resolveGlobals(declaration, nextGlobalInit);
    }
}

void Resolver::resolveBodies(ast::Module& file) {

    for(auto& declaration: eachDeclaration(file)) {
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
            /*
             * A `deriving (...)` clause's own instances - `Resolver::derivedInstances`, which is the
             * declaration walk's record of what it expanded and in what order.
             *
             * One entry per class named, whether or not it became an instance, so that this walk and
             * `declare` see the same list. An expansion that failed consumed no `writtenInstances`
             * slot, which is why the cursor only advances where there is a declaration to advance
             * for.
             */
            case ast::Decl::Alias: {
                if(!declaration.qualified) break;

                for(Size i = 0; i < declaration.alias.derives.size(); i++) {
                    if(nextDerivedBody >= derivedInstances.size()) break;

                    auto expanded = derivedInstances[nextDerivedBody++];
                    if(!expanded) continue;
                    if(nextInstanceBody >= writtenInstances.size()) break;

                    auto pointer = writtenInstances[nextInstanceBody++];
                    if(pointer) resolveInstanceBody(*decl()[pointer], *expanded);
                }

                break;
            }
            default:
                break;
        }
    }
}

} // namespace mir
