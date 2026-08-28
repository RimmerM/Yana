#include "intrinsic.h"
#include "builder.h"
#include "host.h"
#include "name.h"

/*
 * The emitters.
 */

ModulePtr<Value> emitCast(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                          LocationId source, StringId resultName) {
    return resolver.ref(resolver.emit<InstUnary>(source, resultName, type, Value::Cast, args[0]));
}

// `Bitcast`'s single method, and the reason `Value::Bitcast` exists as a kind: between two types of
// the same width a `Cast` already moves no bits, but between `Float` and `I32` it is a numeric
// conversion, and nothing but the kind distinguishes the two questions.
ModulePtr<Value> emitBitcast(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                             LocationId source, StringId resultName) {
    return resolver.ref(resolver.emit<InstUnary>(source, resultName, type, Value::Bitcast, args[0]));
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

/*
 * The same for a vector, which is the literal in every lane.
 *
 * Not a ruling this work invented and not one it could avoid: `class (FromInt(a)) Num(a)`, so a
 * vector that has `Num` has to say what an integer literal means as one, and "every lane" is the
 * only answer that is not arbitrary. What it buys is that `v * 2` and `v + 1` are written the way
 * they are for a scalar; what it costs is that `1 :: Vec(Int)` is a legal spelling of a splat.
 *
 * The literal is built at the *lane* type and splatted, rather than being built at the vector type -
 * a `ConstInt` typed as a vector is a value neither backend can hold, which is the same shape as the
 * reflexive-comparison fold `foldCompare` had to be guarded against.
 */
ModulePtr<Value> emitVectorFromLiteral(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                       LocationId source, StringId resultName) {
    auto lane = vectorLane(resolver.global, type);
    if(!lane) return emitFromLiteral(resolver, args, type, source, resultName);

    auto scalar = emitFromLiteral(resolver, args, lane, source, StringId());
    if(!scalar) return nullptr;

    return resolver.ref(resolver.emit<InstVecSplat>(source, resultName, type, scalar));
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
static ModulePtr<Value> emitShortCircuit(ExprResolver& resolver, Buffer<ResolvedArg> args, TypePtr type,
                                         LocationId source, bool runWhenTrue) {
    if(args.length < 2 || !args[1].isDeferred()) return nullptr;

    auto lhs = args[0].value;
    if(!lhs) return nullptr;

    auto unit = resolver.module.scalar.unit;
    auto rest = resolver.addBlock();
    auto skipped = resolver.addBlock();

    resolver.terminate(resolver.emit<InstJe>(source, StringId(), unit, lhs,
                                             runWhenTrue ? rest : skipped,
                                             runWhenTrue ? skipped : rest));

    BranchArmList arms;

    resolver.current = rest;
    auto value = resolver.force(args[1].promise, type, source);

    // The right operand may itself have branched, and may not complete at all - it is the caller's
    // code, spliced in here, with whatever control flow the caller put in it.
    if(resolver.current) arms.push(BranchArm { resolver.current, value, source });

    arms.push(BranchArm { skipped, lhs, source });

    return resolver.finishBranches(arms, source, true);
}

ModulePtr<Value> emitLogicalAnd(ExprResolver& resolver, Buffer<ResolvedArg> args, TypePtr type,
                                LocationId source, StringId) {
    return emitShortCircuit(resolver, args, type, source, true);
}

ModulePtr<Value> emitLogicalOr(ExprResolver& resolver, Buffer<ResolvedArg> args, TypePtr type,
                               LocationId source, StringId) {
    return emitShortCircuit(resolver, args, type, source, false);
}

/*
 * `!`, `&&` and `||`, which are `Truth` and not `Bitwise` - see the note on their declarations in
 * Core, and Analysis-Ergonomics on why the arrangement they replace was wrong rather than merely
 * inconsistent.
 *
 * Each of these is a *plain generic function* carrying an intrinsic, which is the shape
 * `scanLimitFor` already uses: the declaration in Core has no body, the hook here is the whole
 * definition, and it sees the concrete argument type at every call site. That is what keeps the
 * language's most common operators expanding to a branch and an `xor` rather than becoming generic
 * calls waiting for an inliner - the property `Emit` exists for.
 *
 * `resolver.truthy` is what turns an operand into a `Bool`, and it is the same selection `if x`
 * makes. So there is exactly one answer to "is this value true" in the language, reached the same
 * way by the condition and by the operator, which is the whole of what was broken before.
 */
ModulePtr<Value> emitTruthNot(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                              LocationId source, StringId resultName) {
    auto value = resolver.truthy(args[0], source);
    if(!value) return nullptr;

    // At a `Bool` the truth test is the identity, so what is left is one `xor` - the same single
    // instruction `!` has always been.
    auto one = resolver.makeInt(source, type, 1);
    return resolver.ref(resolver.emit<InstBinary>(source, resultName, type, Value::Xor, value, one));
}

/*
 * The short circuit, answering `Bool` rather than the operand type.
 *
 * `emitShortCircuit` above answers the *left operand* on the path that skipped, because `False && x`
 * is that `False`. That is right when both sides and the result are one type, and wrong here: these
 * answer `Bool` while the left operand may be an `OpenFlags`. So the skipped path is the constant the
 * operator settles on - `False` for `&&`, `True` for `||` - and the taken path is the right operand's
 * own truth.
 */
static ModulePtr<Value> emitTruthCircuit(ExprResolver& resolver, Buffer<ResolvedArg> args, TypePtr type,
                                         LocationId source, bool runWhenTrue) {
    if(args.length < 2 || !args[1].isDeferred()) return nullptr;

    auto lhs = resolver.truthy(args[0].value, source);
    if(!lhs) return nullptr;

    auto unit = resolver.module.scalar.unit;
    auto rest = resolver.addBlock();
    auto skipped = resolver.addBlock();

    resolver.terminate(resolver.emit<InstJe>(source, StringId(), unit, lhs,
                                             runWhenTrue ? rest : skipped,
                                             runWhenTrue ? skipped : rest));

    BranchArmList arms;

    resolver.current = rest;

    /*
     * The right operand, resolved with no expected type.
     *
     * `&&` converts neither side: what it does with an operand is ask its own `Truth`, so there is
     * nothing for the parameter's declared type to convert to and pushing it down would be a
     * conversion the operator does not perform. That is also what lets the two sides have unrelated
     * types - see deferredOnlyVariable in expr_call.cpp, where the variable this position was
     * declared at is left unbound precisely because nothing reads it.
     *
     * Settled before the truth test, because an operand with no pushdown may still be a literal -
     * `flags && 1` - and a literal variable has no instance of anything until it is one type.
     */
    auto forced = resolver.settle(resolver.force(args[1].promise, nullptr, source), source);
    auto value = forced ? resolver.truthy(forced, source) : nullptr;

    // The right operand is the caller's code spliced in here, with whatever control flow the caller
    // put in it - so it may have branched, and may not complete at all.
    if(resolver.current && value) arms.push(BranchArm { resolver.current, value, source });

    arms.push(BranchArm { skipped, resolver.makeInt(source, type, runWhenTrue ? 0 : 1), source });

    return resolver.finishBranches(arms, source, true);
}

ModulePtr<Value> emitTruthAnd(ExprResolver& resolver, Buffer<ResolvedArg> args, TypePtr type,
                              LocationId source, StringId) {
    return emitTruthCircuit(resolver, args, type, source, true);
}

ModulePtr<Value> emitTruthOr(ExprResolver& resolver, Buffer<ResolvedArg> args, TypePtr type,
                             LocationId source, StringId) {
    return emitTruthCircuit(resolver, args, type, source, false);
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
        created->loan = declared->loan;

        if(declared->isLazy()) {
            created->lazyType = substituteType(module, declared->lazyType, args, kNullLocation);
        }

        /*
         * A parameter that arrives as storage gets the slot naming it, exactly as an authored body's
         * would - see bindFunctionArgs, whose two branches these are.
         *
         * No primitive needed one until `Index`: an operation over two `Int`s has its operands in
         * registers, and the generated body never asked where they lived. An aggregate receiver has
         * to be *addressed* - `self.items` is a projection off the parameter's place - and without a
         * slot findPlace answers "this value has no place", which makes the emitter allocate storage
         * of its own and store a value the frame only borrows into it.
         */
        if(created->isMutableBorrow()) {
            function->addLocal(module, created->type, created->name, (ModulePtr<Value>)(created - local),
                               ast::BindType::Ref, true);
        } else if(isMemoryType(global, created->type)) {
            function->addLocal(module, created->type, created->name, (ModulePtr<Value>)(created - local),
                               created->convention);
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
        ArgList pending;

        for(Size i = 0; i < values.size(); i++) {
            auto declared = local[function->args.get(local, i)];

            if(!declared->isLazy()) {
                pending.push(values[i]);
                continue;
            }

            Deferred entry;
            entry.thunk = values[i];
            entry.type = declared->lazyType;
            pending.push(ResolvedArg::deferred(entry));
        }

        result = method.deferred(resolver, toBuffer(pending), function->returnType, kNullLocation, StringId());
        function->deferredIntrinsic = method.deferred;
    } else {
        result = method.emit(resolver, toBuffer(values), function->returnType, kNullLocation, StringId());
        if(method.expand) function->intrinsic = method.emit;
    }

    resolver.terminate(resolver.emit<InstRet>(kNullLocation, StringId(), module.scalar.unit, result));
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

    auto equal = resolver.ref(resolver.emit<InstCmp>(kNullLocation, StringId(), module.scalar.bool_, lhs, rhs, CompareOp::Eq));
    resolver.terminate(resolver.emit<InstJe>(kNullLocation, StringId(), module.scalar.unit, equal, equalBlock, greaterTest));

    // Ordering has no payload, so each result is just its constructor index.
    auto returnOrdering = [&](ModulePtr<Block> block, U64 constructor) {
        resolver.current = block;
        auto value = resolver.makeInt(kNullLocation, ordering, constructor);
        resolver.terminate(resolver.emit<InstRet>(kNullLocation, StringId(), module.scalar.unit, value));
    };

    returnOrdering(equalBlock, 1);

    resolver.current = greaterTest;
    auto greater = resolver.ref(resolver.emit<InstCmp>(kNullLocation, StringId(), module.scalar.bool_, lhs, rhs, CompareOp::Gt));
    resolver.terminate(resolver.emit<InstJe>(kNullLocation, StringId(), module.scalar.unit, greater, greaterBlock, lessBlock));

    returnOrdering(greaterBlock, 2);
    returnOrdering(lessBlock, 0);

    return function - local;
}

ModulePtr<ClassInstance> generateInstance(Module& module, GlobalPtr<TypeClass> classPointer, Buffer<TypePtr> args,
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

    auto pointer = instance - local;
    registerInstance(module, pointer);
    return pointer;
}

/*
 * The primitive instances.
 */

void defineFromInt(Module& module, TypePtr type) {
    // A vector's literal is the literal in every lane - see emitVectorFromLiteral, and note that
    // `Num` declaring `FromInt` as a superclass is what forces the question to have an answer.
    auto emit = isVectorType(*module.types, type) ? emitVectorFromLiteral : emitFromLiteral;
    IntrinsicMethod methods[] = { { "fromInt"_v, 1, emit } };

    generateInstance(module, classNamed(module, "FromInt"_v), { &type, 1 }, { methods, 1 });
}

// The same split `defineFromInt` makes, and for the same reason: a decimal literal in every lane is
// the only answer that is not arbitrary, and a body generic over a float type needs `0.5` to mean
// something whether the substitution was a `Float` or a `Vec(Float)`. Math's shared approximations
// are what asked for it - every coefficient in one is a written decimal.
void defineFromDecimal(Module& module, TypePtr type) {
    auto emit = isVectorType(*module.types, type) ? emitVectorFromLiteral : emitFromLiteral;
    IntrinsicMethod methods[] = { { "fromDecimal"_v, 1, emit } };

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

/*
 * The divisor test, which is `checkIndexInBounds` for the other operand a program can get wrong.
 *
 * Written as the mistake rather than as the invariant - `b == 0`, not `b != 0` - because that is
 * what `checkCondition` takes and what makes the branch inside it predict the way it will go.
 *
 * The zero is a constant at the *divisor's* type rather than at the instance's, and on JS those can
 * differ: a `@bits` refinement divides at its canonical width, and a comparison built at the wrong
 * one of the two is a comparison the lowering then has to reconcile. Reading it off the operand is
 * free and cannot be wrong.
 */
void emitZeroDivisorCheck(ExprResolver& resolver, ModulePtr<Value> divisor, TypePtr type,
                          LocationId source) {
    if(!divisor || !resolver.checksEnabled()) return;

    // A divisor the program wrote down is one the reader can see, and checking it would emit
    // `checkCondition(2 == 0)` in front of every `x / 2`. Skipped here rather than left to the fold
    // so that a division by a literal is the one instruction it reads as in an unoptimized build
    // too. A literal *zero* is not skipped: it is the mistake this exists to report.
    auto written = resolver.local[divisor];
    if(written->kind == Value::ConstInt && ((ConstInt*)written)->value != 0) return;

    auto divisorType = resolver.valueType(divisor);
    if(!divisorType) return;

    auto zero = resolver.constant<ConstInt>(source, divisorType, 0);
    if(!zero) return;

    auto failed = resolver.ref(resolver.emit<InstCmp>(source, StringId(), resolver.module.scalar.bool_,
                                                      divisor, zero, CompareOp::Eq));

    resolver.emitCheck(failed, source);
}

/*
 * `Num`, and the one method whose emitter depends on the type it is being generated for.
 *
 * An integer `/` carries the check from the ruling beside `Div` in inst.def; a float `/` and a
 * vector `/` are the bare instruction. The split is here rather than inside the emitter for the
 * reason `defineFromInt`'s is: the question is answered once per instance, at the type, and a
 * per-call test would ask it again at every division in the program.
 */
ModulePtr<ClassInstance> defineNum(Module& module, TypePtr type) {
    auto divide = isCheckedDivisionType(*module.types, type) ? emitDivision<Value::Div>
                                                             : emitBinary<Value::Div>;
    IntrinsicMethod methods[] = {
        { "+"_v, 2, emitBinary<Value::Add> },
        { "-"_v, 2, emitBinary<Value::Sub> },
        { "*"_v, 2, emitBinary<Value::Mul> },
        { "/"_v, 2, divide },
        { "-"_v, 1, emitUnary<Value::Neg> },
    };

    return generateInstance(module, classNamed(module, "Num"_v), { &type, 1 }, { methods, 5 });
}

// The same split, and `Integral` needs it for the same reason `Num` does even though every type it
// is generated for is an integer one: a vector of integers reaches here too, and its divisor is the
// per-lane question `checkCondition` cannot ask.
ModulePtr<ClassInstance> defineIntegral(Module& module, TypePtr type) {
    auto remainder = isCheckedDivisionType(*module.types, type) ? emitDivision<Value::Rem>
                                                                : emitBinary<Value::Rem>;
    IntrinsicMethod methods[] = {
        { "%"_v, 2, remainder },
        { "shl"_v, 2, emitBinary<Value::Shl> },
        { "shr"_v, 2, emitBinary<Value::Shr> },
        { "sar"_v, 2, emitBinary<Value::Sar> },
        { "rol"_v, 2, emitBinary<Value::Rol> },
        { "ror"_v, 2, emitBinary<Value::Ror> },
    };

    // The four bitwise names are not here: they are `Bitwise`'s, which this class has as a
    // superclass, so there is one declaration of `and` in the language and no call has to say which
    // class it meant. defineNumeric supplies both instances for every width.
    return generateInstance(module, classNamed(module, "Integral"_v), { &type, 1 }, { methods, 6 });
}

/*
 * `ByteSwap`, whose one method is one instruction - and which is generated here rather than written in
 * `lib/Core/` for the reason every other integer instance is.
 *
 * The class stays source, and that is the half worth keeping: a user's own type may be an instance
 * of it, and R1 admits one plain function per (name, arity) so a `byteSwap` per width could never
 * have been eight functions. What was source and is not any more are the *bodies* - eight shift and
 * mask trees that every target then had to recognize again to reach the instruction it has.
 *
 * Generated for 16, 32 and 64 bits, which is exactly the set that had an instance before. `U8` and
 * `I8` have nothing to swap, and `WideInt` is 53 bits in a 64-bit register - which eight bytes a
 * 53-bit value's are is a question this operation has no answer to, so it keeps not having one.
 */
ModulePtr<ClassInstance> defineByteSwap(Module& module, TypePtr type) {
    IntrinsicMethod methods[] = { { "byteSwap"_v, 1, emitUnary<Value::ByteSwap> } };
    return generateInstance(module, classNamed(module, "ByteSwap"_v), { &type, 1 }, { methods, 1 });
}

// Whether a type is one this operation has an answer for - a whole number of bytes, and the whole of
// the value. See defineByteSwap, and the `ByteSwap` row in inst.def.
bool isByteSwappable(GlobalBase global, TypePtr type) {
    if(global[type]->kind != Type::Int) return false;

    auto& integer = *(IntType*)global[type];
    if(integer.canonical) return false;

    // The target's word is byte-swappable whichever end of its bound it lands on - 32 and 64 both
    // have the instruction - so this answers for `Size` without knowing which. `CodeUnit` is
    // declined by the same reading: eight of its two widths has nothing to reverse.
    if(integer.width == IntType::Word) return true;

    return integer.bits == 16 || integer.bits == 32 || integer.bits == 64;
}

/*
 * `bitWidth` - the one member of `Bits` that is not an instruction, and the reason it is a member.
 *
 * `width - leadingZeros(value)`, with the width written as a constant of the instance's own type.
 * That is what a source body could not have said: a subexpression made only of literals resolves on
 * its own and takes `default FromInt`, so a generic body's `leadingZeros(0)` is an `Int` whatever
 * `a` is. Here the type is in hand and the width is read off it.
 *
 * Two instructions and usually fewer: a constant argument folds the whole thing away, and a
 * `bitWidth` beside a `leadingZeros` of the same value shares the count through CSE.
 */
static ModulePtr<Value> emitBitWidth(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                     LocationId source, StringId resultName) {
    auto& integer = *(IntType*)resolver.global[type];

    /*
     * The width, which for `Size` is not a number this stage has - Analysis-Modules.md Move 2.
     *
     * `sizeOf` in bytes, shifted up by three. That is the same device `InstTypeMetric` exists for
     * and it costs nothing here for the same reason: the metric folds to an immediate wherever the
     * target is known, which is every path that reaches a machine, so the shift is one constant
     * folded against another and `bitWidth(x: Size)` is still one subtraction.
     */
    auto width = integer.isTargetWidth()
        ? resolver.ref(resolver.emit<InstBinary>(source, StringId(), type, Value::Shl,
              resolver.ref(resolver.emit<InstTypeMetric>(source, StringId(), type, type, TypeMetricKind::Size)),
              resolver.makeInt(source, type, 3)))
        : resolver.makeInt(source, type, integer.bits);
    auto zeros = resolver.ref(resolver.emit<InstUnary>(source, StringId(), type,
                                                       Value::LeadingZeros, args[0]));

    return resolver.ref(resolver.emit<InstBinary>(source, resultName, type, Value::Sub, width, zeros));
}

/*
 * `Bits`, whose four methods are three instructions and a subtraction - generated here beside
 * `ByteSwap`, and for every one of its reasons.
 *
 * The class stays source, which is the half worth keeping: a newtype over a word is a plausible
 * instance of it and R1 admits one plain function per (name, arity), so a `countBits` per width
 * could never have been six functions. What is generated are the bodies, which as source would have
 * been the five-step SWAR fold and two smear chains that every backend then had to recognize again
 * to reach the instruction it already has.
 */
ModulePtr<ClassInstance> defineBits(Module& module, TypePtr type) {
    IntrinsicMethod methods[] = {
        { "countBits"_v, 1, emitUnary<Value::CountBits> },
        { "leadingZeros"_v, 1, emitUnary<Value::LeadingZeros> },
        { "trailingZeros"_v, 1, emitUnary<Value::TrailingZeros> },
        { "bitWidth"_v, 1, emitBitWidth },
        { "bitsUpTo"_v, 2, emitBinary<Value::BitsUpTo> },
    };

    return generateInstance(module, classNamed(module, "Bits"_v), { &type, 1 }, { methods, 5 });
}

/*
 * `BitPermute`, whose two methods are one instruction each - generated here beside `Bits`, and for
 * every one of its reasons.
 *
 * The same width test guards both classes, which is not a coincidence and is why `hasBitCounts` is
 * asked for this one too: the widths a permutation is expressible at are the widths the lower IR has
 * a scalar for, exactly as the counts' are. A separate predicate would have been the same expression
 * under a second name and one more thing to keep in step.
 */
ModulePtr<ClassInstance> defineBitPermute(Module& module, TypePtr type) {
    IntrinsicMethod methods[] = {
        { "gatherBits"_v, 2, emitBinary<Value::GatherBits> },
        { "scatterBits"_v, 2, emitBinary<Value::ScatterBits> },
    };

    return generateInstance(module, classNamed(module, "BitPermute"_v), { &type, 1 }, { methods, 2 });
}

// The widths the counts are declared over - 32 and 64, which is the set every target has an
// instruction for and the set `lowerType` has a scalar for. See defineBits and the note in inst.def.
bool hasBitCounts(GlobalBase global, TypePtr type) {
    if(global[type]->kind != Type::Int) return false;

    auto& integer = *(IntType*)global[type];

    // Both ends of the word's bound are in the set, so `Size` joins without the set having to be
    // decided per target - see isByteSwappable, which answers the same shape of question.
    if(integer.width == IntType::Word) return true;

    return integer.bits == 32 || integer.bits == 64;
}

/*
 * `Bitwise` for a type whose values are bits - `Bool` and every integer width.
 *
 * Four methods and not seven. `&&`, `||` and `!` used to be here, defaulting to `and`, `or` and
 * `not`, and moving them out is what stopped `!x` and `if x` disagreeing about the same value: they
 * are questions about truth, they answer `Bool`, and they live beside `Truth` as plain functions.
 * See emitTruthNot below and the note on the three declarations in Core.
 *
 * `not` is `emitLogicalNot` - an `xor` against 1 - rather than a `Not` instruction, because at a
 * `Bool` the two differ: complementing the storage of a one-bit value gives something that is not a
 * `Bool`, and complementing its *value* is exactly this xor. A wider integer overrides it below.
 */
ModulePtr<ClassInstance> defineBitwise(Module& module, TypePtr type, Emit complement) {
    IntrinsicMethod methods[] = {
        { "and"_v, 2, emitBinary<Value::And> },
        { "or"_v, 2, emitBinary<Value::Or> },
        { "xor"_v, 2, emitBinary<Value::Xor> },
        { "not"_v, 1, complement },
    };

    return generateInstance(module, classNamed(module, "Bitwise"_v), { &type, 1 }, { methods, 4 });
}

void defineTruth(Module& module, TypePtr type, Emit emit) {
    IntrinsicMethod methods[] = { { "truthy"_v, 1, emit } };
    generateInstance(module, classNamed(module, "Truth"_v), { &type, 1 }, { methods, 1 });
}

ModulePtr<ClassInstance> defineConversion(Module& module, StringView className, StringView method,
                                          TypePtr from, TypePtr to) {
    TypePtr args[] = { from, to };
    IntrinsicMethod methods[] = { { method, 1, emitCast } };

    return generateInstance(module, classNamed(module, className), { args, 2 }, { methods, 1 });
}

/*
 * `Enum(a)`, for every payload-free sum - Analysis-Language.md §5.1.
 *
 * Generated rather than declared, and generated for every such type rather than for the ones that
 * pinned a value, because both functions are decided entirely by the declaration: what `valueOf`
 * answers is what the type already *is*, and what `fromValue` accepts is the set of numbers written
 * in it. There is nothing an author could contribute, which is the same reason `Num(Int)` is
 * generated.
 *
 * Reached through the on-demand hook rather than emitted at every `data` declaration, on the terms
 * `vectorInstance` set: it runs only where an instance lookup found nothing, so it cannot shadow a
 * declared head, and a program that never asks for one never pays for it.
 */
namespace {

// The payload-free sum an `Enum` head names, or nothing. Asked of the *layout* rather than of the
// constructor list, because that is the property both emitters depend on: an enum is its number.
static RecordType* enumHead(GlobalBase global, TypePtr type) {
    if(!type) return nullptr;

    auto value = global[type];
    if(value->kind != Type::Record) return nullptr;

    auto record = (RecordType*)value;
    return record->layout == RecordType::Enum && record->constructors.size() ? record : nullptr;
}

/*
 * The signed integer type an enum's values are *the numbers the declaration wrote* at, or null where
 * reading them as they stand already gives those numbers.
 *
 * Null is the answer for every enum whose values are all non-negative, which is the common case and
 * the one that costs nothing.
 *
 * **A negative `@value` needs the step, and the reason is a rule one level down.** The cast lowering
 * widens with the sign only between two things it agrees are integers, and a payload-free sum is a
 * *record* to that test however much it is a number here - so `cast Signal -> I64` in one step zero-
 * extends whatever `signedType` says, and `@value(-1)` arrives as 4294967295. Going through `Int`
 * first makes the widening that matters a widening between two integers, which is the one the rule
 * covers. `I64` instead where the declaration's values need it, which no ABI this has met asks for.
 *
 * Everything that turns one of these into a number goes through here - `valueOf`, `nameOf` and the
 * generated comparisons - which is what it is for. It used to sit under the comparisons and be
 * reachable only from them, and the two emitters above widened in one step on their own:
 * `valueOf(Failed)` answered 4294967295 in every build, and `nameOf` compared that against the `-1`
 * the declaration wrote, missed every arm and fell through to the last constructor, so `Failed`
 * printed as `Running`. See test/bench/findings.md §69.
 */
static TypePtr enumNumberType(ExprResolver& resolver, RecordType& record) {
    auto& module = resolver.module;
    auto negative = false;
    auto wide = false;

    for(auto constructor: record.constructors.contents(resolver.global)) {
        if(constructor.value < 0) negative = true;
        if(constructor.value < minLimit<I32> || constructor.value > maxLimit<I32>) wide = true;
    }

    if(!negative) return nullptr;
    return wide ? module.scalar.long_ : module.scalar.int_;
}

// The value at that type, where one is called for. A `Cast` rather than a `Bitcast`: what is wanted
// is the *number*, so a one-byte `-1` has to arrive as `-1` and not as 255.
static ModulePtr<Value> enumAsNumber(ExprResolver& resolver, ModulePtr<Value> value, TypePtr at,
                                     LocationId source) {
    if(!at) return value;
    return resolver.ref(resolver.emit<InstUnary>(source, StringId(), at, Value::Cast, value));
}

// The same, for a caller that has a value and its type rather than the record - every caller outside
// the comparisons, which already looked the record up to decide what to compare at.
static ModulePtr<Value> enumSignedNumber(ExprResolver& resolver, ModulePtr<Value> value, TypePtr type,
                                         LocationId source) {
    auto record = enumHead(resolver.global, type);
    return record ? enumAsNumber(resolver, value, enumNumberType(resolver, *record), source) : value;
}

// `valueOf`: the number, which the value already is. A `Cast` and nothing else, which is the whole
// of what §5.1 was asking for - the two compares and two selects it replaces were a `match`
// computing the identity function.
ModulePtr<Value> emitEnumValue(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                               LocationId source, StringId resultName) {
    auto number = enumSignedNumber(resolver, args[0], resolver.valueType(args[0]), source);
    return resolver.ref(resolver.emit<InstUnary>(source, resultName, type, Value::Cast, number));
}

/*
 * Whether `number` is one of the values this declaration names.
 *
 * Two shapes, and which one applies is a property of the declaration rather than a choice. Values
 * that run consecutively are a range, and a range is two comparisons however many constructors there
 * are - which is every enum that pins nothing, so the common case does not pay for the feature. A set
 * with holes in it is one comparison per constructor, or-ed together: no branches, no blocks, and an
 * expression the optimizer is free to make a table of.
 */
static ModulePtr<Value> enumMembership(ExprResolver& resolver, RecordType& record, ModulePtr<Value> number,
                                       LocationId source) {
    auto global = resolver.global;
    auto& module = resolver.module;
    auto bool_ = module.scalar.bool_;
    auto long_ = module.scalar.long_;

    auto range = enumRange(global, record);
    auto constructors = record.constructors.contents(global);
    auto consecutive = U64(range.highest - range.lowest) + 1 == U64(constructors.size());

    if(consecutive) {
        auto low = resolver.makeInt(source, long_, U64(range.lowest));
        auto high = resolver.makeInt(source, long_, U64(range.highest));

        auto atLeast = resolver.ref(resolver.emit<InstCmp>(source, StringId(), bool_, number, low, CompareOp::Ge));
        auto atMost = resolver.ref(resolver.emit<InstCmp>(source, StringId(), bool_, number, high, CompareOp::Le));

        return resolver.ref(resolver.emit<InstBinary>(source, StringId(), bool_, Value::And, atLeast, atMost));
    }

    ModulePtr<Value> held = nullptr;

    for(auto constructor: constructors) {
        auto pinned = resolver.makeInt(source, long_, U64(constructor.value));
        auto matches = resolver.ref(resolver.emit<InstCmp>(source, StringId(), bool_, number, pinned, CompareOp::Eq));

        held = held ? resolver.ref(resolver.emit<InstBinary>(source, StringId(), bool_, Value::Or, held, matches))
                    : matches;
    }

    return held;
}

// Which constructor of `Maybe(a)` is which, found by name rather than by position - the same rule
// `definePreludeLookups` reads `Outcome`'s by, and for the same reason: this is emitted code with no
// declaration in front of it to fix an order.
static U32 constructorNamed(GlobalBase global, RecordType& record, StringView name) {
    auto wanted = Context::nameHash(name);

    for(auto constructor: record.constructors.contents(global)) {
        if(constructor.name == wanted) return constructor.index;
    }

    return maxLimit<U32>;
}

/*
 * `fromValue`: the constructor a number names, or nothing.
 *
 * The partial half, and the reason the class has two functions rather than one conversion in each
 * direction. A number the declaration does not name is not one of these values, and there is no
 * answer to give for it - so the result carries the question, which is what `Maybe` is.
 *
 * The `Just` payload is a `Cast` of the number, not a lookup: past the membership test the number
 * *is* the constructor, because that is what pinning a value means.
 */
ModulePtr<Value> emitEnumFromValue(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                   LocationId source, StringId resultName) {
    auto global = resolver.global;
    auto maybe = (RecordType*)global[type];
    if(global[type]->kind != Type::Record) return nullptr;

    auto just = constructorNamed(global, *maybe, "Just"_v);
    auto nothing = constructorNamed(global, *maybe, "Nothing"_v);
    if(just == maxLimit<U32> || nothing == maxLimit<U32>) return nullptr;

    auto payload = maybe->constructors.get(global, just).content;
    auto record = enumHead(global, payload);
    if(!record) return nullptr;

    auto condition = enumMembership(resolver, *record, args[0], source);
    if(!condition) return nullptr;

    auto present = resolver.addBlock();
    auto absent = resolver.addBlock();
    resolver.terminate(resolver.emit<InstJe>(source, StringId(), resolver.module.scalar.unit,
                                             condition, present, absent));

    BranchArmList arms;

    /*
     * The number read as the constructor, in two steps rather than one.
     *
     * `Cast` answers "how does the value change on the way", and between an `I64` and a payload-free
     * sum there are two different answers stacked on top of each other: the number narrows to the
     * width the sum is held in, and then the bits are read as that sum. Asking one instruction for
     * both left the folder with a numeric conversion whose target has no width to convert *to* - a
     * record is not an integer type, so every rule that reads one declines - and a constant input
     * came out of the branch as the wrong constructor.
     *
     * Split, each half is a shape that already existed: a narrowing between two integers, and a
     * reinterpretation between two things of one width. Which is also what the operation *is*.
     */
    resolver.current = present;
    auto narrowed = resolver.ref(resolver.emit<InstUnary>(source, StringId(), resolver.module.scalar.int_,
                                                         Value::Cast, args[0]));
    auto constructed = resolver.ref(resolver.emit<InstUnary>(source, StringId(), payload, Value::Bitcast, narrowed));
    arms.push(BranchArm { resolver.current, resolver.makeConstructed(type, just, constructed, source), source });

    resolver.current = absent;
    arms.push(BranchArm { resolver.current, resolver.makeConstructed(type, nothing, nullptr, source), source });

    return resolver.finishBranches(arms, source, true);
}

/*
 * `nameOf`: the constructor's own word.
 *
 * A chain of comparisons against the numbers the declaration pinned, each arm producing one string
 * constant, joined by a phi - which is what the `match` in `Show(FileError)` was, written out by the
 * compiler from the declaration instead of by hand from a list that could drift from it.
 *
 * The last constructor is the fall-through rather than a comparison of its own, so an N-constructor
 * type costs N-1 compares and not N. That is sound because the input is one of these values: an
 * `Enum` head is a payload-free sum, and every bit pattern one of those can hold is a constructor
 * the declaration named. It is also what a hand-written `match` with a final `_` produces.
 *
 * Constant-folds away entirely at a site where the constructor is known, which is most of them: the
 * comparisons are against a constant and the arms are constants.
 */
static ModulePtr<Value> emitEnumNameOf(ExprResolver& resolver, ModulePtr<Value> value, RecordType& record,
                                       LocationId source) {
    auto global = resolver.global;
    auto& module = resolver.module;
    auto bool_ = module.scalar.bool_;
    auto long_ = module.scalar.long_;
    auto unit = module.scalar.unit;

    auto constructors = record.constructors.contents(global);

    // Through `enumNumberType` for the reason that function gives: the arms below are the numbers the
    // declaration wrote, so the value has to arrive as the number it wrote too.
    auto asNumber = enumAsNumber(resolver, value, enumNumberType(resolver, record), source);
    auto number = resolver.ref(resolver.emit<InstUnary>(source, StringId(), long_, Value::Cast, asNumber));

    BranchArmList arms;

    for(Size i = 0; i + 1 < constructors.size(); i++) {
        auto pinned = resolver.makeInt(source, long_, U64(constructors[i].value));
        auto matches = resolver.ref(resolver.emit<InstCmp>(source, StringId(), bool_, number, pinned, CompareOp::Eq));

        auto hit = resolver.addBlock();
        auto next = resolver.addBlock();
        resolver.terminate(resolver.emit<InstJe>(source, StringId(), unit, matches, hit, next));

        resolver.current = hit;
        auto text = resolver.resolveString(source, constructors[i].name);
        if(!text) return nullptr;
        arms.push(BranchArm { resolver.current, text, source });

        resolver.current = next;
    }

    auto last = resolver.resolveString(source, constructors[constructors.size() - 1].name);
    if(!last) return nullptr;
    arms.push(BranchArm { resolver.current, last, source });

    return resolver.finishBranches(arms, source, true);
}

ModulePtr<Value> emitEnumName(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr,
                              LocationId source, StringId) {
    auto record = enumHead(resolver.global, resolver.valueType(args[0]));
    if(!record) return nullptr;

    return emitEnumNameOf(resolver, args[0], *record, source);
}

/*
 * A class function of a *known* class, applied to values whose types select the instance.
 *
 * The route `ExprResolver::truthy` takes, for the same reason it takes it: selecting against the
 * class rather than by name means a module that happens to define a plain `length` cannot take over
 * what this emits, and it means the call goes through whatever stands in the slot - a parametric
 * head's generic body, or the class's own default - rather than assuming an implementation.
 *
 * This is where `fromName` reaches out of Core. `Eq(String)` and `Length(String)` are `Text`'s and
 * `Collections`' respectively, and both are `@platform`-split; nothing here knows that, which is the
 * point of asking the class. Instance lookup is program-wide (see findInstances), so what a module
 * imported does not decide which of the two answers.
 */
static ModulePtr<Value> callClassFun(ExprResolver& resolver, GlobalPtr<TypeClass> typeClass, StringView name,
                                     Buffer<ResolvedArg> args, TypePtr result, LocationId source) {
    if(!typeClass) return nullptr;

    auto global = resolver.global;
    auto wanted = Context::nameHash(name);
    auto functions = global[typeClass]->functions;

    for(Size i = 0; i < functions.size(); i++) {
        auto entry = functions.get(global, i);
        if(entry.name != wanted || entry.arity != args.length) continue;

        ClassFunRef reference { typeClass, entry.name, U16(i) };
        ClassMatch match;

        if(!resolver.matchClassFun(reference, args, {}, result, match)) return nullptr;
        if(!match.instance) return nullptr;

        return resolver.emitInstanceCall(resolver.module, match.instance, toBuffer(match.instanceArgs),
                                         match.index, args, source);
    }

    return nullptr;
}

/*
 * `fromName`: the constructor a word names, or nothing.
 *
 * Two levels, and the outer one is why: the names are grouped by length, a single `length` of the
 * input picks the group, and only the names that could possibly match are compared. An enum whose
 * constructors are mostly of different lengths therefore pays one integer compare per *distinct
 * length* plus one string compare, rather than one string compare per constructor.
 *
 * What that saves is the calls and not the comparisons. `Eq(String).==` opens by comparing lengths
 * itself - it has to, since that is what makes its loop's bound safe - so the second gate would be
 * redundant if the call were free. It is not free: nineteen `errno` constructors are nineteen calls
 * to `length` and nineteen to `==` before any optimizer runs, and this is one and one.
 *
 * The comparison inside a group is a chain and is honest about being one. A later pass may turn it
 * into something with a hash in it; as written it is what a hand-written parser would have contained,
 * which is the standard this file holds generated code to.
 */
ModulePtr<Value> emitEnumFromName(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                  LocationId source, StringId) {
    auto global = resolver.global;
    auto& module = resolver.module;
    auto& program = module.program;

    auto maybe = (RecordType*)global[type];
    if(global[type]->kind != Type::Record) return nullptr;

    auto just = constructorNamed(global, *maybe, "Just"_v);
    auto nothing = constructorNamed(global, *maybe, "Nothing"_v);
    if(just == maxLimit<U32> || nothing == maxLimit<U32>) return nullptr;

    auto payload = maybe->constructors.get(global, just).content;
    auto record = enumHead(global, payload);
    if(!record) return nullptr;

    auto bool_ = module.scalar.bool_;
    auto size = module.scalar.size;
    auto unit = module.scalar.unit;
    auto string_ = module.scalar.string_;

    auto eq = program.core ? classNamed(*program.core, "Eq"_v) : nullptr;
    auto length = program.core ? classNamed(*program.core, "Length"_v) : nullptr;
    if(!eq || !length) return nullptr;

    // The distinct lengths, in declaration order, with the constructors at each. A `SmallArray`
    // per group would be a list of lists; the two parallel walks below cost nothing on the sizes
    // an enum actually has and allocate nothing at all.
    auto constructors = record->constructors.contents(global);
    auto nameLength = [&](Size index) {
        return module.context.findName(constructors[index].name).size();
    };

    ResolvedArg lengthArgs[] = { args[0] };
    auto given = callClassFun(resolver, length, "length"_v, { lengthArgs, 1 }, size, source);
    if(!given) return nullptr;

    BranchArmList arms;

    for(Size i = 0; i < constructors.size(); i++) {
        // Only at the first constructor of each length; a later one of the same length is compared
        // inside that length's group rather than opening a second one.
        auto first = true;
        for(Size j = 0; j < i; j++) {
            if(nameLength(j) == nameLength(i)) { first = false; break; }
        }

        if(!first) continue;

        auto wanted = resolver.makeInt(source, size, U64(nameLength(i)));
        auto matches = resolver.ref(resolver.emit<InstCmp>(source, StringId(), bool_, given, wanted, CompareOp::Eq));

        auto group = resolver.addBlock();
        auto next = resolver.addBlock();
        resolver.terminate(resolver.emit<InstJe>(source, StringId(), unit, matches, group, next));

        resolver.current = group;

        for(Size j = i; j < constructors.size(); j++) {
            if(nameLength(j) != nameLength(i)) continue;

            auto text = resolver.resolveString(source, constructors[j].name);
            if(!text) return nullptr;

            ResolvedArg equalArgs[] = { args[0], text };
            auto equal = callClassFun(resolver, eq, "=="_v, { equalArgs, 2 }, bool_, source);
            if(!equal) return nullptr;

            auto hit = resolver.addBlock();
            auto miss = resolver.addBlock();
            resolver.terminate(resolver.emit<InstJe>(source, StringId(), unit, equal, hit, miss));

            resolver.current = hit;

            // The constructor as a value, in the two steps `fromValue` takes and for the same reason:
            // the number narrows to the width the sum is held in, and then the bits are read as that
            // sum. One `Cast` asked for both left the folder with a conversion whose target has no
            // width to convert to.
            auto pinned = resolver.makeInt(source, module.scalar.int_, U64(constructors[j].value));
            auto constructed = resolver.ref(resolver.emit<InstUnary>(source, StringId(), payload,
                                                                     Value::Bitcast, pinned));
            arms.push(BranchArm { resolver.current, resolver.makeConstructed(type, just, constructed, source), source });

            resolver.current = miss;
        }

        // Out of this length's group with nothing matched: the input is the right length and none of
        // these words, which is as final an answer as the outer chain running out.
        arms.push(BranchArm { resolver.current, resolver.makeConstructed(type, nothing, nullptr, source), source });
        resolver.current = next;
    }

    arms.push(BranchArm { resolver.current, resolver.makeConstructed(type, nothing, nullptr, source), source });
    return resolver.finishBranches(arms, source, true);
}

/*
 * `Show` over a payload-free sum, which is the derived instance and not a fourth `Enum` function.
 *
 * What it writes is the constructor's name, which is what a derived `Show` prints in every language
 * that has one and what `Show(FileError)` was already doing by hand. The prose form of an error - 
 * `describeError` - stays a function beside the type, because it is a second answer about the value
 * rather than the same one written out.
 */
static ModulePtr<Value> emitEnumShow(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr,
                                     LocationId source, StringId) {
    auto& module = resolver.module;
    auto pushString = module.program.pushString;

    auto record = enumHead(resolver.global, resolver.valueType(args[0]));
    if(!record || !pushString) return nullptr;

    auto text = emitEnumNameOf(resolver, args[0], *record, source);
    if(!text) return nullptr;

    // Through `borrowArgument` rather than by handing the parameter on, for the reason
    // `resolveFormat` does the same: `&to` arrives as an address and `pushString`'s `&self` wants a
    // borrow of the storage it names, and the two are only the same thing by accident of the level.
    // What this emits is exactly what the hand-written `Show(String).show` emits.
    auto borrowed = resolver.borrowArgument(args[1], module.scalar.string_, source);
    if(!borrowed) return nullptr;

    (*module.arena)[pushString]->used = true;
    auto call = resolver.create<InstCall>(source, StringId(), module.scalar.unit, pushString);
    call->args.push(module.arena, borrowed);
    call->args.push(module.arena, text);
    resolver.append(call);

    return nullptr;
}

/*
 * `showBound`: the longest name, which is a compile-time constant here in the way the class says it
 * should be - an ordinary expression the folder sees through, not a second type-level channel.
 *
 * It is exact rather than generous, which is the contract `Show` states: the buffer is sized from
 * this, so a bound larger than the truth wastes the difference and a bound smaller than the truth is
 * the one thing an instance may not do. The longest constructor name is both.
 */
static ModulePtr<Value> emitEnumShowBound(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                          LocationId source, StringId) {
    auto global = resolver.global;
    auto& module = resolver.module;

    auto maybe = (RecordType*)global[type];
    if(global[type]->kind != Type::Record) return nullptr;

    auto just = constructorNamed(global, *maybe, "Just"_v);
    if(just == maxLimit<U32>) return nullptr;

    auto record = enumHead(global, resolver.valueType(args[0]));
    if(!record) return nullptr;

    Size longest = 0;
    for(auto constructor: record->constructors.contents(global)) {
        auto text = module.context.findName(constructor.name).size();
        if(text > longest) longest = text;
    }

    auto payload = maybe->constructors.get(global, just).content;
    auto bound = resolver.makeInt(source, payload, U64(longest));
    return resolver.makeConstructed(type, just, bound, source);
}

}

ModulePtr<ClassInstance> enumInstance(Module& module, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args) {
    auto& program = module.program;
    if(!typeClass || !program.core || typeClass != program.coreClasses.enum_) return nullptr;
    if(args.length != 1 || !enumHead(*program.types, args[0])) return nullptr;

    IntrinsicMethod methods[] = {
        { "valueOf"_v, 1, emitEnumValue },
        { "fromValue"_v, 1, emitEnumFromValue, nullptr, false },
        { "nameOf"_v, 1, emitEnumName, nullptr, false },
        { "fromName"_v, 1, emitEnumFromName, nullptr, false },
    };

    return generateInstance(*program.core, typeClass, args, { methods, 4 });
}

/*
 * `Show` for a payload-free sum, generated where it is asked for - Analysis-Derive.md's `variant`
 * template, for the one shape whose expansion needs no repetition form.
 *
 * On the terms `vectorInstance` and `enumInstance` set, which is what makes an automatic instance
 * safe here: it is consulted only where an instance lookup found nothing, so a declared `Show` for an
 * enum still wins - `Show(Bool)` in `Text` is the case that proves it, since `Bool` is a payload-free
 * sum with an instance somebody wrote. What this answers is the enums nobody wrote one for.
 *
 * Automatic rather than a `deriving (Show)` clause, and the trade is worth stating because it cuts
 * against the principle the clause exists for. What a type does is normally written where the type
 * is; here it is not. The reason it is defensible for this one class and this one shape is that the
 * answer is not a choice: a constructor with no payload has exactly one text form, which is its name,
 * and every enum in this tree that wrote a `Show` wrote that. When the clause reaches `data`, this
 * stays as the default a type may still override by declaring its own.
 */
ModulePtr<ClassInstance> enumShowInstance(Module& module, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args) {
    auto& program = module.program;
    // `pushString` rather than the module holding it, because that is what the body actually needs
    // and it is recorded a hook later. A `Show` generated without it would be an instance whose
    // `show` writes nothing, which is worse than no instance.
    if(!typeClass || !program.core || !program.pushString || typeClass != program.coreClasses.show) return nullptr;
    if(args.length != 1 || !enumHead(*program.types, args[0])) return nullptr;

    IntrinsicMethod methods[] = {
        { "show"_v, 2, emitEnumShow, nullptr, false },
        { "showBound"_v, 1, emitEnumShowBound, nullptr, false },
    };

    return generateInstance(*program.core, typeClass, args, { methods, 2 });
}

/*
 * `Eq` and `Ord` for a payload-free sum, generated on the terms `enumShowInstance` set.
 *
 * The boilerplate this removes was one line per enum and the line was always the same -
 * `valueOf(lhs) == valueOf(rhs)` - which is the shape of a thing that should not be written. It was
 * also the *wrong* line: `valueOf` answers `I64`, which on JS is a `bigint`, so comparing two
 * weekdays there converted both to a `bigint` and compared those. What these emit is the comparison
 * the values already are, at the width the declaration gave them.
 *
 * **Nothing is cast.** A payload-free sum *is* its discriminant, so the operands go to the
 * comparison as they stand and the operation happens at the type's own width - one byte for a
 * two-constructor enum. That is what the `valueOf` route could not do, since the class fixes the
 * width it reports in.
 *
 * **A negative `@value` is the one case that names a type to compare at**, and where the operands
 * are widened they are widened to a signed integer: `Int` where the values fit it, which is a
 * `number` on JS and free on every native target; `I64` only for a declaration whose values need it,
 * which no ABI this has met asks for.
 *
 * That widening used to be load-bearing, because `signedOperand` answered a record with false and
 * `@value(-1)` would otherwise sort above everything. **It is belt-and-braces now**: `signedType`
 * reads a payload-free sum's own values, so the bare comparison below is already the signed one for
 * such a declaration. Keeping it costs a cast the folder removes and keeps this emitter honest about
 * the width it compares at, rather than resting on a predicate two files away. What it never was is
 * the *whole* fix - a negative sum read wrong through any erased boundary, which no amount of
 * casting here could reach. See test/bench/findings.md §69.
 *
 * The common case - every enum that pins nothing, and every one whose numbers are all non-negative -
 * has no cast at all and is one instruction.
 */
namespace {

template<CompareOp op>
static ModulePtr<Value> emitEnumCompareOp(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                          LocationId source, StringId resultName) {
    auto record = enumHead(resolver.global, resolver.valueType(args[0]));
    auto at = record ? enumNumberType(resolver, *record) : nullptr;

    return resolver.ref(resolver.emit<InstCmp>(source, resultName, resolver.module.scalar.bool_,
                                               enumAsNumber(resolver, args[0], at, source),
                                               enumAsNumber(resolver, args[1], at, source), op));
}

/*
 * `compare`, as the two comparisons an `Ordering` is made of.
 *
 * `lhs < rhs ? LT : (lhs == rhs ? EQ : GT)`, written with selects rather than branches so that it
 * costs no blocks: an `Ordering` is a number here as much as the operands are, and each arm is a
 * constant. The four relational operators above never reach this at all - they are emitted as their
 * own comparison - so what this serves is a caller that wanted the three-way answer.
 */
static ModulePtr<Value> emitEnumCompare(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                        LocationId source, StringId resultName) {
    auto& module = resolver.module;
    auto bool_ = module.scalar.bool_;

    auto record = enumHead(resolver.global, type);
    if(!record || record->constructors.size() != 3) return nullptr;

    auto constructors = record->constructors.contents(resolver.global);

    // An `Ordering` constructor as a value, in `fromValue`'s two steps and for its reason: the
    // number narrows to the width the sum is held in, and then the bits are read as that sum.
    auto ordering = [&](Size index) {
        auto pinned = resolver.makeInt(source, module.scalar.int_, U64(constructors[index].value));
        return resolver.ref(resolver.emit<InstUnary>(source, StringId(), type, Value::Bitcast, pinned));
    };

    auto operands = enumHead(resolver.global, resolver.valueType(args[0]));
    auto at = operands ? enumNumberType(resolver, *operands) : nullptr;
    auto lhs = enumAsNumber(resolver, args[0], at, source);
    auto rhs = enumAsNumber(resolver, args[1], at, source);

    auto less = resolver.ref(resolver.emit<InstCmp>(source, StringId(), bool_, lhs, rhs, CompareOp::Lt));
    auto same = resolver.ref(resolver.emit<InstCmp>(source, StringId(), bool_, lhs, rhs, CompareOp::Eq));

    auto tail = resolver.ref(resolver.emit<InstSelect>(source, StringId(), type, same,
                                                       ordering(1), ordering(2)));
    return resolver.ref(resolver.emit<InstSelect>(source, resultName, type, less, ordering(0), tail));
}

}

ModulePtr<ClassInstance> enumEqInstance(Module& module, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args) {
    auto& program = module.program;
    if(!typeClass || !program.core || typeClass != program.coreClasses.eq) return nullptr;
    if(args.length != 1 || !enumHead(*program.types, args[0])) return nullptr;

    // `==` only. `!=` has a default written over it in the class, and one comparison negated is what
    // that default already is.
    IntrinsicMethod methods[] = { { "=="_v, 2, emitEnumCompareOp<CompareOp::Eq> } };

    return generateInstance(*program.core, typeClass, args, { methods, 1 });
}

ModulePtr<ClassInstance> enumOrdInstance(Module& module, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args) {
    auto& program = module.program;
    if(!typeClass || !program.core || typeClass != program.coreClasses.ord) return nullptr;
    if(args.length != 1 || !enumHead(*program.types, args[0])) return nullptr;

    /*
     * The four operators as well as `compare`, though the class defaults every one of them over it.
     *
     * A default would make `a < b` build an `Ordering` and then compare *that* against `LT` - three
     * operations and a constant where the machine has one instruction. Emitting them here is the
     * same argument `bitWidth` makes in `Bits`: the generic body is correct and is not what the
     * operation is.
     *
     * `min` and `max` keep their defaults, since those are written over `<=` and `>=` and are
     * therefore already one comparison and a select once these exist.
     */
    IntrinsicMethod methods[] = {
        { "compare"_v, 2, emitEnumCompare, nullptr, false },
        { "<"_v, 2, emitEnumCompareOp<CompareOp::Lt> },
        { "<="_v, 2, emitEnumCompareOp<CompareOp::Le> },
        { ">"_v, 2, emitEnumCompareOp<CompareOp::Gt> },
        { ">="_v, 2, emitEnumCompareOp<CompareOp::Ge> },
    };

    return generateInstance(*program.core, typeClass, args, { methods, 5 });
}

ModulePtr<ClassInstance> defineBitcast(Module& module, TypePtr from, TypePtr to, GlobalPtr<GenEnv> gen) {
    TypePtr args[] = { from, to };
    IntrinsicMethod methods[] = { { "bitcast"_v, 1, emitBitcast } };

    return generateInstance(module, classNamed(module, "Bitcast"_v), { args, 2 }, { methods, 1 }, gen);
}

void attachIntrinsic(Module& module, StringView name, Intrinsic intrinsic) {
    auto found = module.functions.get(Context::nameHash(name));

    if(!found) {
        module.context.diagnostics.error("internal: no declaration of the intrinsic %@"_v, kNullLocation, name);
        return;
    }

    (*module.arena)[found.unwrap()]->intrinsic = intrinsic;
}

// The same, for a hook that wants its `@lazy` arguments unevaluated. `&&` and `||` are the only two,
// and they need it for the reason the marker exists: what arrives is a thunk over the caller's
// frame, and the whole point of the expansion is to emit it under a branch rather than call it.
void attachDeferredIntrinsic(Module& module, StringView name, DeferredIntrinsic intrinsic) {
    auto found = module.functions.get(Context::nameHash(name));

    if(!found) {
        module.context.diagnostics.error("internal: no declaration of the intrinsic %@"_v, kNullLocation, name);
        return;
    }

    (*module.arena)[found.unwrap()]->deferredIntrinsic = intrinsic;
}

/*
 * The built-in containers' element and length accessors - Implementation-Simplification.md §2.
 *
 * `xs[i]` is the most common expression the language has, and until these were generated it cost a
 * call and, on JS, a heap allocation per read. Not because anything about it is hard: each of these
 * instances is one line over an operation that already emits exactly the right IR - an address
 * computation and a borrow of the place it names - but because the *instance method* was an ordinary
 * function, and the one thing that could have seen through it declines to. opt_inline.cpp refuses
 * any callee with a `return` parameter, which is every accessor here.
 *
 * So they are generated, exactly as `Num(Int).+` is, and each emitter below is the whole definition
 * of its instance. Every one of them could be written in Yana - the comment above each declaration
 * in `Native` and `Collections` says what that would read like, and why it is not - and the reason
 * they are not is the reason `Num(Int).+` is not: reaching them must cost nothing, and an intrinsic
 * is the only form that costs nothing *without an optimizer having run*. A source body would have to
 * be specialized per element type and then spliced per call site before it was free, so a build with
 * `-no-opt` would pay for a call at every subscript in the program, and the shape of the emitted code
 * would depend on a pass having run rather than on what was written.
 *
 * The generated body and the expansion are the same `Emit` - see generateInstanceFunction, and the
 * note on `Emit` itself - so what a witness slot reaches and what a call site expands to cannot
 * disagree. That is the property a hand-written hook beside a source body does not have.
 *
 * What is *not* generated is anything about the conventions. `return self` and `&self` are declared
 * once, by `class Index`, and generateInstanceFunction copies them off that signature; what each one
 * means at a call site is spent in receiverPlace below, since expandIntrinsic applies none of them.
 *
 * **No bounds check, on either target and in either direction.** That is what these instances have
 * always done; whether a subscript should check is Implementation-Containers.md §15's decision and
 * does not ride in on this one.
 */

namespace {

/*
 * The container this call was handed, as the place its fields are read out of.
 *
 * The argument conventions are applied here because expandIntrinsic applies none - it hands the
 * emitter the values and lets the operation decide - and for these three instances the conventions
 * are load-bearing in two different ways.
 *
 *   `Mutable` is `getMut`'s `&self`. It is what rejects `xs[0] = 1` on an immutable binding, and
 *   what makes the write exclusive for as long as the element borrow lives.
 *
 *   `Loaned` is `get`'s `return self`. The borrow handed back names storage inside the container, so
 *   the loan has to outlive the element - and this is the one thing that expanding the call would
 *   otherwise throw away, because the address the element is read through is a *raw pointer* loaded
 *   out of the container and a pointer root carries no provenance back to anything. Without it the
 *   load of `run.items` was the last use of the array and the drop pass released the run between
 *   that load and the read through it.
 *
 *   `Read` is `length`'s plain `self`, which promises nothing and needs no loan.
 *
 * A pointer and a borrow are the root itself rather than something that has to be in storage first,
 * exactly as in resolveField - and a pointer receiver takes no loan for the reason emitDirectCall
 * gives at its own `return` argument: a scalar was passed by value, so a borrow rooted in the
 * caller's copy would be a borrow of the wrong thing.
 */
enum class Receiver: U8 { Read, Loaned, Mutable };

template<Receiver mode>
Maybe<Place> receiverPlace(ExprResolver& resolver, ModulePtr<Value> self, LocationId source) {
    if(!self) return Nothing();
    auto held = resolver.valueType(self);

    if(mode == Receiver::Mutable) {
        auto borrowed = resolver.borrowArgument(self, held, source, true);
        return borrowed ? Just(Place::inBorrow(borrowed)) : Nothing();
    }

    if(isPointer(resolver.global, held)) return Just(Place::atPointer(self));
    if(isBorrow(resolver.global, held)) return Just(Place::inBorrow(self));

    auto place = resolver.findPlace(self);
    if(!place) return Just(resolver.materialize(self, source));
    if(mode == Receiver::Read) return place;

    auto borrowed = resolver.borrowPlace(place.unwrap(), resolveBorrowType(resolver.module, held, false),
                                         source, true);
    return borrowed ? Just(Place::inBorrow(borrowed)) : Nothing();
}

// One field of a container, as a place. The owner's own storage rather than a copy of it, so `xs[i]`
// reads the descriptor where it already is - which is also what roots the borrow in the array.
Maybe<Place> containerField(ExprResolver& resolver, const Place& owner, StringView field,
                            LocationId source) {
    return resolver.projectField(owner, Context::nameHash(field), source, source);
}

template<Receiver mode>
Maybe<Place> selfField(ExprResolver& resolver, ModulePtr<Value> self, StringView field, LocationId source) {
    auto place = receiverPlace<mode>(resolver, self, source);
    return place ? containerField(resolver, place.unwrap(), field, source) : Nothing();
}

template<Receiver mode>
ModulePtr<Value> selfFieldValue(ExprResolver& resolver, ModulePtr<Value> self, StringView field,
                                LocationId source) {
    auto place = selfField<mode>(resolver, self, field, source);
    return place ? resolver.load(place.unwrap(), source) : nullptr;
}

/*
 * `index` is inside `length` - Implementation-Containers.md §15.
 *
 * One comparison rather than two, and that is what the unsigned reading buys. `Size` is signed
 * because it is the type an `Int` index widens into, so `xs[-1]` is a perfectly ordinary value of
 * it; read as an unsigned number of the same width, a negative index is above every length there is
 * and fails the same test the too-large case fails. It is the idiom `remove` in Collections already
 * writes as `(index :: U32) >= self.length`, said once here for every subscript in the program.
 *
 * The cast is free on both targets - the two types have one width - and the length is widened into
 * it rather than the other way round, since a stored count is narrower than a `Size`.
 *
 * Nothing is emitted at all when the checks are off, including the length load: a load whose result
 * nothing reads is still a load until something removes it, and this runs before any optimizer.
 */
void checkIndexInBounds(ExprResolver& resolver, ModulePtr<Value> index, ModulePtr<Value> length,
                        LocationId source) {
    if(!index || !length || !resolver.checksEnabled()) return;

    auto word = resolver.module.scalar.unsignedSize;
    if(!word) return;

    auto unsignedIndex = resolver.ref(resolver.emit<InstUnary>(source, StringId(), word, Value::Cast, index));

    // The length reaches the same type by widening where it can - a stored `Count` is narrower than
    // an unsigned `Size` - and by reading the same bits where it cannot, which is what happens on JS:
    // `Size` is `Int` there, so the length and the word it is compared at are one width and there is
    // nothing to widen. That second case is a conversion the compiler builds for itself and knows the
    // value of, so it is emitted rather than asked for: `::` may not narrow, and this is not one.
    auto unsignedLength = resolver.convertibleType(resolver.valueType(length), word)
        ? resolver.convert(length, word, source)
        : resolver.ref(resolver.emit<InstUnary>(source, StringId(), word, Value::Cast, length));

    if(!unsignedLength) return;

    auto failed = resolver.ref(resolver.emit<InstCmp>(source, StringId(), resolver.module.scalar.bool_,
                                                      unsignedIndex, unsignedLength, CompareOp::Ge));

    resolver.emitCheck(failed, source);
}

// `borrow(base + index)` - an element of a run, wherever the base pointer came from. The element
// type is read off the pointer rather than off the instance's type arguments, for the reason
// `elementType` in native.cpp is: it is the same answer and it needs no substitution to get it.
ModulePtr<Value> borrowElement(ExprResolver& resolver, ModulePtr<Value> base, ModulePtr<Value> index,
                               TypePtr type, LocationId source, StringId name, bool mut) {
    if(!base || !index) return nullptr;

    auto element = pointeeType(resolver.global, resolver.valueType(base));
    auto address = resolver.offsetPointer(base, element, index, source);

    return resolver.ref(resolver.emit<InstBorrow>(source, name, type, Place::atPointer(address), mut));
}

// `hostAt(items, index)` - the same element on a target with no addresses, which is a place there
// too and not an operation. See the element note in host.cpp.
ModulePtr<Value> borrowHostElement(ExprResolver& resolver, ModulePtr<Value> items, ModulePtr<Value> index,
                                   TypePtr type, LocationId source, StringId name, bool mut) {
    if(!items || !index) return nullptr;

    return resolver.ref(resolver.emit<InstBorrow>(source, name, type,
                                                  hostElementPlace(resolver, items, index), mut));
}

// `instance Index(%a, I64, a)` - `borrow(self + index)`. A pointer is its own base, so this is the
// case the three below are the field loads of. `getMut` borrows the *variable holding the address*,
// which is what a mutable subscript of a pointer has ever been able to mean.
template<Receiver mode>
ModulePtr<Value> emitPointerAt(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                               LocationId source, StringId name) {
    auto base = args[0];

    if(mode == Receiver::Mutable) {
        auto place = receiverPlace<mode>(resolver, args[0], source);
        if(!place) return nullptr;

        base = resolver.load(place.unwrap(), source);
    }

    return borrowElement(resolver, base, args[1], type, source, name, mode == Receiver::Mutable);
}

// `instance Index(Flat(a), Size, a)` natively - `borrow(self.items + index)`, with the descriptor's
// own count as the bound.
template<Receiver mode>
ModulePtr<Value> emitSliceAt(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                             LocationId source, StringId name) {
    // The receiver is reached *once*, and both fields are projected off that one place. Asking for it
    // twice is what a second `selfFieldValue` would do, and for `getMut` that is a second mutable
    // borrow of storage the first one holds exclusively - which the borrow checker reports, correctly,
    // as `xs[i] = v` conflicting with itself.
    auto self = receiverPlace<mode>(resolver, args[0], source);
    if(!self) return nullptr;

    if(resolver.checksEnabled()) {
        auto length = containerField(resolver, self.unwrap(), "length"_v, source);
        if(length) checkIndexInBounds(resolver, args[1], resolver.load(length.unwrap(), source), source);
    }

    auto items = containerField(resolver, self.unwrap(), "items"_v, source);
    if(!items) return nullptr;

    return borrowElement(resolver, resolver.load(items.unwrap(), source), args[1], type, source, name,
                         mode == Receiver::Mutable);
}

// `instance Index(Array(a), Size, a)` natively - `borrow(self.run.items + index)`. One address
// computation and not two loads and a temporary, which is what the owner's own instance buys over
// reaching the slice's through a conversion.
template<Receiver mode>
ModulePtr<Value> emitArrayAt(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                             LocationId source, StringId name) {
    // One receiver place, two projections off it - see emitSliceAt for why asking twice is wrong.
    auto self = receiverPlace<mode>(resolver, args[0], source);
    if(!self) return nullptr;

    // The owner's *live* count and not the run's capacity: a slot past the length is storage that
    // exists and holds nothing, which is exactly the read this check is for.
    if(resolver.checksEnabled()) {
        auto length = containerField(resolver, self.unwrap(), "length"_v, source);
        if(length) checkIndexInBounds(resolver, args[1], resolver.load(length.unwrap(), source), source);
    }

    auto run = containerField(resolver, self.unwrap(), "run"_v, source);
    if(!run) return nullptr;

    auto items = containerField(resolver, run.unwrap(), "items"_v, source);
    if(!items) return nullptr;

    return borrowElement(resolver, resolver.load(items.unwrap(), source), args[1], type, source, name,
                         mode == Receiver::Mutable);
}

// `instance Index(Flat(a), Size, a)` on JS - `hostAt(self.items, self.offset + index)`. The window's
// start is the one thing a host slice carries that a native one folds into its base.
template<Receiver mode>
ModulePtr<Value> emitHostSliceAt(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                 LocationId source, StringId name) {
    auto self = receiverPlace<mode>(resolver, args[0], source);
    if(!self || !args[1]) return nullptr;

    auto items = containerField(resolver, self.unwrap(), "items"_v, source);
    auto offset = containerField(resolver, self.unwrap(), "offset"_v, source);
    if(!items || !offset) return nullptr;

    auto start = resolver.load(offset.unwrap(), source);
    auto index = resolver.ref(resolver.emit<InstBinary>(source, StringId(), resolver.valueType(start),
                                                        Value::Add, start, args[1]));

    // Against the window's own length rather than the host array's: what a slice may read is
    // `offset` up to `offset + length`, and the array behind it is usually longer.
    if(resolver.checksEnabled()) {
        auto length = containerField(resolver, self.unwrap(), "length"_v, source);
        if(length) checkIndexInBounds(resolver, args[1], resolver.load(length.unwrap(), source), source);
    }

    return borrowHostElement(resolver, resolver.load(items.unwrap(), source), index, type, source,
                             name, mode == Receiver::Mutable);
}

/*
 * `instance Index(Array(a), Size, a)` on JS - `hostAt(self.items, index)`.
 *
 * The bound is the container's **stored count** and not the host array's own length, which is its
 * capacity - Implementation-Containers.md §14's typed row, where the two stopped being the same
 * number. Reading the host length here would have let an index into the slack a doubling left behind
 * through the check and hand back an `undefined` the program then treats as an element.
 */
template<Receiver mode>
ModulePtr<Value> emitHostArrayAt(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                 LocationId source, StringId name) {
    auto self = receiverPlace<mode>(resolver, args[0], source);
    if(!self) return nullptr;

    auto items = containerField(resolver, self.unwrap(), "items"_v, source);
    if(!items) return nullptr;

    if(resolver.checksEnabled()) {
        auto length = containerField(resolver, self.unwrap(), "length"_v, source);
        if(length) checkIndexInBounds(resolver, args[1], resolver.load(length.unwrap(), source), source);
    }

    return borrowHostElement(resolver, resolver.load(items.unwrap(), source), args[1], type, source,
                             name, mode == Receiver::Mutable);
}

// `instance Length(Flat(a))` - `self.length`, on both targets, and `instance Length(Array(a))`
// natively - `self.length :: Size`. One emitter, because the ascription is what `convert` does with
// a field that is already a `Size` as well: nothing.
ModulePtr<Value> emitStoredLength(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                  LocationId source, StringId) {
    auto length = selfFieldValue<Receiver::Read>(resolver, args[0], "length"_v, source);
    if(!length) return nullptr;

    // Explicitly, because a `Count` is narrower than a `Size` and the source says `::` for that
    // reason. An implicit conversion would report the precision it is deliberately not losing.
    return resolver.convert(length, type, source, false);
}

/*
 * A head written over one type variable - `Flat(a)`, `Array(a)`, `%a`.
 *
 * The same context `definePointerInstances` builds for `Eq(Ptr(a))`, and the same one a source
 * instance gets from prepareGenEnv: the variable is the instance's own, every generated function is
 * generic over it, and selecting the instance is matching its head rather than comparing it.
 */
GlobalPtr<GenEnv> headEnvironment(Module& module) {
    auto global = *module.types;
    auto env = new (module.types) GenEnv(GenEnv::Instance);
    auto pointer = env - global;

    auto variable = new (module.types) GenType(pointer, module.context.addQualifiedName("a", 1, 1), 0);
    env->types.push(module.types, variable - global);

    return pointer;
}

TypePtr headVariable(Module& module, GlobalPtr<GenEnv> env) {
    auto global = *module.types;
    return (Type*)global[global[env]->types.get(global, 0)] - global;
}

/*
 * `Index(c, k, v)` for one head, whose element is always the head's own variable - which is what the
 * class's functional dependency says out loud.
 *
 * The key is a parameter and not `Size` for the one head where the two differ: a container is
 * indexed by `Size`, which is `Int` on JS and `I64` natively, and a raw pointer is indexed by `I64`
 * on both - because a pointer is Native's and a `%a` on a target with no addresses is not something
 * a program indexes at all.
 */
void defineIndex(Module& module, TypePtr head, GlobalPtr<GenEnv> env, TypePtr key, TypePtr element,
                 Emit get, Emit getMut) {
    TypePtr args[] = { head, key, element };
    IntrinsicMethod methods[] = { { "get"_v, 2, get }, { "getMut"_v, 2, getMut } };

    generateInstance(module, classNamed(module, "Index"_v), { args, 3 }, { methods, 2 }, env);
}

void defineLength(Module& module, TypePtr head, GlobalPtr<GenEnv> env, Emit length) {
    IntrinsicMethod methods[] = { { "length"_v, 1, length } };
    generateInstance(module, classNamed(module, "Length"_v), { &head, 1 }, { methods, 1 }, env);
}

} // namespace

void defineNativeIndexInstances(Module& native) {
    auto js = isJsMode(native.context.settings.mode);

    auto pointerEnv = headEnvironment(native);
    auto pointee = headVariable(native, pointerEnv);
    defineIndex(native, resolvePointerType(native, pointee), pointerEnv, native.scalar.long_, pointee,
                emitPointerAt<Receiver::Loaned>, emitPointerAt<Receiver::Mutable>);

    auto sliceEnv = headEnvironment(native);
    auto element = headVariable(native, sliceEnv);
    auto slice = instantiateRecord(native, native.program.sliceType, { &element, 1 }, kNullLocation);

    // One instance and two bodies, where the source had two `@platform` declarations. What differs
    // is only how an element is reached, which is the whole of what §14 says a host container is.
    defineIndex(native, slice, sliceEnv, native.scalar.size, element,
                js ? emitHostSliceAt<Receiver::Loaned> : emitSliceAt<Receiver::Loaned>,
                js ? emitHostSliceAt<Receiver::Mutable> : emitSliceAt<Receiver::Mutable>);
}

void defineContainerInstances(Module& collections) {
    auto js = isJsMode(collections.context.settings.mode);

    auto arrayEnv = headEnvironment(collections);
    auto element = headVariable(collections, arrayEnv);
    auto array = instantiateRecord(collections, collections.program.arrayType, { &element, 1 }, kNullLocation);

    defineIndex(collections, array, arrayEnv, collections.scalar.size, element,
                js ? emitHostArrayAt<Receiver::Loaned> : emitArrayAt<Receiver::Loaned>,
                js ? emitHostArrayAt<Receiver::Mutable> : emitArrayAt<Receiver::Mutable>);

    // The owner's count and the slice's. Both are a stored field on both targets now - the JS owner
    // used to answer the host array's own length, and stopped when a capacity larger than the count
    // became possible (Implementation-Containers.md §14's typed row). The owner still needs an
    // instance of its own rather than reaching the slice's, since selection does not convert.
    auto sliceEnv = headEnvironment(collections);
    auto sliceElement = headVariable(collections, sliceEnv);

    defineLength(collections, instantiateRecord(collections, collections.program.sliceType,
                                                { &sliceElement, 1 }, kNullLocation),
                 sliceEnv, emitStoredLength);

    auto ownerEnv = headEnvironment(collections);
    auto ownerElement = headVariable(collections, ownerEnv);

    defineLength(collections, instantiateRecord(collections, collections.program.arrayType,
                                                { &ownerElement, 1 }, kNullLocation),
                 ownerEnv, emitStoredLength);
}
