#include "resolve_internal.h"

namespace mir {

void Resolver::resolveStatement(Builder& builder, ast::Expr& expr) {
    switch(expr.kind) {
        case ast::Expr::Multi:
            resolveBlock(builder, expr, nullptr, false, Access::Shared);
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
            resolveIf(builder, expr, nullptr, false, Access::Shared);
            return;
        case ast::Expr::While:
            resolveWhile(builder, expr);
            return;
        case ast::Expr::Ret:
            resolveReturn(builder, expr);
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
            auto value = resolveValue(builder, expr, nullptr, Access::Shared);
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
 * `let x = e`, `let &x = e` and `let ->x = e`.
 *
 * Storage and an initialization, not a binding to a computation. That is what makes the
 * representation place-based: `x` names a place from here on, a later `x = e` is an assignment to
 * the same place, and both of those are things the ownership rules have opinions about. A `let` that
 * named a node instead would have to rediscover the storage the first time anything was written
 * through it.
 *
 * The three spellings differ in one field each and not in what is built. `&` is `writable`; `->` is
 * a *demand* that the initializer be handed over rather than borrowed, which is what a bare name in
 * this position already is - `readMode` answers `Forward` for one and liveness settles it - so the
 * two produce the same node and the marker is the author saying they meant it.
 */
void Resolver::resolveDeclaration(Builder& builder, ast::Expr& expr) {
    for(auto binding: expr.decl.contents(parse)) {
        if(binding.pat.kind != ast::Pat::Var) {
            unsupported(binding.pat.source, "destructuring a declaration"_v);
            continue;
        }

        if(binding.in || binding.alts.size()) {
            unsupported(binding.pat.source, "a declaration with a body"_v);
            continue;
        }

        if(!binding.content) {
            context.diagnostics.error("a local requires an initializer"_v, binding.pat.source);
            continue;
        }

        auto value = resolveValue(builder, *parse[binding.content], nullptr, Access::Owned);
        if(!value) return;

        /*
         * A `let` binds an owner, and this is where that is checked rather than assumed.
         *
         * The initializer of a `let` over a type nothing may copy is a move out of what it named,
         * and `readMode` has already answered whether the position could have one. What arrives
         * borrowed is a *binding to somebody else's storage* - `let y = x` where `x` is a shared
         * parameter of a type with a teardown - which is a loan with an extent, and loans are
         * milestone 5.
         */
        if(!valueOccurrence(*value).contract.isOwned()) {
            unsupported(binding.pat.source, "a `let` that binds a borrow"_v);
            continue;
        }

        auto local = builder.addLocal(binding.pat.var, owned(valueType(*value)),
                                      binding.pat.source, binding.bind == ast::BindType::Ref);

        auto place = builder.localPlace(*local);
        auto operand = builder.resultOf(*value, UseMode::Take);
        builder.addWrite(binding.pat.source, *place, operand, Destination::Init);

        // In scope from *after* its own initializer, which is what makes `let x = x` read the outer
        // `x` rather than itself.
        bindings.push(Binding { binding.pat.var, local });
    }
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

    auto value = resolveValue(builder, written.value, place->occurrence.type, Access::Owned);
    if(!value) return;
    if(!checkType(*value, place->occurrence.type, written.value.source, "an assigned value"_v)) return;

    auto operand = builder.resultOf(*value, UseMode::Take);

    /*
     * `Assign`, always, and `Reinit` never from here.
     *
     * Whether the old contents are there to be released is a dataflow answer and not a syntactic one
     * - the same `x = e` inside a loop replaces a value on the second iteration and fills a moved-out
     * hole on the first - so the resolver states what was written and `analyze_drop.cpp` normalizes
     * it where the state says otherwise. §5.5's "reinit can be a normalized form rather than a source
     * builder operation", read literally.
     */
    builder.addWrite(expr.source, *place, operand, Destination::Assign);
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
    auto condition = resolveValue(builder, written.cond, program.types.boolean, Access::Shared);

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

void Resolver::resolveReturn(Builder& builder, ast::Expr& expr) {
    auto& function = *currentFunction;

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

    auto value = resolveValue(builder, *parse[expr.ret], function.result.type, Access::Owned);
    if(!value) {
        if(!builder.isTerminated()) builder.addUnreachable(expr.source);
        return;
    }

    if(!checkType(*value, function.result.type, expr.source, "a returned value"_v)) {
        builder.addUnreachable(expr.source);
        return;
    }

    // A result the caller receives is theirs, so what leaves here has to be an owner. A borrow
    // returned is a loan that outlives the call it was granted for, which is the check milestone 5
    // states properly; refusing it by name is what keeps this stage from producing one meanwhile.
    //
    // A backstop rather than the diagnostic a program gets. `Resolver::readPlace` reports the one
    // shape that reaches it today - returning a borrowed *name* - and answers null, so this fires
    // only for a borrow that came out of a node, which nothing produces yet.
    if(!valueOccurrence(*value).contract.isOwned()) {
        unsupported(expr.source, "returning a borrow"_v);
        builder.addUnreachable(expr.source);
        return;
    }

    builder.addReturn(expr.source, builder.resultOf(*value, UseMode::Take));
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

void Resolver::resolveBody(Function& function, ast::Decl& declaration) {
    auto& written = declaration.fun;
    if(!written.body) return;

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
    }

    auto& expr = *parse[written.body];
    auto unit = decl()[function.result.type]->kind == Type::Unit;

    if(written.implicitReturn) {
        // `fn f(...) = expr` answers `expr`. A unit-returning one still evaluates it, and what it
        // returns is nothing rather than the value.
        auto value = resolveValue(builder, expr, unit ? nullptr : function.result.type,
                                  unit ? Access::Shared : Access::Owned);

        if(value && !builder.isTerminated()) {
            if(unit) {
                builder.addReturnUnit(expr.source);
            } else if(checkType(*value, function.result.type, expr.source, "a returned value"_v)) {
                if(valueOccurrence(*value).contract.isOwned()) {
                    builder.addReturn(expr.source, builder.resultOf(*value, UseMode::Take));
                } else {
                    unsupported(expr.source, "returning a borrow"_v);
                }
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
