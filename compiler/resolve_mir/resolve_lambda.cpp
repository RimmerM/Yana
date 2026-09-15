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
 * A closure that must outlive its frame needs a **heap-placed** environment - D17, built 2026-09-07.
 * The block is allocated where the value is, its first word is the teardown that releases the
 * captures and hands the block back, and the value's second word carries that block's address with
 * its low bit set. So a heap closure *owns* its environment: it carries no origin, `Shapes::closure`
 * is not on it, and every rule that refused it above stops applying rather than being exempted.
 *
 * **Which of the two a lambda gets is decided by the position it fills**, and that is the one
 * decision this file makes that is not the spec's. An *owning* position - a `return`, a field, a
 * `->` parameter, an assignment into storage that already exists - is exactly where
 * `checkCarriedOrigins` fires, so it is where a frame environment cannot go; a borrowing one is
 * bounded by its call, which is what passing a callback is. The alternative was to decide it from
 * the ownership analysis, which knows more - and cannot be asked, because the *capture conventions*
 * depend on the answer and they are settled while the body is walked.
 *
 * **The conventions come from the binding**, which is what the spec says and what this does not get
 * to choose per use site: `&` is captured by exclusive borrow, and anything else by an independent
 * copy where the type is `TrivialCopy`, by a shared borrow at a frame environment, and by a
 * **consumed** capture at a heap one. Deciding it from the binding rather than from what the body
 * turns out to do is what makes the answer available at the moment the field is appended; the
 * alternative is a pre-scan for writes, which is a second reading of the body that would have to
 * agree with the first about what a write is.
 */

/*
 * How one captured binding travels into the environment.
 *
 * `Copy` is the spec's read-only case at a `TrivialCopy` type - "the two are indistinguishable to
 * any program that never writes the local, which is why the compiler is free to choose the copy". A
 * binding nothing may write *is* such a program, by construction, which is why the test is about the
 * binding and not about the body.
 */
static CaptureKind captureKindOf(DeclBase base, Local& outer, bool heap) {
    // Written through, so a copy cannot satisfy it: "a mutable borrow's contract is writing through
    // to the same storage, which a copy cannot satisfy". `TrivialCopy` grants no exemption here, for
    // the reason it never does at `&`. It is a loan at either placement, which is what makes a
    // heap-placed closure over a `&` binding still an escape and still refused.
    if(outer.writable) return CaptureKind::Unique;

    if(isTriviallyCopyable(base, outer.occurrence.type)) return CaptureKind::Copy;

    /*
     * The spec's **consumed** capture - "a closure that must outlive the frame that built it has to
     * own what it captures", read as the statement about placement it is.
     *
     * Only at a heap environment, and that is the whole rule rather than a heuristic: a frame
     * environment is storage this function still owns, so taking the binding's value into it would
     * move a value into a place its own frame is about to release. The enclosing binding is
     * inaccessible afterwards, which nothing here has to arrange - the read below is a relocation
     * and the move paths say the rest.
     */
    if(heap) return CaptureKind::Owned;
    return CaptureKind::Shared;
}

/*
 * A read of a captured name, for the semantic index - `index.h`.
 *
 * Asked after `findCapture` answered, so the capture is always there to be found: the name was
 * either captured before or appended to the environment by that call.
 */
void Resolver::recordCapture(LocationId source, StringId name) {
    if(!recording(context) || !currentFunction || lambdaFrames.size() <= lambdaFloor) return;

    auto& frame = lambdaFrames[lambdaFrames.size() - 1];

    for(U32 i = frame.captureFloor; i < captures.size(); i++) {
        auto& captured = captures[i];
        if(captured.name != name) continue;

        // The enclosing binding's declaration is the capture's identity, so every use of one name in
        // one lambda is one symbol - and the environment field is what the lifted body reads.
        Symbol symbol;
        symbol.kind = Symbol::Kind::Capture;
        symbol.module = currentFunction->module;
        symbol.function = currentFunction - *program.decl;
        symbol.payload = captured.field;
        symbol.name = name;
        symbol.definition = captured.outer ? captured.outer->source : kNullLocation;

        recordReference(context, source, symbol,
                        captured.outer ? captured.outer->occurrence.type : nullptr);
        return;
    }
}

/*
 * A binding of the enclosing function, named inside a lambda.
 *
 * Answers the place the *lambda's* body reads it through, appending a field to the environment the
 * first time the name is seen. Null where the name is not an enclosing binding at all, which is what
 * sends `resolvePlace` on to the globals and then to its one "unknown name" report.
 */
Place* Resolver::findCapture(Builder& builder, StringId name) {
    // The floor, not zero: a body built on demand inside another one - `requireResult` - must not
    // reach a lambda frame the enclosing walk left open. See `Resolver::lambdaFloor`.
    if(lambdaFrames.size() <= lambdaFloor) return nullptr;

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

    auto kind = captureKindOf(decl(), *outer, frame.heap);

    /*
     * A **borrow** into a heap-placed environment, which is the one thing the placement cannot fix.
     *
     * The block outlives this frame and a `&` capture is a pointer into it, so a closure that left
     * would be holding a reference to storage that is gone - and no rule downstream catches it,
     * because the value carries no origin once the environment is its own. So it is refused here,
     * where the binding that cannot travel has a name.
     *
     * `Unique` is the only kind that reaches this: a writable binding is captured by exclusive
     * borrow at either placement (a copy cannot satisfy writing through), and everything else at a
     * heap environment is a copy or a consumed capture.
     */
    if(frame.heap && kind == CaptureKind::Unique) {
        context.diagnostics.error(
            "a closure that leaves this frame owns what it captured, and %@ is declared `&` - writing through a capture is a borrow of storage this frame is about to lose"_v,
            frame.lifted->source, context.findName(name));

        // And on as a consumed capture, which is what makes this one message: the field is appended,
        // so the next mention of the name finds it in the loop above instead of arriving here again
        // and reporting a second time. Nothing is emitted from a body that reported.
        kind = CaptureKind::Owned;
    }

    RecordField field;
    field.name = name;
    field.source = outer->source;
    field.index = U16(environment.fields.size());

    if(kind == CaptureKind::Copy || kind == CaptureKind::Owned) {
        // A member the environment owns, and one field shape for the two ways of getting one: a
        // second value where the type refuses no duplication, and the binding's own value where it
        // does. Which of them the read below performs is `requestOf` at an owned position, so there
        // is nothing here that has to know.
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

    /*
     * And only while the *lifted* body is what is being built.
     *
     * The frame stays on the stack after the walk, because `buildEnvironment` needs what it
     * accumulated - so a failed read emitted into the *enclosing* builder gets here too, and a local
     * of that body whose index happens to equal the environment parameter's reads as a capture. That
     * is a message about the wrong construct, and one of the two the caller chooses between.
     */
    if(&builder.function != frame.lifted) return false;

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
     * A **frame** environment holds copies and borrows and owes nothing, which is what lets a
     * closure that stays here have no teardown at all - see `ownershipOfOccurrence`, where the
     * closure shape is what says so. A **heap** one may own its captures, and what releases them is
     * the teardown in front of the block; both are checked at the far end rather than here, because
     * the far end is where a release would have to be emitted.
     */
    assertTrue(frame.heap || !hasTeardown(decl(), pointer));

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
        auto value = readPlace(builder, source, *place, requestOf(declared));
        if(!value) return nullptr;

        components.push(value);
    }

    auto value = builder.addAggregate(source, pointer, 0, components.pointer(), components.size());
    if(!value) return nullptr;

    /*
     * The storage, as a **local** of this frame.
     *
     * A place and not a value at either placement, because the record is built field by field and a
     * field needs somewhere to be written. What differs is what leaves this function with it: at a
     * frame environment the local stays here and the closure borrows it, and at a heap one the
     * closure *takes* it - the block the lowering allocates is filled from these bytes and the local
     * is moved out of, so this frame releases neither the captures nor the storage.
     */
    auto local = builder.addLocal(environment.name, owned(pointer), source, false);
    local->environment = true;

    auto place = builder.localPlace(*local);
    builder.addWrite(source, *place, *value, Destination::Init);

    /*
     * Owned where the environment is heap-placed, published otherwise.
     *
     * Published means the read is a loan rather than a copy of the two words - see `Request` - and
     * it is that loan which ties the value to this frame. A heap environment has no such tie by
     * construction, so the read is a relocation and the value it produces is the pair's own.
     */
    if(frame.heap) return readPlace(builder, source, *place, bindingRequest(Access::Owned));

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
    ErrorMark mark(context, parseErrors);
    Builder builder(context, program, lifted);

    LambdaFrame frame;
    frame.lifted = &lifted;
    frame.thunk = thunk;
    frame.heap = lifted.environmentStorage != StorageClass::Frame;
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

        bindLocal(local->name, local);
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

    /*
     * Whether the body **is** the value, which for a lambda is not the question `implicitReturn`
     * answers.
     *
     * `(x) -> e` and `(x): e` are one grammar production apart and the parser records which was
     * written, because for a `fn` the two mean different things - `doc/spec/functions.md`'s rule
     * that a `:` body returns `{}`. For a *lambda* they do not, and the shipping stage says so by
     * ignoring the flag entirely: `(): g.id` is a closure answering `g.id` over there, and refusing
     * it here was reading a `fn`'s rule at a position that has no declaration to read it from.
     *
     * So the rule is `resolveArm`'s, which is the one place this stage already states it: a position
     * that wants a value asks the body for one, and a body whose job is to **leave** - a `return`, or
     * a block ending in one - is resolved as the statement it is and answers nothing. A `{}` result
     * wants no value at all and takes the statement path unchanged, which is where every `:` lambda
     * written for its effect already was.
     */
    auto answers = implicitReturn || (!unit && !leavesBlock(body));

    if(open && !answers) {
        unsupported(source, "a lambda whose body only returns in a position that leaves the result open"_v);
    } else if(answers) {
        auto value = resolveValue(builder, body, (unit || open) ? nullptr : lifted.result.type,
                                  unit ? inspectRequest() : requestOf(lifted.result));

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

    // As for any body - see `resolveBody`.
    if(recording(context) && (mark.reported() || parseErrors != mark.parseErrorsBefore)) {
        lifted.mir = nullptr;
    }

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
 * The position a lambda that wrote its own parameter types *is* - the second half of `resolveLambda`.
 *
 * `fn double(x: Int) = x * 2` states a header and lets its body decide the result, and
 * `(x: Int) -> x * 2` is the same declaration without a name: every parameter says what it is, and
 * the only thing left open is what the body answers. So this builds the position out of the written
 * list and leaves the result `Types::undecided`, which is the state G7c already has a walk for -
 * `resolveLiftedBody` reads the answer off the body and `closeFunPosition` writes it back into the
 * shape.
 *
 * Null where a parameter states nothing, which is the case the diagnostic above is for and is the
 * whole of the boundary: `(v) -> v + 1` at a `let` has no header, no position and therefore no type,
 * and inferring one from the uses of the binding is a second inference this stage does not have.
 *
 * The occurrences come from `resolveOccurrence` at `Position::Parameter`, which is the same function
 * a written `(A, B) -> C` type goes through - a lambda's header is a signature and reading it a
 * second way here is how two readings of one grammar come to disagree. An origin published inside it
 * is refused for that reading's reason: a slot is numbered in the scope of the declaration that
 * wrote it, and a lambda's is a local of the frame it stands in.
 */
DeclPtr<Type> Resolver::writtenLambdaPosition(ast::FunExpr& written, LocationId source) {
    SlotScope slots;
    SmallArray<FunParam, 4> params;

    for(auto argument: written.args.contents(parse)) {
        if(!argument.type) return nullptr;

        Occurrence occurrence;
        if(!resolveOccurrence(*parse[argument.type], argument.bind, Position::Parameter, slots,
                              occurrence)) {
            return nullptr;
        }

        params.push(FunParam { occurrence });
    }

    if(slots.count()) {
        unsupported(source, "an origin published on a lambda's own parameter"_v);
        return nullptr;
    }

    return program.types.funType(decl(), params.pointer(), params.size(),
                                 owned(program.types.undecided));
}

/*
 * `(x) -> x * n` - the lambda, from the position that says what it is, or from its own header.
 *
 * The position says what the parameters and the result are wherever it states them, which is the
 * rule a named function as a value is already under. What the author may write is a parameter type
 * and a parameter convention, and where they do it is checked against the position rather than
 * believed.
 *
 * Where the position states **nothing** - a `let`, a field of a function type this stage has not
 * decided, an ascription that named no shape - a lambda that wrote every one of its parameter types
 * is its own position: see `writtenLambdaPosition`. One rule and not a list of shapes, deliberately,
 * because the alternative is a rule per syntactic place the lambda was found in and they would have
 * to agree. A lambda that left a parameter open still has nowhere to take it from and is refused
 * here, which is the message below.
 */
Node* Resolver::resolveLambda(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint,
                              const Request& wanted) {
    auto& written = *parse[expr.fun];

    if(written.kind != ast::FunKind::Plain) {
        unsupported(expr.source, "a lens or iterator lambda"_v);
        return nullptr;
    }

    if(!hint || decl()[hint]->kind != Type::Fun) {
        hint = writtenLambdaPosition(written, expr.source);

        if(!hint) {
            context.diagnostics.error(
                "a lambda takes its parameters and its result from the position it fills, and this position states neither - write the type, or write a type on every parameter"_v,
                expr.source);
            return nullptr;
        }
    }

    auto& shape = *(FunType*)decl()[hint];

    if(written.args.size() != shape.params.size()) {
        context.diagnostics.error("this position takes %@ arguments and this lambda writes %@"_v,
                                  expr.source, U64(shape.params.size()), U64(written.args.size()));
        return nullptr;
    }

    /*
     * The position's own convention, which is what decides where the environment goes - see the note
     * at the top of this file. `Access::Owned` is the receiver claiming the value is now theirs, and
     * a receiver that outlives this frame is exactly what a heap-placed environment is for.
     */
    return liftFunction(builder, &written.args, written.body, written.implicitReturn, hint,
                        expr.source, LiftKind::Lambda, wanted.access == Access::Owned);
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
    // Never an owning position: `declareDeferred` rewrites a `@lazy` parameter to a shared,
    // call-bounded `() -> T`, so a thunk's environment is a local of the caller's frame by
    // construction and there is nothing here to rank.
    return liftFunction(builder, nullptr, body, true, hint, source, LiftKind::Thunk, false);
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
        auto value = constantValue(inner, parameter.def, parameter.source);
        if(!value) return nullptr;

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
                             LiftKind kind, bool owningPosition) {
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
     * Where the environment goes, ranked by the one function that ranks storage - D6's split, spent
     * here for the first time by something that is not a `@heap` attribute.
     *
     * `outlivesFrame` is the finding and `selectStorage` is the decision, so a region rung arriving
     * later is a case in that function rather than a second rule here. The finding is the position's
     * convention, which `checkCarriedOrigins`' own condition is: an owned position claims the value
     * is the receiver's, and a value that refers to this frame may not be.
     */
    Placement asked;
    asked.outlivesFrame = owningPosition;
    lifted->environmentStorage = selectStorage(asked);

    // What tells the *MIR* reachability walk the allocator is needed - `usesHeapPlacement`, the
    // same producer a heap-placed local is and for the same reason: the environment is an object
    // whose placement left the frame, and `resolve_ssa/opt/reach.cpp` asks again per `fun.value`.
    if(lifted->environmentStorage != StorageClass::Frame) program.usesHeapPlacement = true;

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

    /*
     * A lambda that captured nothing has no environment to place, whatever the position asked for.
     *
     * The rung was selected before the walk because `captureKindOf` needed it, and this is where it
     * is known to have been about nothing: the record has no constructor, so asking for its layout
     * is asking for the layout of an unfinished type. The value is a null environment word either
     * way, which is what makes a non-capturing lambda the same value a named function is.
     */
    if(!capturing) lifted->environmentStorage = StorageClass::Frame;

    if(capturing) held = buildEnvironment(builder, frame, source);

    captures.resize(frame.captureFloor);
    lambdaFrames.resize(U32(lambdaFrames.size() - 1));

    if(capturing && !held) return nullptr;

    // The position, with the result the body decided written into it - see `closeFunPosition`. It is
    // `hint` itself wherever the position was already closed, which is every lambda but a G7c one.
    auto type = closeFunPosition(context, program, hint, lifted->result.type);

    return builder.addFunValue(source, DeclPtr<Function>(lifted - *program.decl), type, held,
                               lifted->environmentStorage != StorageClass::Frame);
}

} // namespace mir
