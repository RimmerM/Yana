#include "native.h"
#include "core.h"
#include "intrinsic.h"
#include "name.h"
#include "../parse/parser.h"
#include "simd.h"

/*
 * Native's declarations.
 *
 * As in Core, everything that can be written in the language is written in the language. What the
 * compiler supplies is what the language has no way to say about itself, which here is every
 * operation whose meaning is the machine's rather than the program's.
 *
 * The fixed-width integer family used to be declared here, on the reasoning that a program which
 * cares how many bits a number has is a program close enough to the machine to have imported
 * Native anyway. That stopped being true once the JS target gained packing and scalarization:
 * `U8` and `I16` are how a record says it wants to be narrow, and on JS the payoff is a smaller
 * object rather than a smaller struct. Asking a pure web program to import the raw-pointer and
 * system-call module to reach them made the width types look unsafe, which they are not. They
 * are Core's now - see core.cpp's defineIntegerTypes.
 *
 * The pointer operations are *generic* intrinsics - declared here with a signature and no body,
 * and generated where they are called. A dereference is not one operation but one per element
 * type, so there is nothing to write down until a call says which; see intrinsic.h. That also
 * means none of them ever becomes a call in the IR: `*p` is a load, and `p + 1` is an add.
 *
 * Comparison is `instance Eq(Ptr(a))` and `instance Ord(Ptr(a))` - generated below rather than
 * written here, so that each method stays the one instruction it is instead of becoming a call.
 * A pointer therefore crosses into generic code that constrains Eq or Ord like any other type.
 *
 * Arithmetic is deliberately *not* `Num`. Three things say so independently: `class (FromInt(a))
 * Num(a)` would make `let p: %U8 = 4096` well-typed, `Num`'s operations are homogeneous while
 * `p + 1` and `difference(p, q)` are not, and `*` and `/` on an address mean nothing. The class
 * that fits pointer arithmetic is a heterogeneous one - roughly `class Offset(a, b)`, which would
 * also cover an iterator plus a count - and until a second type wants it the plain functions below
 * are correct and cost nothing.
 */
// Native's source is `lib/Native/`.

/*
 * The platform file - `lib/Native/Linux.x64.yana`.
 *
 * The system call numbers of one kernel and ABI, and nothing else. Everything it is written in terms
 * of comes from Native, and the one thing Native needs from it is `mapMemory` - which used to make
 * the two a pair of modules importing each other, with the edge from Native pushed in by hand
 * because an import statement there could only name a platform that had not been chosen yet. It is a
 * file of Native now, so the mutual visibility is simply what a module is.

/*
 * The pointer intrinsics.
 */

namespace {

// A pointer operation's element type: what the first argument points at. Every intrinsic below
// needs it, and taking it from the argument rather than from the substituted type arguments is
// what lets one emitter serve a signature whose result is a pointer and one whose result is not.
static TypePtr elementType(ExprResolver& resolver, Buffer<ModulePtr<Value>> args) {
    return pointeeType(resolver.global, resolver.valueType(args[0]));
}

static ModulePtr<Value> emitDeref(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr,
                                  LocationId source, StringId name) {
    return resolver.load(Place::atPointer(args[0]), source, name);
}

// An assignment rather than an initialization: what a raw pointer names is memory the program
// manages itself, so whatever was there is being overwritten. Nothing is dropped either way - a
// pointer root is outside the ownership graph entirely, which is what makes this the unsafe module.
static ModulePtr<Value> emitStore(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr,
                                  LocationId source, StringId) {
    resolver.assign(Place::atPointer(args[0]), args[1], source);
    return nullptr;
}

static ModulePtr<Value> emitAddressOf(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr,
                                      LocationId source, StringId name) {
    return resolver.addressOf(resolver.materialize(args[0], source), source, name);
}

/*
 * The whole-vector transfers - `vectorAt`, `vectorPast` and `setVectorAt`.
 *
 * Each is one address computation and one access, and the address computation is the same one in all
 * three: the element pointer the caller handed over, named at the vector's type rather than at the
 * element's. That is a `Bitcast` and nothing else, which is what `pointerAsVector` is - a pointer's
 * *value* does not depend on what it is said to point at, so there is no arithmetic here and no
 * alignment claim either. A vector load is unaligned on both of this compiler's backends by
 * construction (§5.6 records the one place a legacy packed encoding is not, and why every operand
 * that reaches one is a frame slot).
 */
static ModulePtr<Value> pointerAsVector(ExprResolver& resolver, ModulePtr<Value> pointer, TypePtr vector,
                                        LocationId source) {
    if(!isVectorType(resolver.global, vector)) {
        resolver.context.diagnostics.error("a vector transfer needs a vector type, and %@ is not one"_v,
                                           source, describeType(resolver.context, resolver.global, vector));
        return nullptr;
    }

    auto address = resolvePointerType(resolver.module, vector);
    return resolver.ref(resolver.emit<InstUnary>(source, StringId(), address, Value::Bitcast, pointer));
}

/*
 * `bits` - the movemask, and the one reduction kind that is a machine's rather than the language's.
 *
 * `ReduceOp::Bits` and nothing else; the width comes from the mask the caller handed over and the
 * result is the `Int` the signature states, for the reason `firstSet`'s is one - sixteen or
 * thirty-two bits of answer are not a value the lane type holds.
 */
static ModulePtr<Value> emitMaskBits(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                     LocationId source, StringId name) {
    auto mask = resolver.valueType(args[0]);
    if(!vectorLanes(resolver.global, mask)) return nullptr;

    return resolver.ref(resolver.emit<InstVecReduce>(source, name, type, args[0], ReduceOp::Bits));
}

static ModulePtr<Value> emitVectorAt(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                     LocationId source, StringId name) {
    auto address = pointerAsVector(resolver, args[0], type, source);
    if(!address) return nullptr;

    return resolver.load(Place::atPointer(address), source, name);
}

/*
 * The overreading load - Implementation-Vector.md §3.3 and §9.7, and the only thing in the language
 * that sets that flag.
 *
 * `vectorAt` with the flag on, and it is the *same place*: rooted in the pointer, because that is
 * what a slice's storage is rooted in on this target and no arrangement of borrows changes it -
 * §3.3 asks the verifier to refuse exactly that root, and §9.7 records why the rule could not be
 * held and what stands in for it. What makes an overread true is the type one layer up:
 * `Collections.loadVectorTail` takes a `Flat(a)`, which is a window into storage the language
 * allocated, and nothing else in the tree calls this.
 */
static ModulePtr<Value> emitVectorPast(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                       LocationId source, StringId name) {
    auto address = pointerAsVector(resolver, args[0], type, source);
    if(!address) return nullptr;

    auto load = resolver.emit<InstLoadPlace>(source, name, type, Place::atPointer(address));
    load->overread = true;

    return resolver.ref(load);
}

// The store, which never overreads - a write past the end of an object is not a read of unspecified
// bytes but the destruction of whatever follows. An assignment rather than an initialization for the
// reason `store` is one: a pointer root is outside the ownership graph, so nothing is dropped.
static ModulePtr<Value> emitSetVectorAt(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr,
                                        LocationId source, StringId) {
    auto address = pointerAsVector(resolver, args[0], resolver.valueType(args[1]), source);
    if(!address) return nullptr;

    resolver.assign(Place::atPointer(address), args[1], source);
    return nullptr;
}

/*
 * `null` and `isNull` had emitters here, and they are gone.
 *
 * Both are ordinary library bodies now - `bitcast(0 :: Size)` and a comparison against zero, over
 * the reinterpretation rungs `definePointerInstances` already generates. The reason is not economy:
 * **address zero is a perfectly ordinary address** on bare hardware and inside a kernel, so "the
 * null pointer" is a convention `lib/Native/Pointer.yana` holds and not a fact the core language
 * knows. A compiler that built the constant itself was a compiler with an opinion about the
 * target's address space, stated in a place no reader of the library would find it.
 *
 * What made them intrinsic was cost, and cost is answered where cost is answered: `foldTrivialBody`
 * folds a body that is one expression with no call left in it at the site it was called from, with
 * no optimizer having run. That is a rule about a *shape* rather than about these two names, so a
 * program's own one-line accessors get it too.
 */

// `borrow(p)` and `borrowMut(p)`. The place is the memory the pointer names, so the borrow is
// rooted where the pointer was rooted and everything downstream of it - the return-root check, the
// caller's loan - follows from that one fact.
template<bool mut>
static ModulePtr<Value> emitBorrowAt(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                     LocationId source, StringId name) {
    return resolver.ref(resolver.emit<InstBorrow>(source, name, type, Place::atPointer(args[0]), mut));
}

/*
 * `stringData(&s)` - the two words of a native string, as the record that describes them.
 *
 * A **retype and not a read**. The argument is a borrow of a `String` and the result is a borrow of
 * `Native.StringData`, and `computeString` is what makes those the same bytes: a string's Repr *is*
 * that record's, so the address is unchanged and there is nothing to emit but the change of type.
 * That is the same instruction `bitcast(p) :: %b` is, for the same reason, and it is why this is an
 * intrinsic at all rather than a library function - there is no way to write "these two types occupy
 * one place" in the language, and no reason for a program to be able to.
 *
 * The result is a borrow, which is what keeps ownership out of it. Handing back a `StringData` by
 * value would make a second owner of one run and the frame would release it twice; a borrow is a
 * view, and the `return self` in the declaration is what roots it in the string so the checker gives
 * it the string's extent. Everything after that is the ordinary borrow rule and nothing about
 * strings.
 */
template<bool mut>
static ModulePtr<Value> emitStringData(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                       LocationId source, StringId name) {
    return resolver.ref(resolver.emit<InstUnary>(source, name, type, Value::Cast, args[0]));
}

/*
 * `stringFromData(d)` - the other direction, which is the one that makes a string out of nothing.
 *
 * **Storage and a copy, not a cast**, and the difference is not about strings at all. An intrinsic
 * is expanded at the call site and hands back whatever value it built, where an ordinary call to a
 * function returning an aggregate is given a local by the caller to write into - so an intrinsic
 * whose result is a *memory type* has nowhere for that result to live, and a bare `Cast` of one
 * produces a value every later use asks for and nothing ever lowered. That was the first version,
 * and the symptom was exactly that: "resolve value was used before it was lowered".
 *
 * So this does what the call it stands in for would have done. The storage is a local of the
 * result's type, and the argument is initialized into it - which for two types of identical Repr is
 * a sixteen-byte copy the optimizer removes wherever the source was a temporary built for this call,
 * which is every call site there is.
 *
 * The bytes are not reinterpreted so much as re-owned: whoever built the `StringData` owned the run,
 * and after this the string does. That is why the declaration takes its argument by `->` - the
 * handover is real, and writing it as a borrow would leave two owners of one run.
 */
static ModulePtr<Value> emitStringFromData(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                           LocationId source, StringId name) {
    auto storage = resolver.allocate(type, source, name);
    if(!storage) return nullptr;

    resolver.initialize(resolver.placeFor(storage, source), args[0], source);
    return storage;
}

/*
 * `traits() :: TypeTraits(T)` - what this target made of one type, as a value.
 *
 * **The type comes from the position rather than from an argument**, which is the whole of why this
 * replaced `sizeOf(it: a)` and `alignOf(it: a)`. Their parameter existed only so that `a` had
 * somewhere to come from, so the size of a type nobody happened to hold a value of could not be
 * asked for at all. Here `a` is the result's own type argument, exactly as `null() :: %U8` binds a
 * pointee; a value in hand is served by `traitsOf` in the library, which is one line over this and
 * needs nothing from the compiler.
 *
 * **One function and one record rather than five nullary intrinsics.** The five questions have one
 * answer between them - what this target made of this type - and asking them separately is what let
 * `stride` have no spelling at all, while `Run.yana` computed it as a pointer difference and left a
 * comment apologizing for doing so. A record also makes the answer a *value*: bindable once at the
 * top of a function, passable, storable, where five calls can only ever be five calls.
 *
 * It costs nothing to build. Every component is an `InstTypeMetric`, which folds to an immediate at
 * a concrete type and to a descriptor read at a variable, and the record dissolves into its fields
 * wherever it is used - so `traitsOf(x).size` is the same single immediate `sizeOf(x)` was.
 *
 * A question this deliberately does not answer is the byte order, which `Target.byteOrder` does.
 * Every field here is a fact about `a`; the byte order is a fact about the machine, and putting it
 * in a per-type record would be one answer filed under `n` different types.
 */
static ModulePtr<Value> emitTraits(ExprResolver& resolver, Buffer<ModulePtr<Value>>, TypePtr type,
                                   LocationId source, StringId name) {
    auto& global = resolver.global;

    /*
     * The type being described - the result's own type argument.
     *
     * Reported rather than asserted at every step: what makes this a `TypeTraits` is a declaration
     * in the library, and a library declaring something else under the name is a broken
     * *installation* rather than a broken program. The field count is checked for the same reason
     * and is the one that would otherwise go wrong silently - a sixth field added in source with no
     * metric behind it would be built out of whatever the fifth left behind.
     */
    auto record = global[type]->kind == Type::Record ? (RecordType*)global[type] : nullptr;
    if(!record || record->instanceArgs.size() != 1 || record->constructors.isEmpty()) {
        resolver.context.diagnostics.error("internal: traits() answers something that is not a TypeTraits"_v,
                                           source);
        return nullptr;
    }

    auto of = record->instanceArgs.get(global, 0);
    auto content = record->constructors.get(global, 0).content;
    if(!content || global[content]->kind != Type::Tup) {
        resolver.context.diagnostics.error("internal: TypeTraits has no fields"_v, source);
        return nullptr;
    }

    // In declaration order, which is the only thing this and the source agree by - see the field
    // list in lib/Native/Pointer.yana.
    static constexpr TypeMetricKind kMetrics[] = {
        TypeMetricKind::Size, TypeMetricKind::Align, TypeMetricKind::Stride,
        TypeMetricKind::TrivialCopy, TypeMetricKind::HasTeardown,
    };

    static constexpr Size kMetricCount = sizeof(kMetrics) / sizeof(kMetrics[0]);

    auto& fields = *(TupType*)global[content];
    if(fields.fields.size() != kMetricCount) {
        resolver.context.diagnostics.error("internal: TypeTraits declares %@ fields and the compiler fills %@"_v,
                                           source, Size(fields.fields.size()), Size(kMetricCount));
        return nullptr;
    }

    SmallArray<ModulePtr<Value>, kMetricCount> values;
    for(Size i = 0; i < kMetricCount; i++) {
        auto field = fields.fields.get(global, i).type;
        values.push(resolver.ref(resolver.emit<InstTypeMetric>(source, StringId(), field, of,
                                                               kMetrics[i])));
    }

    /*
     * Written as one instruction where the layout allows it, and field by field where it does not.
     *
     * The fallback is not a rarity here: the two flags are `Bool`s, and a narrow field is a
     * co-packing candidate that `buildFieldAggregate` declines by design - so this record takes the
     * second path on every target that packs. Both are what an ordinary `TypeTraits {...}` written
     * in source would compile to, which is the property worth having: this intrinsic builds the
     * value the language would have built, rather than a second construction path of its own.
     */
    auto storage = resolver.allocate(type, source, name);

    // The payload's place, which is what both writers below take: a record's fields hang off its
    // one constructor. Stepped here rather than left to the aggregate's `constructor` field, so
    // that this is the same pair of calls `fillTuple` makes for a construction written in source -
    // one writer reached two ways is what keeps the value this builds the value the language would
    // have built.
    auto payload = resolver.project(resolver.placeFor(storage, source), ProjectionKind::Downcast, 0);

    if(!resolver.buildFieldAggregate(payload, fields, toBuffer(values), source)) {
        for(Size i = 0; i < values.size(); i++) {
            resolver.initialize(resolver.project(payload, ProjectionKind::Field, U16(i)), values[i],
                                source);
        }
    }

    return storage;
}

/*
 * `p + n` and `p - n`, in elements.
 *
 * The scale used to be folded here, and skipped entirely for a one-byte element - which is what made
 * `%U8` the type byte arithmetic is written in without paying for a multiply. Both halves of that
 * moved rather than disappeared: the scale is a TypeMetric, and the multiply by the one it folds to
 * is removed where every other constant-folding decision is made, in the translation to the lower IR.
 * The resolver no longer claims to know how wide a `U8` is on the target being built for.
 */
template<Value::Kind kind>
static ModulePtr<Value> emitPointerOffset(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                          LocationId source, StringId name) {
    auto offsetType = resolver.valueType(args[1]);
    auto scale = resolver.ref(resolver.emit<InstTypeMetric>(source, StringId(), offsetType,
                                                            elementType(resolver, args),
                                                            TypeMetricKind::Stride));
    auto offset = resolver.ref(resolver.emit<InstBinary>(source, StringId(), offsetType, Value::Mul,
                                                         args[1], scale));

    return resolver.ref(resolver.emit<InstBinary>(source, name, type, kind, args[0], offset));
}

/*
 * The top half of a widening multiply - Implementation-Map.md §3.1's one new primitive.
 *
 * The map's hash is wyhash's and xxh3's core, `lo(a*b) ^ hi(a*b)`, and the low half is `*`. This is
 * the other half: one `InstBinary` at the operand type, which lower_inst.cpp turns into the
 * `MulHi`/`IMulHi` the reciprocal in lower_strength.cpp has always emitted. Nothing synthesizes it
 * out of thirty-two-bit pieces, which is why the declaration is `@platform(native)` - a target
 * without the instruction takes the JS answer, an int32 mixer of its own.
 */
static ModulePtr<Value> emitMulHigh(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                    LocationId source, StringId name) {
    return resolver.ref(resolver.emit<InstBinary>(source, name, type, Value::MulHi, args[0], args[1]));
}

static ModulePtr<Value> emitDifference(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                       LocationId source, StringId name) {
    auto bytes = resolver.ref(resolver.emit<InstBinary>(source, StringId(), type, Value::Sub, args[1], args[0]));
    auto scale = resolver.ref(resolver.emit<InstTypeMetric>(source, StringId(), type,
                                                            elementType(resolver, args),
                                                            TypeMetricKind::Stride));

    return resolver.ref(resolver.emit<InstBinary>(source, name, type, Value::Div, bytes, scale));
}

/*
 * `newRun(n)` - Implementation-Containers.md §2.
 *
 * An intrinsic rather than a function, because what it expands to is an allocation with a count and
 * the language has no spelling for one. It is the *only* thing here that is compiler magic: every
 * other operation on a run - the empty one, the capacity, the address of the slots, growth, and the
 * placement switch its `Reclaim` is - is written in the language above, over this and over the
 * allocator.
 *
 * The result type is what says which element it is a run of. Nothing in the argument list does, and
 * nothing needs to: `newRun()` at a `Run(Buffer)` is the same call at a `Run(U8)` with a different
 * stride, which is exactly the shape a generic intrinsic has.
 */
static ModulePtr<Value> emitNewRun(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                   LocationId source, StringId) {
    ModulePtr<Value> items = nullptr;
    auto run = resolver.buildRun(type, args[0], source, items);

    if(!run) {
        resolver.context.diagnostics.error("internal: newRun's result is not a run of slots"_v, source);
    }

    return run;
}

/*
 * `cloneThread(flags, stackTop, entry, argument, threadId)`, whose third argument is a **function**
 * and reaches the backend as the two words one is.
 *
 * A Yana function value is a code pointer and an environment - see FunValueLayout - and the thread
 * needs both: the code is what it calls and the environment is what that code's first parameter is.
 * Splitting it here rather than in the declaration is what lets the entry be an ordinary function
 * of the language, closure included, instead of a raw pointer the source would have had to produce.
 *
 * The two loads are ordinary reads of the value's own storage and happen in the *parent*, before
 * anything is cloned. What crosses to the child is two words on its stack.
 */
static ModulePtr<Value> emitCloneThread(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                        LocationId source, StringId name) {
    auto entry = resolver.materialize(args[2], source);
    auto code = resolver.load(resolver.project(entry, ProjectionKind::Field, FunValueLayout::kCode), source);
    auto env = resolver.load(resolver.project(entry, ProjectionKind::Field, FunValueLayout::kEnv), source);

    auto instruction = resolver.create<InstNative>(source, name, type, NativeOp::CloneThread);
    auto& arena = resolver.module.arena;

    instruction->args.push(arena, args[0]);   // flags
    instruction->args.push(arena, args[1]);   // the top of the child's stack
    instruction->args.push(arena, code);
    instruction->args.push(arena, args[3]);   // the argument
    instruction->args.push(arena, args[4]);   // where the kernel clears the thread id
    instruction->args.push(arena, env);

    resolver.append(instruction);
    return resolver.ref(instruction);
}

template<NativeOp op>
static ModulePtr<Value> emitNativeOp(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                     LocationId source, StringId name) {
    auto instruction = resolver.create<InstNative>(source, name, type, op);
    for(auto arg: args) instruction->args.push(resolver.module.arena, arg);

    resolver.append(instruction);
    return isUnit(resolver.global, type) ? nullptr : resolver.ref(instruction);
}

} // namespace

/*
 * Assembling the modules.
 */

/*
 * Eq and Ord over every pointer type at once.
 *
 * The head is `Ptr(a)`, written here as a generic context holding one variable and the pointer
 * type over it. Nothing about comparing two addresses depends on what they point at, so the
 * instance has no requirement of its own to prove and is the simplest possible parametric head -
 * which is what makes it the natural first client of one.
 *
 * Every method is an intrinsic, so selecting the instance expands to the same `cmp` a concrete
 * instance would; `compare` is the one method with a real body, and it is specialized per pointee
 * type the way any generic function is.
 */
static void definePointerInstances(Module& module) {
    auto global = *module.types;
    auto env = new (module.types) GenEnv(GenEnv::Instance);
    auto envPointer = env - global;

    auto name = module.context.addQualifiedName("a", 1, 1);
    auto variable = new (module.types) GenType(envPointer, name, 0);
    env->types.push(module.types, variable - global);

    auto pointer = resolvePointerType(module, (Type*)variable - global);

    defineEq(module, pointer, envPointer);
    defineOrd(module, pointer, envPointer);

    /*
     * And the reinterpretation rungs, which is what `cast`, `asInt` and `asPtr` became.
     *
     * Pointer-to-pointer needs a context of its own holding *two* variables: the two sides are
     * unrelated, which is the whole content of the operation, so the one-variable context above
     * cannot express its head. The other two reuse that context, since one side is a concrete
     * address-width integer.
     *
     * `Size` and not `I64`, which is the change Move 2 made possible and the reason a rung at an
     * address is sound at all: the pair holds the same value because both sides *are* the target's
     * word, rather than because both happen to be sixty-four bits on the one machine this module
     * compiles for today. The general ladder declines every abstract width for the same reason - see
     * `reinterpretWidth` - so this is where the one honest address rung lives.
     */
    auto pairEnv = new (module.types) GenEnv(GenEnv::Instance);
    auto pairPointer = pairEnv - global;

    auto fromVariable = new (module.types) GenType(pairPointer, module.context.addQualifiedName("a", 1, 1), 0);
    auto toVariable = new (module.types) GenType(pairPointer, module.context.addQualifiedName("b", 1, 1), 1);
    pairEnv->types.push(module.types, fromVariable - global);
    pairEnv->types.push(module.types, toVariable - global);

    defineBitcast(module, resolvePointerType(module, (Type*)fromVariable - global),
                  resolvePointerType(module, (Type*)toVariable - global), pairPointer);

    auto address = module.program.scalar.size;
    defineBitcast(module, pointer, address, envPointer);
    defineBitcast(module, address, pointer, envPointer);

    /*
     * And the same rung unsigned, on the argument above applied unchanged: `USize` *is* the target's
     * word too, so the pair holds the same value for the same reason.
     *
     * It is not a convenience. An address has no sign - nothing subtracts one pointer from a smaller
     * one and expects a negative address - so a layer that wants to say "machine word, unsigned"
     * about one had to launder it through the signed rung and back, which is two casts stating
     * something neither of them meant. The system-call layer is where that showed: every argument a
     * kernel takes is a word, and the file saying so could not name an address as one.
     */
    auto unsignedAddress = module.program.scalar.unsignedSize;
    defineBitcast(module, pointer, unsignedAddress, envPointer);
    defineBitcast(module, unsignedAddress, pointer, envPointer);
}

/*
 * The emitters, offered for every role Native's declarations may claim.
 *
 * **No condition on any of them, and the conditions that are gone are the point.** Every branch this
 * function used to carry asked the same question - is this declaration part of this build - about a
 * name it was going to look up: the vector transfers on a JS target, the seven system calls, the
 * thread clone off linux-x64, a block copy spelled `blockCopy` on one target and `copyMemory` on the
 * other. The question was asked here because `attachIntrinsic` matched a string against one module's
 * function table and reported a miss, correctly, as an internal error.
 *
 * A role answers it before this function runs. `@platform` and the file selectors decide which
 * declarations exist, `readIntrinsicRole` records which of them claimed what, and a role nothing
 * claimed is a role nothing can call - so an emitter offered for it lands on nothing and costs a
 * null test. The two block-copy declarations are one role claimed from opposite sides of a platform
 * filter, which is the whole of what the `isJsMode` ternary was choosing between.
 *
 * What is left is a list, and a list is what this should have been: one line per operation, in the
 * order `op.def` states them, with no fact about any target in it.
 */
static void attachPointerIntrinsics(Module& module) {
    using intrinsic::Op;

    // The raw pointer - reads, writes, the address of a value, and the two borrows that re-enter
    // the checked world from the unchecked one.
    attachIntrinsic(module, Op::pointerLoad, emitDeref);
    attachIntrinsic(module, Op::pointerStore, emitStore);
    attachIntrinsic(module, Op::addressOf, emitAddressOf);
    attachIntrinsic(module, Op::pointerBorrow, emitBorrowAt<false>);
    attachIntrinsic(module, Op::pointerBorrowMut, emitBorrowAt<true>);

    // Arithmetic in elements, and the measurements the scale comes from. `traits` is
    // `@platform(native)` for a reason that is not "no machine to ask" but "the wrong thing to ask":
    // how many bytes a value occupies on a managed target is the runtime's answer rather than this
    // compiler's - see the declaration, and ManagedTypeDesc for the descriptor cells that
    // consequently do not exist.
    attachIntrinsic(module, Op::pointerAdd, emitPointerOffset<Value::Add>);
    attachIntrinsic(module, Op::pointerSub, emitPointerOffset<Value::Sub>);
    attachIntrinsic(module, Op::pointerDifference, emitDifference);
    attachIntrinsic(module, Op::typeTraits, emitTraits);

    // The transfers only an instruction set has. `bits` is the one reduction kind above the lower IR
    // that only a machine has; Host's `hostVector` family is the managed target's answer to all
    // four, and is attached where the rest of Host is.
    attachIntrinsic(module, Op::vectorAt, emitVectorAt);
    attachIntrinsic(module, Op::vectorPast, emitVectorPast);
    attachIntrinsic(module, Op::setVectorAt, emitSetVectorAt);
    attachIntrinsic(module, Op::maskBits, emitMaskBits);
    attachIntrinsic(module, Op::mulHigh, emitMulHigh);

    // The allocation primitive every container is built on - Implementation-Containers.md §2.
    attachIntrinsic(module, Op::newRun, emitNewRun);

    /*
     * Block memory, and the two ends the one instruction is reached from.
     *
     * On a native build `copyMemory` is a *body* - the vector ladder in `Memory.native.yana`, which
     * calls this only for the lengths that pay a `rep movsb`'s startup back - so what the intrinsic
     * is is the thing the ladder bottoms out in, and the declaration claiming the role is named
     * `blockCopy`. On JS there is no ladder and no bytes to move, and the declaration that claims it
     * is `copyMemory` itself. One role, two `@platform`-exclusive claimants, one line here.
     */
    attachIntrinsic(module, Op::blockCopy, emitNativeOp<NativeOp::CopyMemory>);
    attachIntrinsic(module, Op::setMemory, emitNativeOp<NativeOp::SetMemory>);

    // The system call at each arity a call needs, because a call's argument count is what decides
    // which registers the kernel reads and there is no variadic form to write.
    attachIntrinsic(module, Op::syscall0, emitNativeOp<NativeOp::Syscall>);
    attachIntrinsic(module, Op::syscall1, emitNativeOp<NativeOp::Syscall>);
    attachIntrinsic(module, Op::syscall2, emitNativeOp<NativeOp::Syscall>);
    attachIntrinsic(module, Op::syscall3, emitNativeOp<NativeOp::Syscall>);
    attachIntrinsic(module, Op::syscall4, emitNativeOp<NativeOp::Syscall>);
    attachIntrinsic(module, Op::syscall5, emitNativeOp<NativeOp::Syscall>);
    attachIntrinsic(module, Op::syscall6, emitNativeOp<NativeOp::Syscall>);

    // And the one operation that is a system call and could not be written as one - see
    // NativeOp::CloneThread. A hardcoded sequence rather than an instruction: one kernel's `clone`
    // with one architecture's registers, which its declaration says in the only place a reader will
    // look for it, `Native/Clone.linux.x64.yana`. A mac or arm64 build reads no such file, claims
    // the role with nothing, and this lands on nothing.
    attachIntrinsic(module, Op::cloneThread, emitCloneThread);
}

/*
 * Native's records and what a native `String` is made of - the middle hook, beside Core's.
 *
 * Types rather than functions, which is what puts them here: a signature that writes `[T]` in a
 * binding position becomes a `Flat(T)`, so `passFunctionSignatures` needs the slice before it runs.
 */
void definePreludeNativeTypes(Program& program, Module& module) {
    auto& context = program.context;
    auto native = &module;

    // See Program::runType and Program::sliceType: the resolver produces both without a name to look
    // them up through, so they are recorded rather than searched for at each use.
    auto named = [&](const char* text, Size length) -> GlobalPtr<RecordType> {
        auto found = native->namedTypes.get(context.addQualifiedName(text, length, 1));
        if(!found) return nullptr;

        return (RecordType*)(*program.types)[found.unwrap()] - *program.types;
    };

    program.runType = named("Run", 3);
    program.sliceType = named("Flat", 4);

    /*
     * What a native `String` occupies - see Type::String and `computeString`.
     *
     * The wrapper rather than `Array(U8)` itself, and the two are the same bytes: a single-field
     * record is its field. What the wrapper buys is that a borrow of it is a *borrow* - see the
     * declaration, and `resolveType`'s Borrow case, which makes a borrow of a container a slice.
     *
     * On JS the declaration is `@platform`-excluded, so this answers null and the string stays what
     * it is there: one host value with nothing to lay out.
     */
    auto stringData = named("StringData", 10);
    auto content = stringData ? (Type*)(*program.types)[(TypePtr)stringData] - *program.types : nullptr;
    ((StringType*)(*program.types)[program.scalar.string_])->content = content;
    program.scalar.stringContent = content;
}

void definePreludeNative(Program& program, Module& module) {
    auto& context = program.context;
    auto native = &module;

    // The instances before any body that uses one, and after Core's classes, since the classes they
    // join are its.
    definePointerInstances(*native);
    attachPointerIntrinsics(*native);

    // Recorded so that storage-class selection and drop insertion can emit calls to them without
    // going through name resolution in whichever module happened to need one - see Program.
    auto findNative = [&](const char* text, Size length) -> ModulePtr<Function> {
        auto found = native->functions.get(context.addUnqualifiedName(text, length));
        return found ? found.unwrap() : nullptr;
    };

    program.allocateHeap = findNative("allocateHeap", 12);
    program.freeHeap = findNative("freeHeap", 8);
    program.releaseRun = findNative("releaseRun", 10);

    // After `sliceType`, which is what the head names, and before any body of this module is
    // resolved - which happens once every module's declarations have been read.
    defineNativeIndexInstances(*native);

    // The processor's own instructions - `lib/Native/Intrinsic/X86.yana`, whose declarations are
    // this module's because reaching one is meant to cost an `import Native`.
    defineCpuIntrinsics(*native);
}

/*
 * What a native string is made of - `lib/Native/Text.native.yana`.
 *
 * Four declarations. A native `String` is a run of bytes and a count; a run is Native's, and the two
 * words *are* an `Array(U8)`, whose declaration has to be implicitly visible because `[a]` is
 * grammar. So the reinterpretation names a type of Core and hands out a type of Native, and while
 * the import graph had to be acyclic that made it a module of its own sitting between them -
 * Implementation-Simplification.md §17. Analysis-Modules.md §2.4 is what dissolved the middle: Core
 * and Native are one cycle, and this is a file of the unsafe one.
 *
 * Which is the property that matters and is unchanged: it is **not** implicitly imported, so
 * `stringFromData` - a `String` forged out of bytes with no UTF-8 validation anywhere - is not
 * reachable by writing nothing.
 */

void definePreludeNativeText(Program& program, Module& native) {
    auto& context = program.context;
    auto module = &native;

    using intrinsic::Op;

    /*
     * The three reinterpretations, which are the only compiler-supplied String operations:
     * everything else about a native string is written in Yana over the record they hand back.
     *
     * All three declarations are `@platform(native)`, so a JS build reads none of them and claims
     * none of the roles - which is why this needs no condition of its own any more. It used to test
     * `program.scalar.stringContent`, on the grounds that a name `attachIntrinsic` could not find
     * was an internal error and rightly so; a role nothing claimed is not an error, so the test is
     * gone and what is left says what it does.
     */
    attachIntrinsic(*module, Op::stringData, emitStringData<false>);
    attachIntrinsic(*module, Op::stringDataMut, emitStringData<true>);
    attachIntrinsic(*module, Op::stringFromData, emitStringFromData);

    /*
     * And the constructor a string literal is built through, which is still a name.
     *
     * Recorded for the reason `allocateHeap` is: a literal is emitted by the resolver, which has a
     * global's address and a length and no name resolution to reach a constructor through. It is not
     * an `op.def` role, because the compiler does not *implement* it - `stringLiteral` has a body,
     * and what the compiler needs is a handle on it. That is `known.def`'s direction, and the row
     * arrives with the rest of `String` at milestone 8; until the ownership stage can compile the
     * declaration, a row for it would be a role permanently unfilled.
     */
    auto literal = module->functions.get(context.addUnqualifiedName("stringLiteral", 13));
    program.stringLiteral = literal ? literal.unwrap() : nullptr;
}
