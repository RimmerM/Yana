#include "resolve_internal.h"
#include "print.h"
#include "select.h"

namespace mir {

/*
 * `?` and `?.` - the two operators that read a carrier, and what makes them two.
 *
 * `doc/spec/expressions.md`, and Implementation-Semantics.md part 5. Both ask the same question of
 * the same class - `Try(m -> a, e)`, "which of this value's two paths is it on" - and they differ in
 * *where the other path goes*, which is the whole of why one operator could not have served:
 *
 *  - **`?` leaves the enclosing function.** `parse(text)?` is the value where it parsed and a
 *    `return` where it did not, and what it returns is that function's own result type rebuilt from
 *    the reason - `Try.fromExit`, selected for the **signature** rather than for the operand.
 *  - **`?.` skips to the end of the chain it is written in.** `row?.name.trim()` is a
 *    `Maybe(String)` and nothing departs, which is what every language with this spelling means by
 *    it. What the chain comes back in is `Rewrap(m, b -> n)`, the one direction `Try` cannot state.
 *
 * The asymmetry in the first is the entire reason `?` is compiler-known rather than a suffix
 * somebody could declare. Every other skipping construct in the language has its destination written
 * at the call site - a guard's alternatives, an `is`'s `else`, a `for`'s exit - and this one's is the
 * enclosing return edge, typed by a signature the expression cannot see. Everything else about both
 * operators is ordinary: `Try` and `Rewrap` are library classes, and a carrier joins by writing an
 * instance.
 *
 * ## What is shared with the rest of the resolver
 *
 * Almost all of it, and deliberately. The branch is `armsValue`, the same construct `if`, `match`
 * and `&&` are; the calls are `applyMember`, the same one an operator goes through; the payload is a
 * *place* projected out of the outcome, which is what `resolve_match.cpp` does to a constructor
 * pattern. What is here is the selection of the two shapes and the choice of where the other arm
 * goes, which is the part no existing construct answers.
 */

/*
 * What a carrier's `Try` instance says - the two positions nothing at a use site constrains.
 *
 * `class Try(m -> a, e)` is keyed on the carrier alone, so this is one `selectInstance` with the two
 * determined positions left null and read back off whichever head matched. That is the dependency
 * doing its whole job: `parse(text)?` says which carrier it is about and says nothing at all about
 * what it proceeds or exits with, and asking for those as holes is the difference between a lookup
 * and an inference problem.
 *
 * **A requirement is the other answer**, and it is not the instance table's. A body whose signature
 * declares `(Try(m, a, e))` over its own variable has a *promise* rather than an instance - reading
 * a blanket instance for `m` would commit the body to that instance and ignore the one the caller's
 * type actually has - so the enclosing context's requirements are asked first for a carrier that is
 * still a variable. `fillDependency` in the other stage splits the same way and for the same reason.
 */
bool Resolver::carrierShape(DeclPtr<Type> carrier, LocationId source, TryShape& out) {
    auto klass = program.roles.klass(intrinsic::Known::carrier);
    if(!klass || !carrier) return false;

    out.carrier = carrier;

    /*
     * The signature's own promise, where the carrier is this body's variable.
     *
     * Matched on the carrier position alone, which is the deciding one: a requirement states all
     * three arguments, so what the other two are is read straight off it. Nothing is selected and
     * nothing is instantiated - the call below travels as a dispatch, which is what every class call
     * over a body's own variable does.
     */
    if(!isConcreteType(decl(), carrier)) {
        auto gen = genContext;
        if(!gen) return false;

        for(auto requirement: decl()[gen]->classes.contents(decl())) {
            if(requirement.klass != klass) continue;

            auto args = requirementArgs(decl(), requirement);
            if(args.count != 3 || !sameType(args.data[0], carrier)) continue;

            out.proceeds = args.data[1];
            out.reason = args.data[2];
            return true;
        }

        /*
         * And the **open head** - inventory D13, which is the case a promise cannot cover.
         *
         * `Result(ReadError, a)` inside `fn (ReadWith(s, a)) readAll(...) -> Result(ReadError, a)`
         * names `Result`, so the instance that serves it is decided for every `a` a specialization
         * binds; a requirement that stated it would have to be written on every signature that
         * returns a result, which is what the carrier class exists to not need. A *bare* variable
         * still falls through to the refusal above, because there is nothing there to decide a head
         * by - `openHeadAnswerable` is where the line is.
         */
        InstanceMatch open;
        DeclPtr<Type> asking[3] = { carrier, nullptr, nullptr };

        if(!selectOpenInstance(context, program, klass, asking, 3, gen, source, false, open)) {
            return false;
        }

        if(open.arguments.size() != 3) return false;

        out.proceeds = open.arguments[1];
        out.reason = open.arguments[2];
        return true;
    }

    InstanceMatch found;
    DeclPtr<Type> asked[3] = { carrier, nullptr, nullptr };
    if(!selectInstance(context, program, klass, asked, 3, source, false, found)) return false;
    if(found.arguments.size() != 3) return false;

    out.proceeds = found.arguments[1];
    out.reason = found.arguments[2];
    return true;
}

/*
 * One field of an `Outcome` the caller is holding, as the value that constructor carries.
 *
 * A *place* and not a node, because that is what a sum's payload is: `Proceed(v)` binds `v` by
 * projecting the storage the outcome is in, exactly as a constructor pattern does, and reading it
 * owned is the move out that a payload with a teardown needs. The flat field list is
 * `doc/spec/types.md`'s sum layout - one list, a range per constructor - so the field asked for is
 * the arm's first, both arms carrying one value.
 */
Node* Resolver::outcomePayload(Builder& builder, Place& outcome, U16 constructor, LocationId source) {
    auto& record = *(RecordType*)decl()[outcome.occurrence.type];
    auto arm = record.cons.get(decl(), constructor);

    auto place = builder.fieldPlace(outcome, arm.first);
    if(!place) return nullptr;

    return readPlace(builder, source, *place, bindingRequest(Access::Owned));
}

// Whether the outcome the caller is holding took its `Exit` arm - one discriminant read and one
// comparison, which is `resolveConstructorPattern`'s test with the constructor already decided.
Node* Resolver::outcomeIsExit(Builder& builder, Place& outcome, LocationId source) {
    auto tag = tagTypeOf(decl(), program.types, outcome.occurrence.type);
    if(!tag) return nullptr;

    auto place = builder.discriminantPlace(outcome);
    auto read = readPlace(builder, source, *place, inspectRequest());
    if(!read) return nullptr;

    auto& record = *(RecordType*)decl()[outcome.occurrence.type];
    auto arm = record.cons.get(decl(), kOutcomeExit);

    auto number = builder.addConst(source, intConstant(tag, U64(arm.value)));
    return builder.addBinary(source, BinaryOp::Eq, *read, *number);
}

/*
 * The carrier `?` hands back when it leaves, and the `return` that carries it.
 *
 * Two shapes, and they are `resolveReturn`'s own two rather than anything this file invents - which
 * is the point of it being written here: `?` departs exactly where a written `return` in the same
 * position departs, so a skipping traversal's failure channel and an ordinary function's result are
 * reached the same way from both.
 *
 *  - A **skipping traversal** declared `| E`, whose failure is the wrapper's other arm. `fromExit`
 *    would give the same value through `Try(Outcome(a, e), a, e)`, and building the arm directly is
 *    what keeps the one place that knows a traversal is failing in `resolve_stmt.cpp`'s hands.
 *  - Every other function, whose result type has to be one a carrier can be rebuilt at - which is
 *    the rule part 5 calls out, and the diagnostic for it names the *signature*: `?` in a function
 *    returning `Int` is a mistake about the signature, and a message about the operand would send
 *    the reader to the one part of `let v = parse(text)?` that is fine.
 */
bool Resolver::emitCarrierExit(Builder& builder, Node& reason, const TryShape& leaving,
                               LocationId source) {
    auto& function = *currentFunction;

    if(function.failure) {
        auto value = coerce(builder, reason, function.failure, source, "a failure value"_v);
        if(!value) return false;

        Node* only = value;
        auto wrapped = builder.addAggregate(source, function.result.type, kOutcomeExit, &only, 1);
        if(!wrapped) return false;

        builder.addReturn(source, *wrapped);
        return true;
    }

    auto value = coerce(builder, reason, leaving.reason, source, "the reason this function exits with"_v);
    if(!value) return false;

    Node* operands[1] = { value };
    auto carrier = applyMember(builder, intrinsic::Known::carrier, kTryFromExit, operands, 1, source,
                               function.result.type);
    if(!carrier) return false;

    carrier = coerce(builder, *carrier, function.result.type, source, "a returned value"_v,
                     function.result.contract.access());
    if(!carrier) return false;

    if(!checkResultContract(*carrier, source)) return false;

    builder.addReturn(source, *carrier);
    return true;
}

/*
 * Where `?` may be written at all, asked of the function it would **leave**.
 *
 * Both are about a traversal, and both are the same sentence read at a different declaration: `?`
 * takes the enclosing return edge, and a construct that answers by calling its continuation does not
 * have one to take.
 *
 * The other stage refuses a third - an `=` body, whose result its own body decides, so the type `?`
 * needs does not exist when `?` asks for it. There is no such case here: this stage does not infer a
 * result from an `=` body at all (`resolveSignature` gives one that writes no result type `{}`), so
 * what a `?` there meets is the ordinary message about `{}` having no `Try` instance, which is true.
 * The arm arrives with the inference.
 */
bool Resolver::canLeaveWithCarrier(LocationId source) {
    auto& function = *currentFunction;

    if(function.iterator) {
        context.diagnostics.error("`?` leaves the enclosing function, and an `iter fn` ends by running out of values rather than by returning - what it produces is the loop's own signal, which is not a carrier `?` can rebuild"_v,
                                  source);
        return false;
    }

    if(function.lens && !function.failure) {
        context.diagnostics.error("`?` cannot be written in a transparent `lens fn` - it answers by calling its continuation, so it has no return edge of its own for `?` to take. A lens that may skip declares the carrier it skips through with `| E`, and `?` is accepted in one of those"_v,
                                  source);
        return false;
    }

    return true;
}

/*
 * `x?` - one branch, and a `return` on the arm that did not proceed.
 *
 *     match Try.toOutcome(x):
 *       Proceed(v) -> v
 *       Exit(e)    -> return Try.fromExit(e)
 *
 * The exit arm is written **first** because that is the order `armsValue` lays alternatives out in:
 * an alternative emits its own test as a branch to the next one's entry, so the arm that has a test
 * to make comes before the arm that is whatever is left. Which of the two answers the expression is
 * then decided by which of them reaches the merge, and only the proceeding one does.
 */
Node* Resolver::resolveTry(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint, Request wanted) {
    auto source = expr.source;
    if(!canLeaveWithCarrier(source)) return nullptr;

    /*
     * The operand, resolved **owned**, because `toOutcome(->value: m)` consumes it.
     *
     * Asking for ownership here rather than letting the argument conversion ask is what makes the
     * refusal name the storage: `m?` over a borrowed carrier is reported by `readPlace` as storage
     * this function cannot hand over, which is the message with the fix in it.
     */
    auto value = resolveValue(builder, *parse[expr.tryValue], nullptr, requestOf(ownedContract()));
    if(!value) return nullptr;

    auto carrier = valueType(*value);

    TryShape carried;
    if(!carrierShape(carrier, source, carried)) {
        context.diagnostics.error("`?` needs %@ to say which of its cases means \"carry on\", and it has no `Try` instance"_v,
                                  source, typeText(context, decl(), carrier));
        return nullptr;
    }

    /*
     * And what the *signature* can be rebuilt from, which is the half that is about the enclosing
     * function rather than about the operand.
     *
     * A skipping traversal is exempt: its failure channel is `| E` and needs no instance, because
     * the wrapper it exits through is the compiler's own `Outcome` - see `emitCarrierExit`.
     */
    TryShape leaving;
    auto& function = *currentFunction;

    if(!function.failure && !carrierShape(function.result.type, source, leaving)) {
        context.diagnostics.error("`?` returns from the enclosing function when it exits, so that function's result type has to be one it can rebuild - %@ has no `Try` instance"_v,
                                  source, typeText(context, decl(), function.result.type));
        return nullptr;
    }

    Node* operand[1] = { value };
    auto outcome = applyMember(builder, intrinsic::Known::carrier, kTryToOutcome, operand, 1, source);
    if(!outcome) return nullptr;

    // The outcome in storage of its own, because both arms *project* it: a payload is a field of a
    // sum, and a field needs a place. `hold` is the same temporary a `match` over a call result
    // makes, which is what keeps the two constructs one.
    auto held = hold(builder, *outcome, source);
    if(!held) return nullptr;

    return armsValue(builder, source, 2, hint, true,
        [&](U32 index, Block* onFail, DeclPtr<Type> armHint, Carry& carry) -> Node* {
            if(index == 0) {
                auto exiting = outcomeIsExit(builder, *held, source);
                if(!exiting) return nullptr;

                failIfNot(builder, *exiting, *onFail, carry, source);

                auto reason = outcomePayload(builder, *held, kOutcomeExit, source);
                if(!reason) return nullptr;

                emitCarrierExit(builder, *reason, leaving, source);
                return nullptr;
            }

            auto proceeded = outcomePayload(builder, *held, kOutcomeProceed, source);
            if(!proceeded || !armHint) return proceeded;

            return coerce(builder, *proceeded, armHint, source, "a value"_v, wanted.access,
                          wanted.establishes);
        });
}

/*
 * `a?.b` - optional chaining, and the join at the end of the chain rather than at the return edge.
 *
 * The skip's extent is **syntactic**, which is what makes this implementable without any of the
 * continuation machinery a lens needs: what a `?.` skips is a span of AST nodes rather than the rest
 * of a block, so nothing is lifted and nothing is called. One branch per `?.`, one join, and the
 * whole thing threaded through blocks exactly as an `if` is.
 *
 * What is written *after* the `?.` never reaches `resolveUnwrap` at all. `a?.b` is a field of an
 * unwrap, `a?.[i]` a subscript of one and `a?.(x)` a call of one - three spellings, one node, and
 * `resolveField` and the rest need no case. Getting that wrong first, in the other stage, would have
 * meant a value-taking split of each of them.
 */

/*
 * The nodes of the chain `expr` tops, innermost last.
 *
 * A chain is a name with call, subscript, field and `?.` suffixes on it - the same spine the parser
 * builds, walked back down. **Not through `Nested`**, deliberately: parentheses end a chain, so
 * `(a?.b).c` is a field read of the `Maybe` the parenthesised chain produced, which is the whole
 * point of being able to write them.
 */
static void optionalSpine(ast::ParseBase parse, ast::Expr& expr, SmallArray<ast::Expr*, 8>& out) {
    auto node = &expr;

    while(node) {
        out.push(node);

        switch(node->kind) {
            case ast::Expr::Field:  node = &parse[node->field]->target; break;
            case ast::Expr::Unwrap: node = parse[node->unwrap]; break;
            case ast::Expr::App:    node = &parse[node->app]->callee; break;
            case ast::Expr::Sub:    node = &parse[node->sub]->callee; break;
            default: return;
        }
    }
}

bool Resolver::chainSkips(ast::Expr& expr) {
    SmallArray<ast::Expr*, 8> spine;
    optionalSpine(parse, expr, spine);

    return spine.contains([](ast::Expr* node) { return node->kind == ast::Expr::Unwrap; });
}

/*
 * One `?.` inside a chain - everything before the join.
 *
 * Unwrap, branch, and carry on down the proceeding arm with the payload in hand. The skip arm is
 * *recorded* rather than finished, because what it has to produce is the chain's own result type and
 * the chain has not been resolved yet - so what is kept is the block and the outcome's storage, and
 * the join comes back for both once there is a type for them to answer with.
 */
Node* Resolver::resolveUnwrap(Builder& builder, ast::Expr& expr) {
    auto source = expr.source;

    /*
     * Reached without a chain to skip to, which is a resolver mistake rather than a program one: the
     * dispatch enters `resolveOptionalChain` at the top of any chain containing one of these, so
     * every `?.` that is resolved at all is resolved inside that span.
     */
    if(!optionalChain) {
        context.diagnostics.error("internal: `?.` resolved outside the chain it skips to the end of"_v,
                                  source);
        return nullptr;
    }

    // Owned, because `toOutcome(->value: m)` consumes the carrier - the same sentence `?` is under,
    // and the same reason the refusal for a borrowed carrier names the storage rather than the class.
    auto value = resolveValue(builder, *parse[expr.unwrap], nullptr, requestOf(ownedContract()));
    if(!value) return nullptr;

    auto carrier = valueType(*value);

    TryShape carried;
    if(!carrierShape(carrier, source, carried)) {
        context.diagnostics.error("`?.` needs %@ to say which of its cases means \"carry on\", and it has no `Try` instance"_v,
                                  source, typeText(context, decl(), carrier));
        return nullptr;
    }

    /*
     * The **first** `?.` decides what the chain comes back in.
     *
     * A later one in the same chain may hold a different carrier - `a?.b?.c` where the two are a
     * `Maybe` and a `Result` - and only their *reasons* have to meet, which the join does with the
     * ordinary widening step every other position uses.
     */
    if(!optionalChain->carrier) optionalChain->carrier = carrier;

    Node* operand[1] = { value };
    auto outcome = applyMember(builder, intrinsic::Known::carrier, kTryToOutcome, operand, 1, source);
    if(!outcome) return nullptr;

    auto held = hold(builder, *outcome, source);
    if(!held) return nullptr;

    auto proceeding = outcomeIsExit(builder, *held, source);
    if(!proceeding) return nullptr;

    proceeding = builder.addUnary(source, UnaryOp::Not, *proceeding);
    if(!proceeding) return nullptr;

    // The skip block, entered when the test fails - which is `failIfNot`'s own shape, with the
    // carried values declared on both sides of the branch and rebound on the one we carry on in.
    auto skip = builder.addBlock(StringId());
    optionalChain->carry->declare(builder, *skip);

    failIfNot(builder, *proceeding, *skip, *optionalChain->carry, source);

    optionalChain->skips.push(OptionalSkip { skip, held, carried.reason, source });

    auto payload = outcomePayload(builder, *held, kOutcomeProceed, source);
    if(!payload) return nullptr;

    if(decl()[carried.proceeds]->kind == Type::Unit) {
        context.diagnostics.error("`?.` unwraps %@ into %@, which carries no value for the rest of this chain to be about"_v,
                                  source, typeText(context, decl(), carrier),
                                  typeText(context, decl(), carried.proceeds));
        return nullptr;
    }

    return payload;
}

/*
 * The whole chain, from its topmost node.
 *
 * Entered before anything below is resolved, because the join every `?.` in it branches to has to
 * exist first and because the values the branches cross have to be snapshotted at the top. What is
 * resolved underneath is the ordinary chain - the `?.` nodes find this through `optionalChain` and
 * add themselves to it - so nothing here duplicates a field access or a call.
 *
 * The layout is `armsValue`'s, spelled out rather than reused: the alternatives here are not written
 * side by side and cannot be, because one of them *is* the chain and the others branch out of the
 * middle of it. What is kept from that construct is every rule about the merge - one occurrence,
 * every edge supplying it, the carried values snapshotted per arm and replayed on the edge.
 */
Node* Resolver::resolveOptionalChain(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint) {
    auto source = expr.source;

    OptionalChain chain;
    Carry carry(*this, source);
    chain.carry = &carry;

    optionalSpine(parse, expr, chain.spine);

    auto join = builder.addBlock(StringId());

    // Saved and restored rather than assigned, so that a chain written inside this one's *arguments*
    // is its own: `a?.b(c?.d)` has two joins, and the inner `?.` belongs to the inner one.
    auto outer = optionalChain;
    optionalChain = &chain;

    /*
     * The chain itself, resolved **owned**, and not at whatever the surrounding position asked for.
     *
     * What consumes the chain's answer is `rewrap(->value: b)`, one line down, and the position the
     * outer expression is in has nothing to say about that: a `?.` in an argument that borrows still
     * has to hand the payload to the wrapper before anything borrows the wrapper. The outer request
     * applies to what this function returns, which is the merge, and is spent by whoever coerces it.
     */
    auto produced = resolveValue(builder, expr, nullptr, requestOf(ownedContract()));

    optionalChain = outer;

    // No `?.` reached its branch, which is a chain whose first one could not resolve at all. There
    // is nothing to rewrap and nothing to join.
    if(!chain.carrier || chain.skips.isEmpty()) return produced;
    if(!produced) return nullptr;

    /*
     * What the chain comes back in - `Rewrap(m, b -> n)`, the one question `Try` cannot answer.
     *
     * `Try` reads a carrier to what is inside it; this has a carrier and a payload and needs the
     * wrapper around *that* payload. The dependency is what makes it a lookup: `Maybe(Row)` and
     * `String` give `Maybe(String)` off the head that matched, with nothing at the use site
     * constraining the answer.
     */
    auto payload = valueType(*produced);
    auto rewrapClass = program.roles.klass(intrinsic::Known::rewrap);

    InstanceMatch rewrapping;
    DeclPtr<Type> asked[3] = { chain.carrier, payload, nullptr };

    if(!rewrapClass ||
       !selectInstance(context, program, rewrapClass, asked, 3, source, false, rewrapping) ||
       rewrapping.arguments.size() != 3) {
        context.diagnostics.error("`?.` on %@ has to put what the rest of this chain produced - %@ - back into the same kind of wrapper, and there is no `Rewrap` instance saying which type that is"_v,
                                  source, typeText(context, decl(), chain.carrier),
                                  typeText(context, decl(), payload));
        return nullptr;
    }

    auto result = rewrapping.arguments[2];

    // And what that result exits with, which is what every skip's reason has to become.
    TryShape rebuilt;
    if(!carrierShape(result, source, rebuilt)) {
        context.diagnostics.error("`?.` produces %@ here, which has no `Try` instance to build its empty case with"_v,
                                  source, typeText(context, decl(), result));
        return nullptr;
    }

    /*
     * The mistake this operator is most likely to be met with, named before it becomes a conversion
     * error about a type the author did not write.
     *
     * `?.` and `?` look related and are not: a chain containing `?.` comes back **wrapped**, because
     * the skip goes to the end of the chain and not out of the function. Someone who expected Rust's
     * `?` writes `node(n)?.id` in a function returning `Int` and is otherwise told only that
     * `Maybe(Int)` is not `Int`, which is true and explains nothing.
     */
    if(hint && !sameType(result, hint) && sameType(payload, hint)) {
        context.diagnostics.error("`?.` produces %@ rather than %@ - a chain containing it comes back wrapped, because the skip goes to the end of the chain and not out of this function. `?` is the operator that leaves, so write `(x?).field` if that is what was meant"_v,
                                  source, typeText(context, decl(), result),
                                  typeText(context, decl(), hint));
        return nullptr;
    }

    SmallArray<Block*, 8> ends;
    SmallArray<Node*, 8> values;
    SmallArray<Node*, 16> saved;

    // The proceeding arm: what the chain produced, put back in the wrapper it came out of.
    Node* only = produced;
    auto wrapped = applyMember(builder, intrinsic::Known::rewrap, kRewrapRewrap, &only, 1, source,
                               result, chain.carrier);

    ends.push(builder.currentBlock());
    values.push(wrapped);
    carry.save(saved);

    /*
     * The skip arms, finished now that there is a type for them to produce.
     *
     * Each is entered holding the outcome its own `?.` built, so the reason is projected here rather
     * than before the branch - a payload read on the path where its constructor is not the live one
     * would be a move out of storage that holds something else. Each reason is then widened into the
     * result's, which is the ordinary one-step conversion every other position gets: `a?.b?.c` over
     * two carriers that disagree lands on that message and not on one about `Try`.
     */
    for(auto& skipped: chain.skips) {
        builder.setBlock(*skipped.block);
        carry.arrive(builder, *skipped.block, 0);

        auto reason = outcomePayload(builder, *skipped.outcome, kOutcomeExit, skipped.source);
        Node* empty = nullptr;

        if(reason) {
            reason = coerce(builder, *reason, rebuilt.reason, skipped.source,
                            "the reason this chain skips with"_v);
        }

        if(reason) {
            Node* one = reason;
            empty = applyMember(builder, intrinsic::Known::carrier, kTryFromExit, &one, 1,
                                skipped.source, result);
        }

        ends.push(builder.currentBlock());
        values.push(empty);
        carry.save(saved);
    }

    auto argument = mergeArms(builder, *join, ends, values, source);
    carry.declare(builder, *join);

    for(Size i = 0; i < ends.size(); i++) {
        (void)closeArm(builder, *ends[i], *join, argument ? values[i] : nullptr, argument != nullptr,
                 source, carry, saved.pointer() + i * carry.count());
    }

    builder.setBlock(*join);
    carry.arrive(builder, *join, argument ? 1 : 0);

    if(!argument) builder.addUnreachable(source);
    return argument;
}

} // namespace mir
