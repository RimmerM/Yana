#include "intrinsic.h"
#include "builder.h"
#include "name.h"

/*
 * The emitters.
 */

ModulePtr<Value> emitCast(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                          LocationId source, StringId resultName) {
    return resolver.ref(resolver.emit<InstUnary>(source, resultName, type, Value::Cast, args[0]));
}

// Folding the constant here rather than emitting a cast is what lets every literal go through a
// class without concrete arithmetic generating anything it did not generate before: `1 :: Double`
// is still one immediate, not a Long immediate and a conversion.
//
// A `fromInt(x)` written out with a runtime argument is an ordinary numeric conversion, and is
// also what the generated body of the instance itself contains.
ModulePtr<Value> emitFromLiteral(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                 LocationId source, StringId resultName) {
    auto value = resolver.local[args[0]];

    if(value->kind == Value::ConstInt) {
        auto literal = ((ConstInt*)value)->value;
        return isFloat(resolver.global, type) ? resolver.makeFloat(source, type, F64(literal))
                                              : resolver.makeInt(source, type, literal);
    }

    if(value->kind == Value::ConstDouble) return resolver.makeFloat(source, type, ((ConstDouble*)value)->value);
    if(value->kind == Value::ConstFloat) return resolver.makeFloat(source, type, F64(((ConstFloat*)value)->value));

    return emitCast(resolver, args, type, source, resultName);
}

// The identity is a real instance rather than a special case in the resolver, so that the
// condition path has exactly one shape - and because it expands to nothing, `if a > b` produces
// the IR it always did.
ModulePtr<Value> emitIdentity(ExprResolver&, Buffer<ModulePtr<Value>> args, TypePtr, LocationId, StringId) {
    return args[0];
}

// The zero is built at the operand's type rather than the result's, which is what makes one
// emitter serve both the integer and the floating-point instances.
ModulePtr<Value> emitTruthy(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                            LocationId source, StringId resultName) {
    auto from = resolver.valueType(args[0]);
    auto zero = isFloat(resolver.global, from) ? resolver.makeFloat(source, from, 0.0)
                                               : resolver.makeInt(source, from, 0);

    return resolver.ref(resolver.emit<InstCmp>(source, resultName, type, args[0], zero, CompareOp::Ne));
}

ModulePtr<Value> emitLogicalNot(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                LocationId source, StringId resultName) {
    auto one = resolver.makeInt(source, type, 1);
    return resolver.ref(resolver.emit<InstBinary>(source, resultName, type, Value::Xor, args[0], one));
}

/*
 * The short-circuit, which is one shape read two ways.
 *
 * `&&` runs its right operand when the left holds and `||` when it does not, and that is the whole
 * of the difference: both produce the *left* operand on the path that skipped, because `False && x`
 * is that False and `True || x` is that True. So nothing is built for the skipped side - no
 * constant, no second value, and no reason for the two emitters to be more than one.
 *
 * The right operand is emitted inside the branch rather than passed in, which is what `@lazy` buys
 * here: `p != nil && p.load() > 0` dereferences only where the test held, and no closure, no call
 * and no allocation appear anywhere in the result.
 */
static ModulePtr<Value> emitShortCircuit(ExprResolver& resolver, Buffer<ModulePtr<Value>> args,
                                         Buffer<Deferred> deferred, TypePtr type, LocationId source,
                                         bool runWhenTrue) {
    auto lhs = args[0];
    if(!lhs || deferred.length < 2) return nullptr;

    auto unit = resolver.module.scalar.unit;
    auto rest = resolver.addBlock();
    auto skipped = resolver.addBlock();

    resolver.terminate(resolver.emit<InstJe>(source, 0, unit, lhs,
                                             runWhenTrue ? rest : skipped,
                                             runWhenTrue ? skipped : rest));

    Array<BranchArm> arms;

    resolver.current = rest;
    auto value = resolver.force(deferred[1], type, source);

    // The right operand may itself have branched, and may not complete at all - it is the caller's
    // code, spliced in here, with whatever control flow the caller put in it.
    if(resolver.current) arms.push(BranchArm { resolver.current, value, source });

    arms.push(BranchArm { skipped, lhs, source });

    return resolver.finishBranches(arms, source, true);
}

ModulePtr<Value> emitLogicalAnd(ExprResolver& resolver, Buffer<ModulePtr<Value>> args,
                                Buffer<Deferred> deferred, TypePtr type, LocationId source, StringId) {
    return emitShortCircuit(resolver, args, deferred, type, source, true);
}

ModulePtr<Value> emitLogicalOr(ExprResolver& resolver, Buffer<ModulePtr<Value>> args,
                               Buffer<Deferred> deferred, TypePtr type, LocationId source, StringId) {
    return emitShortCircuit(resolver, args, deferred, type, source, false);
}

/*
 * Generating an instance function.
 */

GlobalPtr<TypeClass> classNamed(Module& module, StringView text) {
    auto found = findClass(module, Context::nameHash(text), kNullLocation);
    assertTrue(found != nullptr);
    return found;
}

// Emits `fn f(args...) = <emit>(args...)` as a real function, so the instance has something to
// print, lower and eventually take the address of even though ordinary calls inline it.
static ModulePtr<Function> generateInstanceFunction(Module& module, TypeClass& typeClass, Buffer<TypePtr> args,
                                                    U16 index, const IntrinsicMethod& method,
                                                    GlobalPtr<GenEnv> gen) {
    ModuleBase local = *module.arena;
    GlobalBase global = *module.types;

    auto signature = local[typeClass.functions.get(global, index).fun];
    auto name = typeClass.functions.get(global, index).name;

    auto function = addAnonymousFunction(module, instanceFunctionName(module, typeClass, args, name), kNullLocation);
    function->instanceOf = (TypeClass*)&typeClass - global;
    function->gen = gen;
    for(auto arg: args) function->instanceArgs.push(module.arena, arg);

    function->returnType = substituteType(module, signature->returnType, args, kNullLocation);

    for(Size i = 0; i < signature->args.size(); i++) {
        auto declared = local[signature->args.get(local, i)];
        auto created = function->addArg(module, declared->name,
                                        substituteType(module, declared->type, args, kNullLocation), kNullLocation);
        created->convention = declared->convention;
        created->returnRoot = declared->returnRoot;

        if(declared->isLazy()) {
            created->lazyType = substituteType(module, declared->lazyType, args, kNullLocation);
        }
    }

    ExprResolver resolver(module.context, module, *function);

    Scratch<ValueList> held(module.program.valueLists);
    auto& values = *held;
    for(auto arg: function->args.contents(local)) values.push((ModulePtr<Value>)arg);

    ModulePtr<Value> result = nullptr;

    if(method.deferred) {
        // The body a call that cannot see through this function reaches. Its `@lazy` parameter is
        // the thunk the caller built, so it forces by calling it - the one form of the expansion
        // that costs an indirect call, and the reason a real body exists at all.
        SmallArray<Deferred, 4> pending;

        for(Size i = 0; i < values.size(); i++) {
            auto declared = local[function->args.get(local, i)];
            Deferred entry;

            if(declared->isLazy()) {
                entry.thunk = values[i];
                entry.type = declared->lazyType;
                values[i] = nullptr;
            }

            pending.push(entry);
        }

        result = method.deferred(resolver, toBuffer(values), toBuffer(pending), function->returnType,
                                 kNullLocation, 0);
        function->deferredIntrinsic = method.deferred;
    } else {
        result = method.emit(resolver, toBuffer(values), function->returnType, kNullLocation, 0);
        function->intrinsic = method.emit;
    }

    resolver.terminate(resolver.emit<InstRet>(kNullLocation, 0, module.scalar.unit, result));
    return function - local;
}

// `compare` is the one primitive operation that is not a single instruction, so it has a real
// body and no intrinsic: calls to it are ordinary calls that reach the backend as written.
static ModulePtr<Function> generateCompare(Module& module, TypeClass& typeClass, TypePtr type,
                                          GlobalPtr<GenEnv> gen) {
    ModuleBase local = *module.arena;
    auto ordering = module.scalar.ordering;
    TypePtr args[] = { type };

    auto function = addAnonymousFunction(
        module, instanceFunctionName(module, typeClass, { args, 1 }, Context::nameHash("compare", 7)), kNullLocation);

    function->instanceOf = (TypeClass*)&typeClass - *module.types;
    function->gen = gen;
    function->instanceArgs.push(module.arena, type);
    function->returnType = ordering;

    auto name = [&](StringView text) { return module.context.addQualifiedName(text.ptr, text.length, 1); };
    auto lhs = ModulePtr<Value>(function->addArg(module, name("lhs"_v), type, kNullLocation) - local);
    auto rhs = ModulePtr<Value>(function->addArg(module, name("rhs"_v), type, kNullLocation) - local);

    ExprResolver resolver(module.context, module, *function);
    auto equalBlock = resolver.addBlock();
    auto greaterTest = resolver.addBlock();
    auto greaterBlock = resolver.addBlock();
    auto lessBlock = resolver.addBlock();

    auto equal = resolver.ref(resolver.emit<InstCmp>(kNullLocation, 0, module.scalar.bool_, lhs, rhs, CompareOp::Eq));
    resolver.terminate(resolver.emit<InstJe>(kNullLocation, 0, module.scalar.unit, equal, equalBlock, greaterTest));

    // Ordering has no payload, so each result is just its constructor index.
    auto returnOrdering = [&](ModulePtr<Block> block, U64 constructor) {
        resolver.current = block;
        auto value = resolver.makeInt(kNullLocation, ordering, constructor);
        resolver.terminate(resolver.emit<InstRet>(kNullLocation, 0, module.scalar.unit, value));
    };

    returnOrdering(equalBlock, 1);

    resolver.current = greaterTest;
    auto greater = resolver.ref(resolver.emit<InstCmp>(kNullLocation, 0, module.scalar.bool_, lhs, rhs, CompareOp::Gt));
    resolver.terminate(resolver.emit<InstJe>(kNullLocation, 0, module.scalar.unit, greater, greaterBlock, lessBlock));

    returnOrdering(greaterBlock, 2);
    returnOrdering(lessBlock, 0);

    return function - local;
}

void generateInstance(Module& module, GlobalPtr<TypeClass> classPointer, Buffer<TypePtr> args,
                      Buffer<IntrinsicMethod> methods, GlobalPtr<GenEnv> gen) {
    ModuleBase local = *module.arena;
    GlobalBase global = *module.types;
    auto typeClass = global[classPointer];

    auto instance = new (module.arena) ClassInstance(classPointer);
    instance->module = &module;
    instance->gen = gen;
    for(auto arg: args) instance->forTypes.push(module.arena, arg);
    for(Size i = 0; i < typeClass->functions.size(); i++) instance->functions.push(module.arena, nullptr);

    for(auto& method: methods) {
        auto wanted = Context::nameHash(method.name);
        auto matched = false;

        for(Size i = 0; i < typeClass->functions.size(); i++) {
            auto entry = typeClass->functions.get(global, i);
            if(entry.name != wanted || entry.arity != method.arity) continue;
            if(instance->functions.get(local, i)) continue;

            instance->functions.set(local, i,
                                    generateInstanceFunction(module, *typeClass, args, U16(i), method, gen));
            matched = true;
            break;
        }

        assertTrue(matched);
    }

    // `compare` is the only class function no table entry above covers. The tables stay complete
    // even where the class now has a default, so a primitive's `!=` is still the one `cmp` it
    // always was rather than a specialized `!(lhs == rhs)` waiting to be inlined.
    for(Size i = 0; i < typeClass->functions.size(); i++) {
        if(instance->functions.get(local, i)) continue;

        auto entry = typeClass->functions.get(global, i);
        if(entry.defaultFun) {
            instance->functions.set(local, i, entry.defaultFun);
            continue;
        }

        assertTrue(entry.name == Context::nameHash("compare", 7));
        instance->functions.set(local, i, generateCompare(module, *typeClass, args[0], gen));
    }

    module.instances.push(instance - local);
}

/*
 * The primitive instances.
 */

void defineFromInt(Module& module, TypePtr type) {
    IntrinsicMethod methods[] = { { "fromInt"_v, 1, emitFromLiteral } };
    generateInstance(module, classNamed(module, "FromInt"_v), { &type, 1 }, { methods, 1 });
}

void defineFromDecimal(Module& module, TypePtr type) {
    IntrinsicMethod methods[] = { { "fromDecimal"_v, 1, emitFromLiteral } };
    generateInstance(module, classNamed(module, "FromDecimal"_v), { &type, 1 }, { methods, 1 });
}

void defineEq(Module& module, TypePtr type, GlobalPtr<GenEnv> gen) {
    IntrinsicMethod methods[] = {
        { "=="_v, 2, emitCompare<CompareOp::Eq> },
        { "!="_v, 2, emitCompare<CompareOp::Ne> },
    };

    generateInstance(module, classNamed(module, "Eq"_v), { &type, 1 }, { methods, 2 }, gen);
}

void defineOrd(Module& module, TypePtr type, GlobalPtr<GenEnv> gen) {
    IntrinsicMethod methods[] = {
        { "<"_v, 2, emitCompare<CompareOp::Lt> },
        { "<="_v, 2, emitCompare<CompareOp::Le> },
        { ">"_v, 2, emitCompare<CompareOp::Gt> },
        { ">="_v, 2, emitCompare<CompareOp::Ge> },
    };

    generateInstance(module, classNamed(module, "Ord"_v), { &type, 1 }, { methods, 4 }, gen);
}

void defineNum(Module& module, TypePtr type) {
    IntrinsicMethod methods[] = {
        { "+"_v, 2, emitBinary<Value::Add> },
        { "-"_v, 2, emitBinary<Value::Sub> },
        { "*"_v, 2, emitBinary<Value::Mul> },
        { "/"_v, 2, emitBinary<Value::Div> },
        { "-"_v, 1, emitUnary<Value::Neg> },
    };

    generateInstance(module, classNamed(module, "Num"_v), { &type, 1 }, { methods, 5 });
}

void defineIntegral(Module& module, TypePtr type) {
    IntrinsicMethod methods[] = {
        { "rem"_v, 2, emitBinary<Value::Rem> },
        { "%"_v, 2, emitBinary<Value::Rem> },
        { "shl"_v, 2, emitBinary<Value::Shl> },
        { "shr"_v, 2, emitBinary<Value::Shr> },
        { "sar"_v, 2, emitBinary<Value::Sar> },
        { "and"_v, 2, emitBinary<Value::And> },
        { "or"_v, 2, emitBinary<Value::Or> },
        { "xor"_v, 2, emitBinary<Value::Xor> },
        { "not"_v, 1, emitUnary<Value::Not> },
    };

    generateInstance(module, classNamed(module, "Integral"_v), { &type, 1 }, { methods, 9 });
}

void defineLogic(Module& module, TypePtr type) {
    IntrinsicMethod methods[] = {
        { "&&"_v, 2, nullptr, emitLogicalAnd },
        { "||"_v, 2, nullptr, emitLogicalOr },
        { "and"_v, 2, emitBinary<Value::And> },
        { "or"_v, 2, emitBinary<Value::Or> },
        { "xor"_v, 2, emitBinary<Value::Xor> },
        { "not"_v, 1, emitLogicalNot },
        { "!"_v, 1, emitLogicalNot },
    };

    generateInstance(module, classNamed(module, "Logic"_v), { &type, 1 }, { methods, 7 });
}

void defineTruth(Module& module, TypePtr type, Emit emit) {
    IntrinsicMethod methods[] = { { "truthy"_v, 1, emit } };
    generateInstance(module, classNamed(module, "Truth"_v), { &type, 1 }, { methods, 1 });
}

void defineConversion(Module& module, StringView className, StringView method, TypePtr from, TypePtr to) {
    TypePtr args[] = { from, to };
    IntrinsicMethod methods[] = { { method, 1, emitCast } };

    generateInstance(module, classNamed(module, className), { args, 2 }, { methods, 1 });
}

void attachIntrinsic(Module& module, StringView name, Intrinsic intrinsic) {
    auto found = module.functions.get(Context::nameHash(name));

    if(!found) {
        module.context.diagnostics.error("internal: no declaration of the intrinsic %@"_v, kNullLocation, name);
        return;
    }

    (*module.arena)[found.unwrap()]->intrinsic = intrinsic;
}
