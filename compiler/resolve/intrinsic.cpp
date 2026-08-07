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
        function->intrinsic = method.emit;
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

    registerInstance(module, instance - local);
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
    auto unsignedLength = resolver.convert(length, word, source, false);
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

// `instance Index(Array(a), Size, a)` on JS - `hostAt(self.items, index)`. The host array *is* the
// container here, so its own `length` is the bound and there is no stored count to read.
template<Receiver mode>
ModulePtr<Value> emitHostArrayAt(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                 LocationId source, StringId name) {
    auto items = selfFieldValue<mode>(resolver, args[0], "items"_v, source);

    if(resolver.checksEnabled() && items) {
        auto length = emitHostLengthOf(resolver, items, resolver.module.scalar.size, source, StringId());
        checkIndexInBounds(resolver, args[1], length, source);
    }

    return borrowHostElement(resolver, items, args[1], type, source, name, mode == Receiver::Mutable);
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

// `instance Length(Array(a))` on JS - `hostLength(self.items)`, which is the count because the host
// array is the container.
ModulePtr<Value> emitHostArrayLength(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                     LocationId source, StringId name) {
    auto items = selfFieldValue<Receiver::Read>(resolver, args[0], "items"_v, source);
    if(!items) return nullptr;

    return emitHostLengthOf(resolver, items, type, source, name);
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

    // The owner's count and the slice's, each kept where that target keeps it: a field natively on
    // both, and the host array's own `length` for the JS owner - which is why the owner needs an
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
                 ownerEnv, js ? emitHostArrayLength : emitStoredLength);
}
