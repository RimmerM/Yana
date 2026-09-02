#include "resolve_internal.h"
#include "own.h"

namespace mir {

/*
 * Lambdas and closures - `doc/spec/functions.md`'s Closures, and Analysis-IR.md §11.4's G7b.
 *
 * A lambda is **lifted**: it becomes an ordinary function of this module with one parameter in front
 * of the ones the author wrote, and the expression becomes a `fun.value` naming it. That is the
 * whole of the construct, and everything below answers one of two questions about it - what the
 * extra parameter holds, and how long the storage behind it lives.
 *
 * **What it holds.** The captures, as a record synthesized while the body is walked. A capture is
 * not found by a scan: it is a name the body says that the *enclosing* function bound, so the body
 * walk is what finds it and the environment's field list is appended to as it goes. That is possible
 * because a capture is a **place** rather than an instruction - the body reads a field of the
 * environment parameter, and a place is interned rather than emitted - so a field that does not
 * exist when the lambda's first statement is resolved can exist by the time its last one is.
 *
 * **How long the storage lives**, which is the harder half. The environment is a local of the
 * *enclosing* frame and the value refers to it, so a closure is a value that refers to somebody
 * else's storage - which this stage has had a vocabulary for since milestone 3. It is spelled as an
 * owned pair with one **interior** position (`Shapes::closure`), and from there every existing rule
 * applies with nothing added to it: the origin analysis roots the value in the loans the environment
 * took, `checkReturnConformance` refuses a result that carries one, and the loan checker holds a `&`
 * capture to its exclusivity for as long as the closure is live.
 *
 * A closure that must outlive its frame needs a **heap-placed** environment, and this stage has no
 * allocator. `StorageClass::Heap` is therefore a rung nothing selects - `verifyBody` refuses an
 * object on it, exactly as it refuses one in a region with no witness - and the escapes that would
 * have needed one are reported where they are written, in terms of the placement they would need.
 *
 * **The conventions come from the binding**, which is what the spec says and what this does not get
 * to choose per use site: `&` is captured by exclusive borrow, and anything else by shared borrow or
 * by an independent copy where the type is `TrivialCopy`. Deciding it from the binding rather than
 * from what the body turns out to do is what makes the answer available at the moment the field is
 * appended; the alternative is a pre-scan for writes, which is a second reading of the body that
 * would have to agree with the first about what a write is.
 */

/*
 * How one captured binding travels into the environment.
 *
 * `Copy` is the spec's read-only case at a `TrivialCopy` type - "the two are indistinguishable to
 * any program that never writes the local, which is why the compiler is free to choose the copy". A
 * binding nothing may write *is* such a program, by construction, which is why the test is about the
 * binding and not about the body.
 */
static CaptureKind captureKindOf(DeclBase base, Local& outer) {
    // Written through, so a copy cannot satisfy it: "a mutable borrow's contract is writing through
    // to the same storage, which a copy cannot satisfy". `TrivialCopy` grants no exemption here, for
    // the reason it never does at `&`.
    if(outer.writable) return CaptureKind::Unique;

    if(isTriviallyCopyable(base, outer.occurrence.type)) return CaptureKind::Copy;
    return CaptureKind::Shared;
}

/*
 * A binding of the enclosing function, named inside a lambda.
 *
 * Answers the place the *lambda's* body reads it through, appending a field to the environment the
 * first time the name is seen. Null where the name is not an enclosing binding at all, which is what
 * sends `resolvePlace` on to the globals and then to its one "unknown name" report.
 */
Place* Resolver::findCapture(Builder& builder, StringId name) {
    if(!lambdaFrames.size()) return nullptr;

    auto& frame = lambdaFrames[lambdaFrames.size() - 1];
    auto& environment = *(RecordType*)decl()[frame.lifted->environment];

    // Already captured, and therefore already a field. The place is re-interned rather than stored,
    // because interning is what makes two projections of one path one record - see `Place`.
    for(U32 i = frame.captureFloor; i < captures.size(); i++) {
        if(captures[i].name != name) continue;
        return builder.fieldPlace(*builder.localPlace(*frame.parameter), captures[i].field);
    }

    /*
     * The enclosing scope, innermost first, and stopping where the enclosing walk itself stopped.
     *
     * The same backward walk `findLocal` makes over the same array, over the range the lambda's own
     * floor excludes: `[outerFloor, floor)` is exactly the names that were in scope where the lambda
     * was written.
     */
    Local* outer = nullptr;
    for(U32 i = frame.floor; i > frame.outerFloor; i--) {
        if(bindings[i - 1].name != name) continue;
        outer = bindings[i - 1].local;
        break;
    }

    if(!outer) return nullptr;

    auto kind = captureKindOf(decl(), *outer);

    RecordField field;
    field.name = name;
    field.source = outer->source;
    field.index = U16(environment.fields.size());

    if(kind == CaptureKind::Copy) {
        // A member the environment owns: a second value of a type that owes nothing and refuses no
        // duplication, taken when the closure is built.
        field.occurrence = owned(outer->occurrence.type);
    } else {
        /*
         * An **external** field, in an origin set of its own.
         *
         * One slot per borrowed capture rather than one for all of them, because they are not one
         * object: §2.2 groups fields under a name and is explicit that grouping "does not claim
         * that the members are one object", so a shared slot would make reading one capture keep
         * another's loan live.
         */
        auto access = kind == CaptureKind::Unique ? Access::Unique : Access::Shared;
        field.occurrence = Occurrence { outer->occurrence.type,
                                        slotContract(access, environment.originSlots), nullptr };
        environment.originSlots++;
    }

    environment.fields.push(program.decl, field);
    captures.push(Capture { name, outer, field.index, kind });

    return builder.fieldPlace(*builder.localPlace(*frame.parameter), field.index);
}

/*
 * Whether a place is reached through the innermost lambda's environment - one prefix walk.
 *
 * Only for the *message* at a position that consumes, which is the one place the difference between
 * "this frame borrows it" and "this lambda captured it" changes what an author should do about it.
 */
bool Resolver::isCapture(Builder& builder, Place& place) {
    if(!lambdaFrames.size()) return false;

    auto& frame = lambdaFrames[lambdaFrames.size() - 1];
    if(!frame.parameter) return false;

    auto base = builder.base();
    auto root = &place;
    while(root->parent) root = base[root->parent];

    return root->kind == Place::Local && root->payload == frame.parameter->index.index;
}

/*
 * The environment's value, and the borrow of it the closure carries - emitted into the **enclosing**
 * builder, at the point the lambda was written.
 *
 * After the body has been walked, because that is when the field list is complete. Nothing was
 * emitted into this builder while the lambda was resolved - a lifted body is a body of its own, with
 * its own blocks - so the point the lambda stood at is still where this builder is.
 */
Node* Resolver::buildEnvironment(Builder& builder, LambdaFrame& frame, LocationId source) {
    auto pointer = frame.lifted->environment;
    auto& environment = *(RecordType*)decl()[pointer];

    /*
     * The environment's one constructor, now that its field list is complete.
     *
     * `RecordType::cons` is never empty - a synthesized record is a record, and every walk below
     * reads the constructor rather than testing for one. The range is stated *here* rather than
     * where the shell was made because the captures are appended as the body walk finds them, so
     * this is the first point at which there is a count to state.
     */
    RecordCon only;
    only.name = environment.name;
    only.source = source;
    only.count = U16(environment.fields.size());
    environment.cons.push(program.decl, only);
    environment.layout = RecordLayout::Single;

    /*
     * The schema, then the classification - the pair `instantiateRecord` runs, in the order it runs
     * them. The schema is what makes each borrowed field an origin position, which is what the
     * aggregate below is then read through.
     */
    foldRecordSchema(context, program, environment);
    classifyRecord(context, program, environment);

    /*
     * Nothing the environment holds owes a release: a copy is trivial and a borrow is somebody
     * else's. That is what lets a function value have no teardown at all - see `ownershipOf` - so it
     * is checked here, where the record is finished, rather than assumed at the far end where a
     * teardown would have had to be emitted.
     */
    assertTrue(!hasTeardown(decl(), pointer));

    SmallArray<Node*, 4> components;

    for(U32 i = frame.captureFloor; i < captures.size(); i++) {
        auto& capture = captures[i];
        auto place = builder.localPlace(*capture.outer);

        /*
         * The request is the field's own, which is what makes the three capture kinds one call.
         *
         * `published` is what separates a borrow from a copy at a trivially copyable type - see
         * `Request` - and an external field's contract has it set, so a `let &n = 0` capture takes
         * the loan the spec requires and a `let n = 0` one takes the copy the spec permits.
         */
        auto declared = environment.field(decl(), capture.field).occurrence;
        auto value = readPlace(builder, source, *place, requestOf(declared.contract));
        if(!value) return nullptr;

        components.push(value);
    }

    auto value = builder.addAggregate(source, pointer, 0, components.pointer(), components.size());
    if(!value) return nullptr;

    /*
     * The storage, as a **local** of this frame.
     *
     * A place and not a value, because the closure has to refer to it and a reference needs
     * somewhere to point. It is also where the placement decision lands: the local's object carries
     * a `StorageClass`, `Frame` is what this stage produces, and the day a closure may outlive its
     * frame is the day something here selects `Heap` instead.
     */
    auto local = builder.addLocal(environment.name, owned(pointer), source, false);
    local->environment = true;

    auto place = builder.localPlace(*local);
    builder.addWrite(source, *place, *value, Destination::Init);

    // Published, so that the read is a loan rather than a copy of the two words - see `Request`. It
    // is the loan that ties the value to the frame, and a copy would tie it to nothing.
    return readPlace(builder, source, *place, Request { Access::Shared, true });
}

/*
 * The lifted body, resolved as the function it now is.
 *
 * `Resolver::resolveBody`'s walk with two things changed, and both are about *nesting*: the caller's
 * names are hidden behind a floor rather than cleared, and the frame this pushes stays on the stack
 * after the walk, because the environment it accumulated is what the caller builds next.
 */
void Resolver::resolveLiftedBody(Function& lifted, ast::Expr& body, bool implicitReturn, bool thunk,
                                 LocationId source) {
    // The same mark an ordinary body takes - `ErrorMark`, and `finish` is what reads it.
    ErrorMark mark(context);
    Builder builder(context, program, lifted);

    LambdaFrame frame;
    frame.lifted = &lifted;
    frame.thunk = thunk;
    frame.outerFloor = bindingFloor;
    frame.floor = U32(bindings.size());
    frame.captureFloor = U32(captures.size());

    auto outerFunction = currentFunction;
    auto outerHeld = held;
    auto outerBindingFloor = bindingFloor;
    auto outerLoops = loops;
    auto outerContinuation = continuationLocal;
    auto outerYielded = yieldedLocal;
    auto outerGuard = guardYields;

    currentFunction = &lifted;

    /*
     * The walk's own stacks, which stop at the boundary because the *body* does.
     *
     * A lambda is another function: its blocks, its locals and its frame are its own, so nothing the
     * enclosing walk is in the middle of names anything reachable from here. Left standing, each is
     * a block or a local of one body read while another is being built - `break` inside a lambda
     * written in a loop is the one that was reachable from source, and it named the enclosing
     * function's exit block from a body that has no edge to it.
     *
     * So this is four fields for one rule rather than four rules: `held` for the caller's
     * temporaries, `loops` for the blocks a `break` may name, and the lens pair for the continuation
     * a `yield` calls - each of which now reports what it is, a construct written outside the
     * function it appears in.
     */
    held = nullptr;
    loops = nullptr;
    continuationLocal = nullptr;
    yieldedLocal = nullptr;
    guardYields = false;
    bindingFloor = frame.floor;

    GenScope generics(*this, lifted.gen);

    for(auto argumentPointer: lifted.args.contents(decl())) {
        auto local = builder.addParameter(*decl()[argumentPointer]);

        // The environment is argument zero and is not a name: the author wrote no such parameter,
        // and binding its synthesized symbol would put a name in scope that no source can say.
        if(local->parameter == 0) {
            frame.parameter = local;
            continue;
        }

        bindings.push(Binding { local->name, local });
    }

    lambdaFrames.push(frame);

    /*
     * A result the *position* left open - Analysis-IR.md §11.4's G7c.
     *
     * `fn (r) run(body: (I32) -> r) -> r` hands the lambda a shape whose result is still a variable,
     * and what decides it is this body. So the body is resolved against no result hint and its
     * answer becomes the lambda's result, which the caller then binds `r` from through the ordinary
     * argument walk - the same route a named function's declared result takes.
     *
     * The **implicit-return** form only, and that is where the information is rather than a
     * shortcut: a statement body decides its result at each `return`, and inferring one from several
     * is a join over types this stage does not do anywhere else.
     */
    auto open = !isConcreteType(decl(), lifted.result.type);
    auto unit = !open && decl()[lifted.result.type]->kind == Type::Unit;

    if(open && !implicitReturn) {
        unsupported(source, "a lambda with a statement body in a position that leaves the result open"_v);
    } else if(implicitReturn) {
        auto value = resolveValue(builder, body, (unit || open) ? nullptr : lifted.result.type,
                                  unit ? inspectRequest() : requestOf(lifted.result.contract));

        /*
         * What the body answered *is* the result, decided before anything is checked against it.
         *
         * A body that answered nothing decidable - an empty literal, a call that reported - leaves
         * the result open, and one message about that is better than the type mismatch every
         * position downstream would report against a variable.
         */
        if(value && open) {
            auto answered = valueType(*value);

            if(answered && isConcreteType(decl(), answered)) {
                lifted.result.type = answered;
                open = false;
            } else {
                context.diagnostics.error(
                    "this position does not say what the lambda answers, and its body does not decide one"_v,
                    body.source);
            }
        }

        if(value && !open && !builder.isTerminated()) {
            if(unit) {
                builder.addReturnUnit(body.source);
            } else if((value = coerce(builder, *value, lifted.result.type, body.source,
                                      "a returned value"_v)) &&
                      checkResultContract(*value, body.source)) {
                builder.addReturn(body.source, *value);
            }
        }
    } else {
        resolveStatement(builder, body);
    }

    finish(builder, lifted, unit, source, mark);
    builder.seal();

    bindings.resize(frame.floor);
    bindingFloor = outerBindingFloor;
    currentFunction = outerFunction;
    held = outerHeld;
    loops = outerLoops;
    continuationLocal = outerContinuation;
    yieldedLocal = outerYielded;
    guardYields = outerGuard;
}

/*
 * `(x) -> x * n` - the lambda, from the position that says what it is.
 *
 * The position and nothing else says what the parameters and the result are, which is the rule a
 * named function as a value is already under and for a stronger reason: a lambda writes no result
 * type at all, so there is nowhere else for one to come from. What the author may write is a
 * parameter type and a parameter convention, and where they do it is checked against the position
 * rather than believed.
 */
Node* Resolver::resolveLambda(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint) {
    auto& written = *parse[expr.fun];

    if(written.kind != ast::FunKind::Plain) {
        unsupported(expr.source, "a lens or iterator lambda"_v);
        return nullptr;
    }

    if(!hint || decl()[hint]->kind != Type::Fun) {
        context.diagnostics.error(
            "a lambda takes its parameters and its result from the position it fills, and this position states neither - write the type"_v,
            expr.source);
        return nullptr;
    }

    auto& shape = *(FunType*)decl()[hint];

    if(written.args.size() != shape.params.size()) {
        context.diagnostics.error("this position takes %@ arguments and this lambda writes %@"_v,
                                  expr.source, U64(shape.params.size()), U64(written.args.size()));
        return nullptr;
    }

    return liftFunction(builder, &written.args, written.body, written.implicitReturn, hint,
                        expr.source, LiftKind::Lambda);
}

/*
 * A `@lazy` argument, which is a lambda the author did not write - `doc/spec/functions.md`.
 *
 * "The argument expression compiles to a nullary thunk over the caller's frame" is the spec's
 * sentence and this is the whole of it: the position's type is `() -> T`, the expression is the
 * body, and every rule about what a closure may capture and how long its environment lives applies
 * unchanged because it *is* one. What the two forms do not share is a written parameter list, which
 * is why the lift takes one rather than a `FunExpr`.
 */
Node* Resolver::resolveThunk(Builder& builder, ast::Expr& body, DeclPtr<Type> hint,
                             LocationId source) {
    return liftFunction(builder, nullptr, body, true, hint, source, LiftKind::Thunk);
}

/*
 * What a call that leaves a **deferred defaulted** position out passes - `Arg::defThunk`.
 *
 * A `@lazy` position holds `() -> T`, so a default there cannot be the constant itself: what the
 * callee will do with it is call it. This builds the one function that answers it, whose whole body
 * is `return <constant>`.
 *
 * The constant is built **inside** the thunk rather than at the call site that omitted the position,
 * and that is the point rather than a saving. A value materialized in the caller and returned by the
 * thunk is a value of the caller's frame handed out of a call, which is the shape the ownership
 * passes are right to refuse; built here it is an ordinary result of an ordinary function, which is
 * what the author's own `f(x, "")` would have produced too.
 *
 * Not `liftFunction`, and not because it would be inconvenient: that one exists to lift a body out
 * of *the frame it was written in*, so everything it does is about captures - the environment
 * record, the two refusals that stop one being built where its storage could not live. This thunk
 * captures nothing by construction. So the two refusals do not apply to it, which is the whole
 * reason a `@lazy` default is callable from a generic body and from inside another lambda where a
 * written `@lazy` argument is not: there is no environment for a specialization to have wanted
 * differently.
 *
 * One per parameter for the whole program, in the module that **declared** it - the constant is that
 * declaration's, and a thunk per calling module would be several symbols saying one thing.
 */
Node* Resolver::defaultThunk(Builder& builder, Function& callee, Arg& parameter,
                             LocationId source) {
    if(!parameter.defThunk) {
        auto& shape = *(FunType*)decl()[parameter.occurrence.type];

        auto text = Tritium::format("%@.%@.default", context.findName(callee.symbol),
                                    context.findName(parameter.name));
        auto name = program.uniqueSymbol(callee.module, context,
                                         StringView(text.text(), text.size()));

        auto lifted = new (program.decl) Function(name, callee.module);
        lifted->source = parameter.source;

        // The position's own result occurrence, which `declareDeferred` built out of the type the
        // author wrote: a thunk answers an owned value however the parameter was spelled.
        lifted->result = shape.result;
        lifted->summary.bodyKnown = true;

        decl()[callee.module]->functions.push(program.decl,
                                              DeclPtr<Function>(lifted - *program.decl));

        Builder inner(context, program, *lifted);
        auto value = inner.addConst(parameter.source, parameter.def);
        inner.addReturn(parameter.source, *value);
        inner.seal();

        parameter.defThunk = DeclPtr<Function>(lifted - *program.decl);
    }

    // A null environment, which is what having captured nothing is spelled as - see `liftFunction`,
    // where the same word makes a lambda a value that refers to no frame at all.
    return builder.addFunValue(source, parameter.defThunk, parameter.occurrence.type, nullptr);
}

/*
 * A body lifted out of the frame it was written in - the lambda and the `@lazy` thunk.
 *
 * One function because the difference between them is what the author wrote and not what is built: a
 * lambda states its parameters and a thunk has none, and from the environment down there is nothing
 * left that could tell the two apart. `kind` reaches exactly two places - the symbol a dump shows,
 * and the two refusals, which have to name the construct the author wrote rather than the one this
 * function is making.
 */
Node* Resolver::liftFunction(Builder& builder, const ast::ParseList<ast::Arg>* args, ast::Expr& body,
                             bool implicitReturn, DeclPtr<Type> hint, LocationId source,
                             LiftKind kind) {
    auto lambda = kind == LiftKind::Lambda;
    auto what = lambda ? "a lambda"_v : "a `@lazy` argument"_v;

    auto& shape = *(FunType*)decl()[hint];

    /*
     * The two shapes whose environment would have to be somewhere this stage cannot put one, each
     * refused by name.
     *
     * A **nested** lambda reaches its captures through the enclosing lambda's environment, which is
     * a chain of places rather than one; a lambda in a **generic** body would want argument zero for
     * a generic environment and for captures at once, which is the one thing that position cannot
     * hold (see `Function::environment`).
     *
     * A lambda inside a **lens** call was a third and is not one any more: a lens is a function, so
     * a lambda written in a continuation is a lambda written in an ordinary block of an ordinary
     * body, and which frame its environment belongs to is the frame it is standing in.
     */
    if(lambdaFrames.size()) {
        auto enclosing = lambdaFrames[lambdaFrames.size() - 1].thunk ? "a `@lazy` argument"_v
                                                                     : "another lambda"_v;
        auto text = Tritium::format("%@ inside %@", what, enclosing);
        unsupported(source, stringView(text));
        return nullptr;
    }

    if(currentFunction && isGeneric(decl(), currentFunction->gen)) {
        auto text = Tritium::format("%@ in a generic function", what);
        unsupported(source, stringView(text));
        return nullptr;
    }

    /*
     * The lifted declaration and its environment, named after the function they were written in.
     *
     * A symbol and not something source can say: a lambda has no name, and what a dump and a symbol
     * table need is one that says where it came from. `uniqueSymbol` is what keeps two lambdas of
     * one function apart, on the terms every other synthesized symbol is under, and the environment
     * is named after the lambda because there is exactly one of it per lambda.
     */
    auto text = Tritium::format("%@.%@", context.findName(currentFunction->symbol),
                                lambda ? "lambda"_v : "thunk"_v);
    auto name = program.uniqueSymbol(here(), context, StringView(text.text(), text.size()));

    auto envText = Tritium::format("%@.env", context.findName(name));
    auto envName = program.uniqueSymbol(here(), context,
                                        StringView(envText.text(), envText.size()));

    auto environment = program.types.recordType(envName, here(), source);
    auto environmentPointer = asBase<Type>(DeclPtr<RecordType>(environment - *program.decl));

    auto lifted = new (program.decl) Function(name, here());
    lifted->source = source;
    lifted->environment = environmentPointer;
    lifted->result = shape.result;
    lifted->summary.bodyKnown = true;

    /*
     * The environment parameter, first and **exclusive**.
     *
     * Exclusive because a `&` capture is written through it: `fieldPlace` narrows a unique external
     * field to a shared view when the record was reached sharedly, so an environment reached as
     * shared would make every capture read-only and the spec's written case unreachable. What makes
     * that sound is not this parameter but the loan the *capture* took, which is exclusive for as
     * long as the closure is live.
     */
    auto envArg = new (program.decl) Arg();
    envArg->name = envName;
    envArg->occurrence = borrowed(environmentPointer, Access::Unique);
    envArg->source = source;
    envArg->index = 0;
    lifted->args.push(program.decl, envArg - *program.decl);

    // A thunk writes none, which is what makes it one: `args` is null and the loop below is the
    // whole of what a written parameter list adds. Copied out first because `contents` is not const.
    ast::ParseList<ast::Arg> writtenArgs;
    if(args) writtenArgs = *args;

    for(auto argument: writtenArgs.contents(parse)) {
        auto declared = shape.params.get(decl(), U32(lifted->args.size() - 1)).occurrence;

        if(argument.type) {
            auto type = resolveType(*parse[argument.type]);
            if(!type) return nullptr;

            if(!sameType(type, declared.type)) {
                context.diagnostics.error("this position takes %@ here, and the lambda writes %@"_v,
                                          argument.source, typeText(context, decl(), declared.type),
                                          typeText(context, decl(), type));
                return nullptr;
            }
        }

        /*
         * A written `&` or `->` against what the position asked for.
         *
         * Only a *written* one is compared, because there is no marker for a plain borrow: an
         * unwritten convention is the position's, and the author who wrote nothing has said nothing
         * to disagree with. What they did write is held to the position, because a lambda that
         * consumed a parameter the caller only lends is a second reading of one signature.
         */
        auto asked = argument.bind == ast::BindType::Sink   ? Access::Owned :
                     argument.bind == ast::BindType::Ref    ? Access::Unique :
                                                              Access::Shared;

        if(argument.bind != ast::BindType::Borrow && asked != declared.contract.access()) {
            context.diagnostics.error(
                "this parameter is %@ in the position's type, and the lambda writes %@"_v,
                argument.source, accessName(declared.contract.access()), accessName(asked));
            return nullptr;
        }

        auto declaredArg = new (program.decl) Arg();
        declaredArg->name = argument.name;
        declaredArg->occurrence = declared;
        declaredArg->source = argument.source;
        declaredArg->index = U32(lifted->args.size());
        lifted->args.push(program.decl, declaredArg - *program.decl);
    }

    /*
     * Into the module, once the header is known to be one - a parameter list that reported is a
     * lambda there is no body for, and the record it would have captured into was never classified.
     * Everything above this line is arena memory nothing names.
     */
    current->records.push(program.decl, DeclPtr<RecordType>(environment - *program.decl));
    current->functions.push(program.decl, DeclPtr<Function>(lifted - *program.decl));

    resolveLiftedBody(*lifted, body, implicitReturn, !lambda, source);

    /*
     * The environment, where the body turned out to name anything.
     *
     * A lambda that captured nothing is a plain function value with a null environment word, which
     * is the same value a named function produces - so it may be stored, returned and copied exactly
     * as one, and nothing about it refers to this frame. That is not an optimization: it is the
     * difference between a value that carries an origin and one that does not.
     */
    auto& frame = lambdaFrames[lambdaFrames.size() - 1];
    auto capturing = captures.size() > frame.captureFloor;
    Node* held = nullptr;

    if(capturing) held = buildEnvironment(builder, frame, source);

    captures.resize(frame.captureFloor);
    lambdaFrames.resize(U32(lambdaFrames.size() - 1));

    if(capturing && !held) return nullptr;

    // The position, with the result the body decided written into it - see `closeFunPosition`. It is
    // `hint` itself wherever the position was already closed, which is every lambda but a G7c one.
    auto type = closeFunPosition(context, program, hint, lifted->result.type);

    return builder.addFunValue(source, DeclPtr<Function>(lifted - *program.decl), type, held);
}

} // namespace mir
