#include "expr.h"
#include "analyze.h"
#include "generic.h"
#include "name.h"
#include "witness.h"

/*
 * Function values and closures.
 *
 * A function value is two words - `{code, env}` - and every callable has that shape, whatever it was
 * written as. A lambda that captured nothing, a lambda that captured five things and a plain
 * function referenced by name differ only in what the second word holds, and `code` always takes the
 * environment as its first parameter so that a call site never has to know which of the three it is
 * holding. That uniformity is the whole design: it is what lets a function type be a type rather
 * than a family of them, and it is what a `PropertyWitness` and an `any C` payload will be built on
 * later.
 *
 * What the environment *contains* is not in the value. It is static data in front of the lifted
 * function's entry point - the closure header - because a closure's captures are decided by which
 * lambda it came from, and `code` already names the lambda. See ClosureHeaderLayout.
 *
 * Lifting.
 *
 * A lambda body becomes an ordinary function of the module, resolved here and now rather than
 * queued: the bindings it captures are the enclosing resolver's, and those are scratch state that
 * exists only while that body is being resolved. Its arguments are the environment first and the
 * declared ones after, so the lifted function is nothing an ordinary one could not have been.
 *
 * Captures (Design-Memory §8).
 *
 * There is no capture list. A name the body uses that belongs to an enclosing function is one more
 * binding of that name, under the ordinary conventions, and which convention is inferred from the
 * enclosing binding:
 *
 *  - it names *mutable* storage (`let &x`, a `&` parameter): a mutable borrow. §8 requires this for
 *    a capture the body writes, and taking it for one that merely reads is the deliberate
 *    over-approximation described at captureBinding().
 *  - it names storage this frame does not own (an immutable borrow, a `&` parameter's target seen
 *    through another closure): an immutable borrow, because there is nothing here to move.
 *  - TrivialCopy: an independent copy, per §2.1.
 *  - anything else: a sink. The closure owns it, and the enclosing binding is inaccessible
 *    afterwards - which is what makes returning a closure over an owned local work at all.
 *
 * The environment.
 *
 * One tuple, holding a word per capture: the value for the two that own it and a `&T` for the two
 * that borrow. It is owned by the *function value* rather than by the frame that built it - the
 * closure's derived Reclaim is what hands it back, see teardownGlueFor's Fun case in analyze.cpp -
 * and it is allocated wherever the ownership pass says the captures have to live: on the frame for a
 * closure that dies in it, and on the heap for one that leaves. Which of the two it was is written
 * into the lambda's closure header, because the Reclaim that has to know is not in this frame.
 */

// The name a lifted lambda is printed and linked under. It is not addressable in source; what it
// needs is to be unique and to say where it came from.
static StringId lambdaName(Module& module) {
    StringBuilder text;
    text << module.context.findName(module.name) << ".lambda$";
    show(module.program.lambdaCounter++, text);

    return module.context.addQualifiedName(text.pointer(), text.size(), 1);
}

// The same, for the nullary thunk a `@lazy` argument becomes. Named apart from an ordinary lambda
// so that a closure in the output can be read back to the thing in source that caused it.
static StringId deferredName(Module& module) {
    StringBuilder text;
    text << module.context.findName(module.name) << ".deferred$";
    show(module.program.lambdaCounter++, text);

    return module.context.addQualifiedName(text.pointer(), text.size(), 1);
}

// The same, for the thunk that lets a named function be a function value.
static StringId thunkName(Module& module, Function& callee) {
    StringBuilder text;
    text << "funvalue$" << module.context.findName(callee.name);
    return module.context.addQualifiedName(text.pointer(), text.size(), 1);
}

// The type one name has, without emitting anything to find out. Deliberately separate from
// placeOf(), which for a by-reference capture emits the load that reaches the storage.
static TypePtr bindingType(ExprResolver& resolver, const Binding& binding) {
    auto global = resolver.global;

    if(binding.captured) {
        auto field = resolver.envType->fields.get(global, binding.captureField);
        if(!binding.captureBorrow) return field.type;

        return isBorrow(global, field.type) ? ((BorrowType*)global[field.type])->to : field.type;
    }

    if(binding.local != maxLimit<U32>) {
        return resolver.function.localAt(resolver.local, binding.local).type;
    }

    if(binding.borrow) {
        auto type = resolver.valueType(binding.borrow);
        return isBorrow(global, type) ? ((BorrowType*)global[type])->to : type;
    }

    return resolver.valueType(binding.value);
}

// Whether writing through this name is allowed - the same question isWritablePlace() asks of a
// place, asked of a binding instead so that a capture's convention can be decided before any
// instruction that would name the storage exists.
static bool writableBinding(ExprResolver& resolver, const Binding& binding) {
    auto global = resolver.global;

    if(binding.captured) {
        auto field = resolver.envType->fields.get(global, binding.captureField);
        return binding.captureBorrow && isBorrow(global, field.type) &&
               ((BorrowType*)global[field.type])->mut;
    }

    if(binding.local != maxLimit<U32>) {
        return resolver.function.localAt(resolver.local, binding.local).convention == ast::BindType::Ref;
    }

    if(binding.borrow) {
        auto type = resolver.valueType(binding.borrow);
        return isBorrow(global, type) && ((BorrowType*)global[type])->mut;
    }

    return false;
}

// Whether the storage this name reaches belongs to some other frame, which is what makes moving out
// of it impossible however the type is classified.
static bool borrowedBinding(ExprResolver& resolver, const Binding& binding) {
    if(binding.captured) return binding.captureBorrow;
    if(binding.borrow) return true;

    if(binding.local != maxLimit<U32>) {
        return resolver.function.localAt(resolver.local, binding.local).borrowed;
    }

    return false;
}

Place ExprResolver::placeOf(const Binding& binding, LocationId source) {
    if(!binding.captured) return binding.place();

    auto field = project(Place::atPointer(envArg), ProjectionKind::Field, binding.captureField);
    if(!binding.captureBorrow) return field;

    // A by-reference capture holds an address, so reaching the storage is one load and then a place
    // rooted in what it produced - the same shape `let &entry = f(...)` already has.
    return Place::inBorrow(load(field, source));
}

/*
 * One more binding of a name that belongs to an enclosing function.
 *
 * The convention is inferred rather than written, and the first rule is deliberately wider than
 * Design-Memory §8 asks for: §8 says a capture the body *writes* must be by reference, and this
 * takes a mutable borrow of every capture whose enclosing binding *could* be written. Deciding it
 * the narrow way means knowing what the whole body does before resolving any of it, and the cost of
 * the wide way is exclusivity a read-only closure over a `let &` did not need - which rejects
 * programs rather than accepting bad ones, and is the same direction every other approximation in
 * the resolver leans.
 */
Binding* ExprResolver::captureBinding(StringId name, LocationId source) {
    if(!enclosing || !envType) return nullptr;

    /*
     * Already captured, once, and named again from a scope that did not exist the first time.
     *
     * Which capture a name is refers to the whole body - the environment gains its field when the
     * body first names it and keeps it - but the *binding* recording that is pushed onto the
     * ordinary scope stack, so an `if` arm or a `match` case that discovers the capture takes it
     * away again when it ends. Without this the name is captured a second time, and the environment
     * has two fields holding the same thing.
     */
    for(Size i = 0; i < captures.size(); i++) {
        if(captures[i].name != name) continue;

        Binding existing;
        existing.name = name;
        existing.captured = true;
        existing.captureBorrow = captures[i].byReference;
        existing.captureField = U16(i);

        bindings.push(existing);
        return &bindings[bindings.size() - 1];
    }

    // Recursive on purpose: a lambda inside a lambda naming a binding two frames out captures it
    // through the one in between, which is this happening twice rather than a second mechanism.
    auto outer = enclosing->findBinding(name, source);
    if(!outer) return nullptr;

    /*
     * A `@lazy` parameter of the enclosing function is not capturable.
     *
     * What the name holds is the thunk, so the capture would be a closure inside a closure - which
     * this version cannot own (the environment would have to release it) and could not check (the
     * force would happen in a lifted body, where the enclosing function's at-most-once rule cannot
     * see it). Passing the parameter *on* is the supported shape and is not a capture at all; see
     * makeThunk, which recognizes it before any body is resolved.
     *
     * The failed binding is recorded rather than left absent so that this is the only diagnostic:
     * returning nothing would have the name reported a second time as an unknown one.
     */
    if(outer->lazy) {
        context.diagnostics.error("%@ is a `@lazy` parameter of the enclosing function and cannot be captured - read it into a `let` first, and capture that"_v,
                                  source, context.findName(name));

        bindings.push(Binding { name, nullptr });
        return &bindings[bindings.size() - 1];
    }

    auto type = bindingType(*enclosing, *outer);

    Capture capture;
    capture.name = name;
    capture.type = type;

    if(writableBinding(*enclosing, *outer)) {
        capture.convention = ast::BindType::Ref;
        capture.byReference = true;
    } else if(borrowedBinding(*enclosing, *outer)) {
        capture.convention = ast::BindType::Borrow;
        capture.byReference = true;
    } else if(ownershipOf(module, type).trivialCopy) {
        capture.convention = ast::BindType::Borrow;
    } else {
        capture.convention = ast::BindType::Sink;
    }

    auto fieldType = capture.byReference
        ? resolveBorrowType(module, type, capture.convention == ast::BindType::Ref)
        : type;

    auto index = U16(captures.size());
    captures.push(capture);
    envType->fields.push(module.types, Field { fieldType, name });

    Binding binding;
    binding.name = name;
    binding.captured = true;
    binding.captureBorrow = capture.byReference;
    binding.captureField = index;

    bindings.push(binding);
    return &bindings[bindings.size() - 1];
}

// Writes one word into each field of the environment, in the enclosing frame, at the point the
// closure is built. This is where a capture's convention becomes an instruction: a borrow, or the
// copy-or-move sinkValue() already picks between by the source's ownership classification.
void fillEnvironment(ExprResolver& resolver, ExprResolver& body, Place place, LocationId source) {
    U16 index = 0;

    for(auto& capture: body.captures) {
        auto field = resolver.project(place, ProjectionKind::Field, index++);
        auto outer = resolver.findBinding(capture.name);
        if(!outer) continue;

        // An immutable binding is a name for an SSA value and nothing more, so there is no place to
        // read and nothing to move out of: what goes into the environment is the value itself.
        // Such a binding is never captured by reference, since it names no storage to refer to.
        if(!outer->isPlace()) {
            resolver.initialize(field, resolver.sinkValue(outer->value, source), source);
            continue;
        }

        auto from = resolver.placeOf(*outer, source);

        if(capture.byReference) {
            auto mutable_ = capture.convention == ast::BindType::Ref;
            auto type = resolveBorrowType(resolver.module, capture.type, mutable_);
            auto borrow = resolver.emit<InstBorrow>(source, 0, type, from, mutable_);
            resolver.initialize(field, resolver.ref(borrow), source);
            continue;
        }

        // The two owning conventions are one call: sinkValue() moves what is not TrivialCopy and
        // duplicates what is, which is exactly §8's split between "consumed" and "read only, and
        // therefore copied".
        auto value = resolver.load(from, source);
        resolver.initialize(field, resolver.sinkValue(value, source), source);
    }
}

ModulePtr<Value> ExprResolver::makeFunValue(TypePtr type, ModulePtr<Function> code, ModulePtr<Value> env,
                                            LocationId source, StringId name) {
    auto codeType = funValueFieldType(module, FunValueLayout::kCode);
    local[code]->used = true;

    auto storage = allocate(type, source, name);
    auto place = placeFor(storage, source);

    auto codeValue = ref(emit<InstSymbol>(source, 0, codeType, code, nullptr));
    initialize(project(place, ProjectionKind::Field, FunValueLayout::kCode), codeValue, source);

    initialize(project(place, ProjectionKind::Field, FunValueLayout::kEnv),
               env ? env : constantBits(codeType, 0, source), source);

    return storage;
}

/*
 * The thunk that makes a named function a function value.
 *
 * Every code word is called with the environment first, so a function that was not written as a
 * lambda needs one word of glue to drop it. Generating that rather than giving function values two
 * calling shapes is the same trade a class witness's entry thunks make: one adapter, at one place,
 * instead of a test at every call site.
 */
static ModulePtr<Function> functionThunk(Module& module, ModulePtr<Function> callee, LocationId source) {
    auto& program = module.program;
    if(auto found = program.functionThunks.get(U32(callee))) return found.unwrap();

    auto target = (*module.arena)[callee];
    auto function = addAnonymousFunction(module, thunkName(module, *target), source);
    auto pointer = function - *module.arena;
    *program.functionThunks.add(U32(callee)).value = pointer;

    function->returnType = target->returnType;
    function->used = true;
    function->takesEnv = true;
    target->used = true;

    // The dropped environment. It has no captures behind it, so its type is a pointer to nothing.
    function->addArg(module, module.context.addUnqualifiedName("env", 3),
                     funValueFieldType(module, FunValueLayout::kEnv), source);

    ValueList args;
    for(auto argPointer: target->args.contents(*module.arena)) {
        auto declared = (*module.arena)[argPointer];
        auto forwarded = function->addArg(module, declared->name, declared->type, source);
        forwarded->convention = declared->convention;
        forwarded->returnRoot = declared->returnRoot;
        args.push((ModulePtr<Value>)(forwarded - *module.arena));
    }

    ExprResolver resolver(module.context, module, *function);
    bindFunctionArgs(resolver, module, *function, 1);

    // Through emitDirectCall rather than by building the call directly, so that each parameter's
    // convention is applied exactly once more: a `&` parameter is re-borrowed, and a `->` one is
    // moved out of the thunk's own slot. Skipping that would leave the thunk owning a value it had
    // already handed on, and its frame would release it a second time.
    auto result = resolver.emitDirectCall(callee, toBuffer(args), source);

    resolver.terminate(resolver.emit<InstRet>(source, 0, module.scalar.unit,
                                              isUnit(*module.types, target->returnType) ? nullptr : result));

    return pointer;
}

ModulePtr<Value> ExprResolver::functionValue(ModulePtr<Function> callee, LocationId source) {
    auto target = local[callee];

    // A lens reached as a value is a lens reached through the erased callback ABI: the two words
    // say nothing about which argument is the continuation, so a call site holding one cannot split
    // its block. That ABI is Implementation-Generics.md part 7's own open question.
    if(target->funKind != ast::FunKind::Plain) {
        context.diagnostics.error("%@ is a lens, so it cannot be used as a function value yet - the block a lens call splits is decided where the call is written"_v,
                                  source, context.findName(target->name));
        return nullptr;
    }

    if(target->gen) {
        context.diagnostics.error("%@ is generic, and a generic function cannot be used as a function value yet - it would need a witness rather than an address"_v,
                                  source, context.findName(target->name));
        return nullptr;
    }

    if(target->intrinsic) {
        context.diagnostics.error("%@ has no address to take - the compiler generates its implementation at each call site"_v,
                                  source, context.findName(target->name));
        return nullptr;
    }

    Array<FunArg> args;
    for(auto argPointer: target->args.contents(local)) {
        auto declared = local[argPointer];

        // A `@lazy` parameter is what the *call site* builds the thunk for, and a function value
        // has no call site to build one at: the two words are all a caller holding one gets, and
        // nothing in them says which argument was meant to stay unevaluated. Taking the address of
        // such a function is what §7's erased callback ABI is for, and it is not here yet.
        if(declared->isLazy()) {
            context.diagnostics.error("%@ has a `@lazy` parameter, so it cannot be used as a function value yet - the thunk is built where the call is written"_v,
                                      source, context.findName(target->name));
            return nullptr;
        }

        args.push(FunArg { declared->type, declared->name, declared->convention, declared->returnRoot });
    }

    auto type = resolveFunType(module, toBuffer(args), target->returnType, ast::FunKind::Plain);
    auto thunk = functionThunk(module, callee, source);
    return makeFunValue(type, thunk, nullptr, source, 0);
}

ModulePtr<Value> ExprResolver::emitDynamicCall(ModulePtr<Value> callable, Buffer<ModulePtr<Value>> args,
                                               LocationId source, StringId resultName) {
    if(!callable) return nullptr;

    auto type = valueType(callable);
    auto signature = (FunType*)global[type];

    if(signature->kind != ast::FunKind::Plain) {
        context.diagnostics.error("lens and iter function values are not available yet"_v, source);
        return nullptr;
    }

    for(auto declared: signature->args.contents(global)) {
        if(!declared.lazy) continue;

        context.diagnostics.error("a function value with a `@lazy` parameter cannot be called yet - the thunk is built where the call is written, and this call site cannot see which parameter asked for one"_v,
                                  source);
        return nullptr;
    }

    if(signature->args.size() != args.length) {
        context.diagnostics.error("%@ takes %@ arguments, but this call passes %@"_v, source,
                                  describeType(context, global, type),
                                  U32(signature->args.size()), U32(args.length));
        return nullptr;
    }

    /*
     * Every argument's convention, read off the *type*.
     *
     * This is what FunArg exists for. A caller reaching a function through a value has the type and
     * nothing else, so a `&` argument here has to become a mutable borrow for the same reason it
     * does at a direct call - the callee writes through it, and which callee that is has not been
     * decided yet.
     */
    auto packed = packedMark();

    ValueList converted;
    for(Size i = 0; i < args.length; i++) {
        auto declared = signature->args.get(global, i);

        if(declared.convention == ast::BindType::Ref) {
            converted.push(borrowArgument(args[i], declared.type, source, declared.returnRoot));
            continue;
        }

        auto value = convert(args[i], declared.type, source);
        if(declared.convention == ast::BindType::Sink) value = sinkValue(value, source);

        // The loan a `return` argument creates has to outlive the call, exactly as it does for a
        // direct one - the marker is part of the type precisely so that this is possible here.
        if(declared.returnRoot && value) {
            if(auto argPlace = findPlace(value)) {
                value = borrowPlace(argPlace.unwrap(), resolveBorrowType(module, declared.type, false),
                                    source, true);
            }
        }

        converted.push(value);
    }

    // The value is the operand rather than the two words unpacked out of it, so that the closure
    // stays live across the call it is the callee of - see InstCallDyn.
    auto call = create<InstCallDyn>(source, resultName, signature->result, callable, nullptr, type);
    for(auto value: converted) {
        if(value) call->args.push(module.arena, value);
    }

    append(call);
    auto result = ref(call);

    if(isMemoryType(global, signature->result)) {
        call->local = function.addLocal(module, signature->result, resultName, result);
    }

    flushPackedBorrows(packed);
    return result;
}

ModulePtr<Value> ExprResolver::resolveFun(const ast::Expr& expr, const ast::FunExpr& fun, TypePtr target) {
    auto source = expr.source;

    if(fun.kind != ast::FunKind::Plain) {
        context.diagnostics.error("lens and iter lambdas are not available yet"_v, source);
        return nullptr;
    }

    /*
     * A lambda in a generic body is rejected rather than approximated.
     *
     * A generic body is resolved once and cloned per instantiation, and the lifted function is a
     * separate declaration that cloning does not reach - so a lambda whose signature or captures
     * mentioned a type variable would be shared by every specialization at whatever types the
     * generic body happened to name. Making it work means cloning the lifted function alongside its
     * caller, which is real work and belongs with constrained function values.
     */
    if(functionGen(global, function)) {
        context.diagnostics.error("a lambda inside a generic function is not available yet - the lifted body would have to be specialized alongside its caller"_v,
                                  source);
        return nullptr;
    }

    // The expected type, where the position supplies one. It is what gives an argument its type
    // when the lambda did not write one, and what decides the result - and a lambda that is not in
    // such a position is resolved bottom-up instead.
    auto expected = target && global[target]->kind == Type::Fun ? (FunType*)global[target] : nullptr;
    auto astArgs = fun.args;

    if(expected && expected->args.size() != astArgs.size()) {
        context.diagnostics.error("this lambda takes %@ arguments, but %@ was expected"_v, source,
                                  U32(astArgs.size()), describeType(context, global, target));

        expected = nullptr;
    }

    if(expected && expected->kind != ast::FunKind::Plain) {
        context.diagnostics.error("lens and iter function values are not available yet"_v, source);
        return nullptr;
    }

    auto lambda = addAnonymousFunction(module, lambdaName(module), source);
    lambda->used = true;
    lambda->takesEnv = true;

    /*
     * The environment's type is built as the body names its captures, which is why it is a fresh
     * tuple rather than an interned one: the fields do not all exist yet when the parameter that
     * points at it is created. Nothing ever compares two of these - an environment type is not
     * something source can name - so interning would buy nothing anyway.
     */
    auto envTuple = new (module.types) TupType;
    envTuple->named = true;

    auto envPointer = resolvePointerType(module, (Type*)envTuple - global);
    auto envArgValue = lambda->addArg(module, context.addUnqualifiedName("env", 3), envPointer, source);

    Array<FunArg> signature;
    U16 index = 0;
    auto allRootsMutable = true;
    auto roots = 0u;

    for(auto arg: astArgs.contents(parse)) {
        TypePtr argType = nullptr;

        if(arg.type) argType = resolveType(module, *parse[arg.type], nullptr);
        else if(expected) argType = expected->args.get(global, index).type;

        if(!argType) {
            context.diagnostics.error("cannot tell what type the lambda argument %@ has - give it one, or write the lambda where a function type is expected"_v,
                                      arg.source, context.findName(arg.name));

            argType = module.scalar.error;
        }

        if(arg.def) {
            context.diagnostics.error("a lambda argument cannot have a default value"_v, arg.source);
        }

        if(arg.lazy) {
            context.diagnostics.error("a lambda cannot declare a `@lazy` parameter yet - only a call to a named function or a class function builds the thunk"_v,
                                      arg.source);
        }

        auto declared = lambda->addArg(module, arg.name, argType, arg.source);
        declared->convention = arg.bind;
        declared->returnRoot = arg.returnRoot;

        if(arg.returnRoot) {
            if(checkReturnRoot(module, argType, arg.bind, index, arg.source)) {
                roots++;
                if(arg.bind != ast::BindType::Ref) allRootsMutable = false;
            } else {
                declared->returnRoot = false;
            }
        }

        signature.push(FunArg { argType, arg.name, arg.bind, declared->returnRoot });
        index++;
    }

    lambda->returnType = expected ? expected->result : nullptr;

    ExprResolver body(context, module, *lambda);
    body.enclosing = this;
    body.envArg = (ModulePtr<Value>)(envArgValue - local);
    body.envType = envTuple;
    body.resultInferred = expected == nullptr;

    bindFunctionArgs(body, module, *lambda, 1);

    auto wantsValue = !lambda->returnType || !isUnit(global, lambda->returnType);
    auto result = body.resolve(fun.body, lambda->returnType, wantsValue);

    if(body.current) {
        if(!lambda->returnType) {
            // Bottom-up: the body decides. Settling first is what keeps a lambda whose body is a
            // bare literal from having a result type no caller could name.
            result = body.settle(result, source);
            lambda->returnType = result ? body.valueType(result) : module.scalar.unit;
        } else if(isUnit(global, lambda->returnType)) {
            result = nullptr;
        } else {
            result = body.convert(result, lambda->returnType, source);
        }

        body.terminate(body.emit<InstRet>(source, 0, module.scalar.unit, result));
    } else if(!lambda->returnType) {
        // Every path left through an explicit `return`, which resultInferred has already reported.
        lambda->returnType = module.scalar.unit;
    }

    if(isBorrow(global, lambda->returnType) && roots) {
        lambda->returnType = applyReturnRootMutability(module, lambda->returnType, allRootsMutable);
    }

    auto type = resolveFunType(module, toBuffer(signature), lambda->returnType, ast::FunKind::Plain);

    // The environment is complete once the body is. Nothing is decided here any more - a code
    // generator lays the tuple out when it emits - but a capture that made the environment contain
    // itself would still be an infinitely large value, and that is a source error worth reporting
    // against the lambda rather than against whichever backend noticed.
    auto envType = (Type*)envTuple - global;
    checkTypeAcyclic(module, envType, source);

    // A lambda that captured nothing gets neither storage nor a header: the value's second word is
    // null, and its teardown is a branch that never fires.
    if(body.captures.isEmpty()) return makeFunValue(type, lambda - local, nullptr, source, 0);

    // The header goes in front of the lifted function rather than into the value, which is what
    // keeps a function value two words wide. Its flags are completed by selectStorage, the only
    // pass that knows where the environment below actually landed.
    auto lambdaPointer = (ModulePtr<Function>)(lambda - local);
    closureHeaderFor(module, lambdaPointer, envType, source);

    auto storage = allocate(envType, source, 0, ast::BindType::Borrow, true);
    ((InstAlloc*)local[storage])->closure = lambdaPointer;

    auto place = placeFor(storage, source);
    fillEnvironment(*this, body, place, source);

    // Typed as a bare address rather than as `%Env`, because that is what the function value's
    // second word is: whoever reads it does so through the descriptor the code word leads to, which
    // is the only thing that knows what is in there.
    auto address = ref(emit<InstAddress>(source, 0, funValueFieldType(module, FunValueLayout::kEnv), place));

    return makeFunValue(type, lambda - local, address, source, 0);
}

/*
 * Deferred arguments - Design.md's Deferred arguments, and Analysis-Lens.md §4.
 *
 * A `@lazy` argument shares this file's machinery and nothing else with a lens. It is a *downward*
 * thunk: the callee calls it and returns from it, resuming where it was. It consumes no part of the
 * caller's block, needs no continuation splitter, and is therefore an ordinary nullary closure over
 * the caller's frame - which is why it lands here rather than waiting on any of the lens work.
 *
 * There is no memoization slot, and the reason is the at-most-once rule rather than an optimization:
 * forcing twice is rejected (see checkLazyForcing in expr.cpp), so nothing can observe the
 * difference between call-by-name and call-by-need and no cell has to exist to make that true.
 */

Deferred ExprResolver::deferredValue(ModulePtr<Value> value, TypePtr type) {
    Deferred deferred;

    if(value && type && valueType(value) == resolveThunkType(module, type)) deferred.thunk = value;
    else deferred.value = value;

    return deferred;
}

ModulePtr<Value> ExprResolver::force(const Deferred& deferred, TypePtr expected, LocationId source) {
    // The two forms that cost nothing: the argument is still an expression, so it is emitted here,
    // in the block the callee decided to run it in.
    if(deferred.expr) return resolve(*deferred.expr, expected);

    if(deferred.chain) {
        auto& chain = *deferred.chain;
        auto operandIndex = chain.operandIndex;
        auto operatorIndex = chain.operatorIndex;

        return resolvePrecedence(*chain.operands, *chain.operators, operandIndex, operatorIndex,
                                 chain.minimumPrecedence, expected);
    }

    // The closure a callee that could not see the argument was handed. Calling it is the force, and
    // this is the one place a `@lazy` parameter costs an indirect call.
    if(deferred.thunk) {
        auto result = emitDynamicCall(deferred.thunk, {}, source, 0);
        return expected ? convert(result, expected, source) : result;
    }

    return expected ? convert(deferred.value, expected, source) : deferred.value;
}

/*
 * The closure an opaque callee is handed.
 *
 * Two things it may not do yet, and both are stated as restrictions rather than discovered as
 * miscompiles:
 *
 *  - it may not *consume* what it captures. An unforced thunk still holds whatever moved into it,
 *    so the owner has to be dropped at the end of the call by a thunk-local teardown that runs only
 *    if the thunk did not - which is Analysis-Lens.md §4's second option and is real work in the
 *    drop pass. Until it exists, a capture that would be a sink is reported;
 *  - it may not appear in a generic body, for the reason a lambda may not: the lifted function is a
 *    separate declaration that cloning does not reach, so every specialization would share one body
 *    built at whatever types the generic happened to name.
 *
 * Neither restricts the case this version exists for. `&&`, `||` and `??` on a concrete type reach
 * an intrinsic that sees through the call, so no thunk is built at all.
 */
ModulePtr<Value> ExprResolver::makeThunk(const Deferred& deferred, TypePtr type, LocationId source) {
    if(!deferred.isSet() || !type) return nullptr;

    auto wanted = resolveThunkType(module, type);

    // Already a thunk: a `@lazy` parameter passed straight on to another `@lazy` parameter, which
    // is one closure rather than one wrapped in another.
    if(deferred.thunk && valueType(deferred.thunk) == wanted) return deferred.thunk;

    /*
     * The same, before the argument has been resolved: `f(@lazy v)` where `v` is this function's own
     * `@lazy` parameter.
     *
     * Handing on the thunk that already exists is not only the cheaper answer, it is the only
     * available one - a second thunk would have to *capture* the first, and a closure is not
     * something a thunk may capture (see below). It is also the correct one: the argument's one
     * permitted evaluation moves to the callee along with it, which is what checkLazyForcing counts
     * when it treats passing a `@lazy` parameter on as using it.
     */
    if(deferred.expr) {
        auto& named = unwrapNested(*deferred.expr);

        if(named.kind == ast::Expr::Var) {
            auto binding = findBinding(named.var);
            if(binding && binding->lazy && valueType(binding->value) == wanted) return binding->value;
        }
    }

    if(functionGen(global, function)) {
        context.diagnostics.error("a `@lazy` argument inside a generic function is not available yet unless the callee can be seen through - the thunk would have to be specialized alongside its caller"_v,
                                  source);
        return nullptr;
    }

    auto lambda = addAnonymousFunction(module, deferredName(module), source);
    lambda->used = true;
    lambda->takesEnv = true;
    lambda->returnType = type;

    auto envTuple = new (module.types) TupType;
    envTuple->named = true;

    auto envPointer = resolvePointerType(module, (Type*)envTuple - global);
    auto envArgValue = lambda->addArg(module, context.addUnqualifiedName("env", 3), envPointer, source);

    ExprResolver body(context, module, *lambda);
    body.enclosing = this;
    body.envArg = (ModulePtr<Value>)(envArgValue - local);
    body.envType = envTuple;
    body.inThunk = true;

    auto result = body.force(deferred, type, source);

    if(body.current) {
        result = isUnit(global, type) ? nullptr : body.convert(result, type, source);
        body.terminate(body.emit<InstRet>(source, 0, module.scalar.unit, result));
    }

    for(auto& capture: body.captures) {
        if(capture.convention != ast::BindType::Sink) continue;

        context.diagnostics.error("a `@lazy` argument cannot consume %@ yet - the callee may never run it, and the thunk would then still be holding what it moved in"_v,
                                  source, context.findName(capture.name));
        return nullptr;
    }

    auto funType = resolveThunkType(module, type);
    if(body.captures.isEmpty()) return makeFunValue(funType, lambda - local, nullptr, source, 0);

    auto envType = (Type*)envTuple - global;
    checkTypeAcyclic(module, envType, source);

    auto lambdaPointer = (ModulePtr<Function>)(lambda - local);
    closureHeaderFor(module, lambdaPointer, envType, source);

    auto storage = allocate(envType, source, 0, ast::BindType::Borrow, true);
    ((InstAlloc*)local[storage])->closure = lambdaPointer;

    auto place = placeFor(storage, source);
    fillEnvironment(*this, body, place, source);

    auto address = ref(emit<InstAddress>(source, 0, funValueFieldType(module, FunValueLayout::kEnv), place));
    return makeFunValue(funType, lambda - local, address, source, 0);
}
