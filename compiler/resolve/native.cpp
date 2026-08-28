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

// The lower IR has no pointer immediates on purpose, so a null pointer is the integer zero
// reinterpreted - which is what `bitcast(0) :: %a` says anyway.
static ModulePtr<Value> emitNull(ExprResolver& resolver, Buffer<ModulePtr<Value>>, TypePtr type,
                                 LocationId source, StringId name) {
    auto zero = resolver.makeInt(source, resolver.module.scalar.long_, 0);
    return resolver.ref(resolver.emit<InstUnary>(source, name, type, Value::Bitcast, zero));
}

// And the test for one goes the other way: the address as a number, against zero.
static ModulePtr<Value> emitIsNull(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                   LocationId source, StringId name) {
    auto address = resolver.module.scalar.long_;
    auto number = resolver.ref(resolver.emit<InstUnary>(source, StringId(), address, Value::Bitcast, args[0]));
    auto zero = resolver.makeInt(source, address, 0);

    return resolver.ref(resolver.emit<InstCmp>(source, name, type, number, zero, CompareOp::Eq));
}

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
 * `sizeOf(x)` and `alignOf(x)`.
 *
 * A question rather than an answer, now that layout is a target's business: the instruction carries
 * the type and whoever emits folds it. That costs nothing on the concrete path - it is one immediate
 * either way - and it is what makes these work inside a generic body at all, where there is no
 * number to fold and the width comes out of the caller's TypeDesc instead.
 */
static ModulePtr<Value> emitSizeOf(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                   LocationId source, StringId name) {
    return resolver.ref(resolver.emit<InstTypeMetric>(source, name, type, resolver.valueType(args[0]),
                                                      TypeMetricKind::Size));
}

static ModulePtr<Value> emitAlignOf(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                    LocationId source, StringId name) {
    return resolver.ref(resolver.emit<InstTypeMetric>(source, name, type, resolver.valueType(args[0]),
                                                      TypeMetricKind::Align));
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

static void attachPointerIntrinsics(Module& module) {
    attachIntrinsic(module, "*"_v, emitDeref);
    attachIntrinsic(module, "store"_v, emitStore);
    attachIntrinsic(module, "addressOf"_v, emitAddressOf);
    attachIntrinsic(module, "null"_v, emitNull);
    attachIntrinsic(module, "isNull"_v, emitIsNull);
    attachIntrinsic(module, "borrow"_v, emitBorrowAt<false>);
    attachIntrinsic(module, "borrowMut"_v, emitBorrowAt<true>);
    // The three vector transfers are `@platform(native)`, so on a JS build they were never declared
    // and there is nothing to attach - which `attachIntrinsic` reports as an internal error rather
    // than skipping, and rightly. Host's `hostVector` family is that target's answer.
    if(!isJsMode(module.context.settings)) {
        attachIntrinsic(module, "vectorAt"_v, emitVectorAt);
        attachIntrinsic(module, "vectorPast"_v, emitVectorPast);
        attachIntrinsic(module, "setVectorAt"_v, emitSetVectorAt);

        // `bits` is the one kind above the lower IR that only a machine has. The two pinned-width
        // declarations that used to sit here are gone: a probe writes `vectorAt(p) :: Vec(U8, 16)`
        // now that a count is a const parameter with a default, so there is nothing left to pin.
        attachIntrinsic(module, "bits"_v, emitMaskBits);

        // And the two measurements, `@platform(native)` for a different reason: not "no machine to
        // ask" but "the wrong thing to ask". How many bytes a value occupies on a managed target is
        // the runtime's answer rather than this compiler's - see the declarations in
        // `Native/Pointer.yana`, and ManagedTypeDesc for the descriptor cell that consequently does
        // not exist.
        attachIntrinsic(module, "sizeOf"_v, emitSizeOf);
        attachIntrinsic(module, "alignOf"_v, emitAlignOf);
    }

    attachIntrinsic(module, "+"_v, emitPointerOffset<Value::Add>);
    attachIntrinsic(module, "-"_v, emitPointerOffset<Value::Sub>);
    attachIntrinsic(module, "difference"_v, emitDifference);

    if(!isJsMode(module.context.settings)) attachIntrinsic(module, "mulHigh"_v, emitMulHigh);

    attachIntrinsic(module, "newRun"_v, emitNewRun);

    /*
     * The block copy, under the name the target it is reachable by has for it.
     *
     * One instruction and two declarations, because the two targets reach it from opposite ends. On
     * a native build `copyMemory` is a *body* - the vector ladder in `Native/Memory.native.yana`, which calls this
     * only for the lengths that pay a `rep movsb`'s startup back - so the intrinsic is the thing the
     * ladder bottoms out in and is named `blockCopy`. On a JS build there is no ladder and no bytes
     * to move: what the operation is for there is the shape `blockCopyShape` recovers from the
     * compiler-generated relocation glue, so it keeps the name it has always had.
     *
     * Attaching by mode rather than attaching both, because `attachIntrinsic` reports a name it
     * cannot find as an internal error and rightly - each of the two declarations is `@platform`ed
     * to one target and does not exist on the other.
     */
    attachIntrinsic(module, isJsMode(module.context.settings) ? "copyMemory"_v : "blockCopy"_v,
                    emitNativeOp<NativeOp::CopyMemory>);

    attachIntrinsic(module, "setMemory"_v, emitNativeOp<NativeOp::SetMemory>);

    /*
     * The system call, at each arity a call needs - and only where there is a kernel to call.
     *
     * By mode for the reason the block copy above is: the declarations are `Native/Memory.native.yana`'s
     * and a JS build never read that file, so attaching here would be `attachIntrinsic` reporting
     * seven names it cannot find. That the file is selected by its *name* now rather than by seven
     * `@platform` attributes changes nothing about this - what a target has is still what decides.
     */
    if(!isJsMode(module.context.settings)) {
        static const StringView syscalls[] = {
            "syscall0"_v, "syscall1"_v, "syscall2"_v, "syscall3"_v,
            "syscall4"_v, "syscall5"_v, "syscall6"_v,
        };

        for(auto& name: syscalls) attachIntrinsic(module, name, emitNativeOp<NativeOp::Syscall>);

        /*
         * And the one operation that is a system call and could not be written as one - see
         * NativeOp::CloneThread.
         *
         * Selected more narrowly than the seven above, and not on the same question at all. A
         * syscall is an instruction every kernel-having target has; this is a *hardcoded sequence*
         * - one kernel's `clone` with one architecture's registers, an x64 pseudo here and an
         * x86-64 inline-asm block under LLVM - so what it needs is not "a kernel" but this kernel
         * on this machine. Its declaration says the same thing in the only place a reader will look
         * for it, which is the file name: `Native/Clone.linux.x64.yana`.
         *
         * The two have to agree, because a name the file did not declare is what `attachIntrinsic`
         * reports as an internal error - correctly. A mac or arm64 build reads no such file, and
         * this is the condition under which it does.
         */
        auto& settings = module.context.settings;
        if(settings.target == TargetType::Linux && settings.arch == TargetArch::X64) {
            attachIntrinsic(module, "cloneThread"_v, emitCloneThread);
        }
    }
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

    auto stringData = program.scalar.stringContent;

    /*
     * The two reinterpretations, which are the only compiler-supplied String operations: everything
     * else about a native string is written in Yana over the record they hand back.
     *
     * Attached only where they were declared, exactly as `Host` attaches its own. All are
     * `@platform(native)`, so a JS build read none of the declarations and there is nothing to hook -
     * which `attachIntrinsic` reports as an internal error rather than skipping, and rightly, since
     * a missing declaration is normally a typo.
     */
    if(stringData) {
        attachIntrinsic(*module, "stringData"_v, emitStringData<false>);
        attachIntrinsic(*module, "stringDataMut"_v, emitStringData<true>);
        attachIntrinsic(*module, "stringFromData"_v, emitStringFromData);

        // Recorded for the same reason `allocateHeap` is: a string literal is emitted by the
        // resolver, which has a global's address and a length and no name resolution to reach a
        // constructor through. See Program::stringLiteral.
        auto literal = module->functions.get(context.addUnqualifiedName("stringLiteral", 13));
        program.stringLiteral = literal ? literal.unwrap() : nullptr;
    }
}
