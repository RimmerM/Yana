#include "simd.h"
#include "builder.h"
#include "intrinsic.h"
#include "name.h"
#include "edit.h"

/*
 * The portable set - Design-Vector §3.3.
 *
 * Each of these is one instruction and none of them is ever a call: `expandIntrinsic` runs them at
 * the call site, so `splat(x)` is a `vsplat` in the IR with nothing around it. That is the same
 * arrangement `Num(Int).+` has and it is here for the same reason - reaching one must cost nothing
 * *without an optimizer having run*.
 */

/*
 * An integer argument this resolver can read as a constant.
 *
 * A `ConstInt`, or a negation of one - and the second half is not pedantry. `-1` is not a literal
 * here: the parser produces `1` and a unary minus, `Num.-` expands to a `Neg`, and nothing folds it
 * until the optimizer runs, which is long after an intrinsic has had to decide what it expands to.
 * So a lane pattern written with a negative distance would be refused for not being constant.
 */
static bool constantInteger(ExprResolver& resolver, ModulePtr<Value> argument, I64& result) {
    auto value = resolver.local[argument];

    if(value->kind == Value::Neg) {
        auto inner = resolver.local[((InstUnary*)value)->from];
        if(inner->kind != Value::ConstInt) return false;

        result = -((ConstInt*)inner)->value;
        return true;
    }

    if(value->kind != Value::ConstInt) return false;

    result = ((ConstInt*)value)->value;
    return true;
}

// The lane index a `lane` or a `withLane` names, which has to be a constant this resolver can read.
// Reported here rather than downstream because this is the only place that can name the argument:
// below it, a lane index is a field of an instruction and there is nothing to point at.
static bool constantLane(ExprResolver& resolver, ModulePtr<Value> argument, TypePtr vector,
                         LocationId source, U16& lane) {
    auto value = resolver.local[argument];

    if(value->kind != Value::ConstInt) {
        resolver.context.diagnostics.error("a lane index must be a constant - a vector lane is named by the instruction that reads it, and a computed index is a shuffle rather than a lane read"_v,
                                           source);
        return false;
    }

    auto index = ((ConstInt&)*value).value;
    auto lanes = vectorLanes(resolver.global, vector);

    if(index < 0 || U64(index) >= lanes) {
        resolver.context.diagnostics.error("lane %@ is past the end of a vector of %@ lanes"_v,
                                           source, I32(index), lanes);
        return false;
    }

    lane = U16(index);
    return true;
}

static ModulePtr<Value> emitSplat(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                  LocationId source, StringId name) {
    auto lane = vectorLane(resolver.global, type);
    auto value = lane ? resolver.convert(args[0], lane, source) : args[0];
    if(!value) return nullptr;

    return resolver.ref(resolver.emit<InstVecSplat>(source, name, type, value));
}

static ModulePtr<Value> emitLane(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                 LocationId source, StringId name) {
    U16 lane = 0;
    auto vector = resolver.valueType(args[0]);
    if(!constantLane(resolver, args[1], vector, source, lane)) return nullptr;

    return resolver.ref(resolver.emit<InstVecLane>(source, name, type, args[0], lane));
}

static ModulePtr<Value> emitWithLane(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                     LocationId source, StringId name) {
    U16 lane = 0;
    if(!constantLane(resolver, args[1], type, source, lane)) return nullptr;

    auto element = vectorLane(resolver.global, type);
    auto value = element ? resolver.convert(args[2], element, source) : args[2];
    if(!value) return nullptr;

    return resolver.ref(resolver.emit<InstVecLane>(source, name, type, args[0], lane, value));
}

template<ReduceOp reduce>
static ModulePtr<Value> emitReduce(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                   LocationId source, StringId name) {
    return resolver.ref(resolver.emit<InstVecReduce>(source, name, type, args[0], reduce));
}

/*
 * The rest of the portable set - §9 item 4.
 *
 * Everything below is built out of the five vector kinds and the ordinary arithmetic, which is
 * Implementation-Vector.md §3.2's claim tested: a portable vector library of thirty names needs no
 * instruction the IR did not already have. Where an expansion is more than one instruction that is
 * said at the expansion, along with what the machine would have done in one.
 */

// A lane-typed constant, which is the one thing every expansion below needs and the one place the
// float/integer split has to be made by hand: `makeInt` at a float type is a value neither backend
// can hold.
static ModulePtr<Value> laneConstant(ExprResolver& resolver, TypePtr lane, LocationId source, I64 value) {
    return isFloat(resolver.global, lane) ? resolver.makeFloat(source, lane, F64(value))
                                          : resolver.makeInt(source, lane, value);
}

static ModulePtr<Value> splatConstant(ExprResolver& resolver, TypePtr vector, LocationId source, I64 value) {
    auto lane = vectorLane(resolver.global, vector);
    if(!lane) return nullptr;

    return resolver.ref(resolver.emit<InstVecSplat>(source, StringId(), vector,
                                                    laneConstant(resolver, lane, source, value)));
}

static ModulePtr<Value> shuffleBy(ExprResolver& resolver, TypePtr type, ModulePtr<Value> left,
                                  ModulePtr<Value> right, Buffer<U8> pattern, LocationId source, StringId name) {
    auto shuffle = resolver.emit<InstVecShuffle>(source, name, type, left, right);
    for(auto entry: pattern) shuffle->pattern.push(entry);

    return resolver.ref(shuffle);
}

/*
 * `zero` and `iota`, which take no argument and are selected by what the result is asked to be.
 *
 * `iota` is a chain of lane writes over a zero rather than a constant, because there is no vector
 * constant in this IR and deliberately so - a shuffle pattern does not fit in a general register and
 * a lane pattern is not an immediate on any of these machines, so a vector constant is a `.rodata`
 * entry, which is the backend's business rather than the IR's. What the chain costs is nothing that
 * survives: every entry is a constant, so the folder answers the whole of it and both backends see
 * the finished vector. It is written as `lanes` instructions and read as one.
 */
static ModulePtr<Value> emitZero(ExprResolver& resolver, Buffer<ModulePtr<Value>>, TypePtr type,
                                 LocationId source, StringId name) {
    auto lane = vectorLane(resolver.global, type);
    if(!lane) return nullptr;

    return resolver.ref(resolver.emit<InstVecSplat>(source, name, type,
                                                    laneConstant(resolver, lane, source, 0)));
}

static ModulePtr<Value> emitIota(ExprResolver& resolver, Buffer<ModulePtr<Value>>, TypePtr type,
                                 LocationId source, StringId name) {
    auto lane = vectorLane(resolver.global, type);
    auto lanes = vectorLanes(resolver.global, type);
    if(!lane || !lanes) return nullptr;

    auto value = splatConstant(resolver, type, source, 0);

    for(U32 i = 1; i < lanes; i++) {
        auto last = i + 1 == lanes;
        value = resolver.ref(resolver.emit<InstVecLane>(source, last ? name : StringId(), type, value,
                                                        U16(i), laneConstant(resolver, lane, source, i)));
    }

    return value;
}

// The lane count, which the *type* already carries - so this is a constant and the argument it was
// read off is dead. Design-Vector §3.3 calls it a compile-time constant and this is what makes that
// true rather than nearly true: there is no call and no vector operation in the result.
static ModulePtr<Value> emitLanes(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                  LocationId source, StringId) {
    return resolver.makeInt(source, type, I64(vectorLanes(resolver.global, resolver.valueType(args[0]))));
}

/*
 * `min` and `max`, which are a comparison and a select.
 *
 * Two instructions where `minps` is one, and the shape rather than an instruction because that is
 * what a target *without* a packed minimum needs and what a target with one folds back: the x64
 * backend recognizes exactly this pair (`selectPackedMinMax`) and LLVM's own selection does the
 * same. NaN follows the comparison: `min(NaN, b)` answers `b`, which is what `minps` does with its
 * operands in this order - so the fold is an identity rather than an approximation, and **the
 * operand order here is what makes that true**.
 */
template<CompareOp op>
static ModulePtr<Value> emitMinMax(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                   LocationId source, StringId name) {
    auto mask = maskFor(resolver.module, type);
    if(!mask) return nullptr;

    auto cmp = resolver.ref(resolver.emit<InstCmp>(source, StringId(), mask, args[0], args[1], op));
    return resolver.ref(resolver.emit<InstSelect>(source, name, type, cmp, args[0], args[1]));
}

/*
 * `abs`, which is one instruction and used to be three.
 *
 * It was `select(v .<= 0, 0 - v, v)` - the comparison non-strict and the negation a subtraction from
 * zero, so that `-0.0` answers `+0.0` where `-v` would have answered `-0.0`, and a NaN compares
 * false and passes through the untouched arm. That last part is what made it wrong to leave as a
 * shape: every target computes a magnitude in one instruction and every one of them clears the sign
 * of a NaN, so a backend that used its own instruction disagreed with one that expanded this, and
 * nothing in the language said which was right. `Value::Abs` is where that is now ruled on - see the
 * row in inst.def.
 *
 * An unsigned lane is already its own magnitude, so the answer is the operand and no instruction is
 * emitted at all - which is also why the verifier refuses one over an unsigned lane rather than
 * treating it as a no-op.
 */
static ModulePtr<Value> emitAbs(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                LocationId source, StringId name) {
    auto lane = vectorLane(resolver.global, type);
    auto mask = maskFor(resolver.module, type);
    if(!lane || !mask) return nullptr;

    auto integer = resolver.global[lane];
    if(integer->kind == Type::Int && !((IntType*)integer)->isSigned) return args[0];

    return resolver.ref(resolver.emit<InstUnary>(source, name, type, Value::Abs, args[0]));
}

/*
 * The rearrangements, all of which are one `VecShuffle` and differ only in the pattern.
 *
 * A pattern entry names a lane of the two sources concatenated, so a shuffle within one vector names
 * that vector twice - which is what keeps the numbering independent of how many sources were meant.
 * Every pattern here is built from the lane count, so none of them can be written wrong at a call
 * site and none of them is checked at one.
 */
using LanePattern = SmallArray<U8, 16>;

/*
 * The lane pattern a `shuffle` was written with - Design-Swizzle §13.1.
 *
 * Read back out of the literal that carried it, which is the one thing in this file that walks the
 * IR rather than building it, and is worth saying why. A pattern is not a *value*: it is a field of
 * the instruction on every target that has one, and there is no signature that can carry it except
 * as an argument. So it arrives as a `[Int *m]` - `arrayShapeFor` is what makes the literal fill a
 * parameter whose count is a variable - which resolves to a frame slot and one `InstAggregate`
 * writing the entries into it, and this reads the entries and erases both.
 *
 * Erasing them is not tidiness. Every intrinsic here is one instruction "because reaching one must
 * cost nothing *without an optimizer having run*", and a shuffle that left a stack slot and `m`
 * stores behind would be the only one that did not keep that promise.
 *
 * The aggregate is found by the slot it writes rather than by position, because the only structural
 * fact this may rely on is `resolveFixedArray`'s: the literal allocates and then fills, in the block
 * that is current when the argument is resolved. A pattern argument written any other way - a
 * binding, a global, a slice - has no aggregate to find and is refused by name.
 */
static bool constantPattern(ExprResolver& resolver, ModulePtr<Value> argument, U32 sources, U32 lanes,
                            U32 results, LocationId source, LanePattern& pattern) {
    auto& local = resolver.local;
    auto reject = [&]() {
        resolver.context.diagnostics.error("a lane pattern must be written as a literal at the call - it is the instruction's own field on every target that has one, and a computed pattern is a runtime shuffle, which is a platform module's business"_v,
                                           source);
        return false;
    };

    auto allocation = local[argument];
    if(!allocation || allocation->kind != Value::Alloc || !resolver.current) return reject();

    auto slot = ((InstAlloc*)allocation)->local;
    auto block = local[resolver.current];
    ModulePtr<InstAggregate> written = nullptr;

    for(Size i = block->instructionCount(); i-- > 0;) {
        auto instruction = block->instructionAt(local, i);
        if((ModulePtr<Value>)instruction == argument) break;
        if(local[instruction]->kind != Value::Aggregate) continue;

        auto& aggregate = (InstAggregate&)*local[instruction];
        if(aggregate.place.root != PlaceRoot::Local || aggregate.place.local != slot) continue;

        written = (ModulePtr<InstAggregate>)instruction;
        break;
    }

    if(!written) return reject();

    auto& components = local[written]->components;
    if(components.size() != results) return reject();

    /*
     * The entries in the order they were pushed, which is element order - see buildAggregate, whose
     * components are one per element with the index as the step.
     *
     * `constantInteger` and not a bare `ConstInt` test, for the reason it exists: `-1` is a negation
     * of a literal rather than a literal, and nothing folds it until long after this has had to
     * decide. Read as one, a negative entry gets the range message it deserves instead of being
     * reported as a pattern nobody wrote out.
     */
    for(Size i = 0; i < components.size(); i++) {
        I64 index = 0;
        if(!constantInteger(resolver, components.get(local, i).value, index)) return reject();

        if(index < 0 || U64(index) >= U64(lanes) * sources) {
            resolver.context.diagnostics.error("lane %@ is past the end of what this shuffle reads - %@"_v,
                                               source, I32(index),
                                               sources == 1
                                                   ? "a one-vector shuffle names the lanes of its own source"_v
                                                   : "a two-vector shuffle names the lanes of the two placed end to end"_v);
            return false;
        }

        pattern.push(U8(index));
    }

    IrEditor editor(resolver.module, resolver.function);
    editor.eraseInstruction((ModulePtr<Inst>)written);
    editor.eraseInstruction((ModulePtr<Inst>)argument);

    return true;
}

/*
 * `shuffle` and `shuffle2` - the general rearrangement the named patterns below are cases of.
 *
 * The result's lane count is the pattern's length and comes out of the *type*, which the solve
 * already bound from the literal - so this reads it rather than counting anything. What decides
 * whether the count may differ from the source's is nothing here: `InstVecShuffle` states one entry
 * per result lane and `verifyFunction` checks the entries against the source's, which is the form
 * `unpackLow` already emits.
 *
 * A one-vector shuffle names its source twice, which is `shuffleBy`'s convention for every pattern
 * in this file and is what keeps the numbering independent of how many sources were meant.
 */
template<U32 sources>
static ModulePtr<Value> emitShuffle(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                    LocationId source, StringId name) {
    auto lanes = vectorLanes(resolver.global, resolver.valueType(args[0]));
    auto results = vectorLanes(resolver.global, type);
    if(!lanes || !results) return nullptr;

    LanePattern pattern;
    if(!constantPattern(resolver, args[sources], sources, lanes, results, source, pattern)) return nullptr;

    return shuffleBy(resolver, type, args[0], args[sources - 1], toBuffer(pattern), source, name);
}

static ModulePtr<Value> emitReverse(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                    LocationId source, StringId name) {
    auto lanes = vectorLanes(resolver.global, type);
    if(!lanes) return nullptr;

    LanePattern pattern;
    for(U32 i = 0; i < lanes; i++) pattern.push(U8(lanes - 1 - i));

    return shuffleBy(resolver, type, args[0], args[0], toBuffer(pattern), source, name);
}

/*
 * A rotation *left* by a constant: lane `i` of the result is lane `i + by` of the source, so
 * `rotate(v, 1)` moves every lane one place toward lane zero and wraps.
 *
 * The distance is a constant for the reason a lane index is: the pattern is a field of the
 * instruction on every target this compiles to, and a computed one is `pshufb` on x86 and nothing at
 * all elsewhere. A negative distance rotates the other way, which the modulus handles rather than a
 * second name.
 */
static ModulePtr<Value> emitRotate(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                   LocationId source, StringId name) {
    auto lanes = vectorLanes(resolver.global, type);
    if(!lanes) return nullptr;

    I64 distance = 0;

    if(!constantInteger(resolver, args[1], distance)) {
        resolver.context.diagnostics.error("a rotation distance must be a constant - a vector's lanes are rearranged by a pattern the instruction carries, and a computed one is not an operation every target has"_v,
                                           source);
        return nullptr;
    }

    auto by = distance % I64(lanes);
    if(by < 0) by += I64(lanes);

    LanePattern pattern;
    for(U32 i = 0; i < lanes; i++) pattern.push(U8((I64(i) + by) % I64(lanes)));

    return shuffleBy(resolver, type, args[0], args[0], toBuffer(pattern), source, name);
}

/*
 * A window out of two vectors placed end to end - `palignr` on x86, `vext` on ARM.
 *
 * `below` supplies lanes `0` through `n - 1` of the concatenation and `above` supplies the rest,
 * which is the operand order the x86 instruction has and is stated in the declaration for that
 * reason. The pattern is therefore `from + i` read against a source numbering where `below` is the
 * *second* operand - so the two arguments are handed to `shuffleBy` the way the machine names them
 * and the pattern is what puts them the right way round.
 *
 * The distance is a constant on `rotate`'s terms, and unlike a rotation it does not wrap: a window
 * starting past the end of the pair names lanes that are not there, so it is refused where it is
 * written rather than taken modulo `2n`.
 */
static ModulePtr<Value> emitAlignLanes(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                       LocationId source, StringId name) {
    auto lanes = vectorLanes(resolver.global, type);
    if(!lanes) return nullptr;

    I64 from = 0;

    if(!constantInteger(resolver, args[2], from)) {
        resolver.context.diagnostics.error("the start of an aligned window must be a constant - it is the instruction's own immediate, and a computed one is not an operation any target has"_v,
                                           source);
        return nullptr;
    }

    if(from < 0 || from > I64(lanes)) {
        resolver.context.diagnostics.error("a window of %@ lanes starting at %@ runs past the end of the pair - the start is between 0 and %@"_v,
                                           source, lanes, I32(from), lanes);
        return nullptr;
    }

    // `above` is operand zero and `below` is operand one, so the concatenation's lane `k` is pattern
    // entry `k + lanes` for `k` below the count and `k - lanes` above it.
    LanePattern pattern;
    for(U32 i = 0; i < lanes; i++) {
        auto k = I64(i) + from;
        pattern.push(U8(k < I64(lanes) ? k + I64(lanes) : k - I64(lanes)));
    }

    return shuffleBy(resolver, type, args[0], args[1], toBuffer(pattern), source, name);
}

// The two halves of a merge: the low halves of both sources interleaved, and the high halves. `high`
// is the whole of the difference between them, which is where each source's half starts.
template<bool high>
static ModulePtr<Value> emitInterleave(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                       LocationId source, StringId name) {
    auto lanes = vectorLanes(resolver.global, type);
    if(!lanes) return nullptr;

    auto start = high ? lanes / 2 : 0;

    LanePattern pattern;
    for(U32 i = 0; i < lanes; i++) {
        auto half = start + i / 2;
        pattern.push(U8(i % 2 ? lanes + half : half));
    }

    return shuffleBy(resolver, type, args[0], args[1], toBuffer(pattern), source, name);
}

/*
 * The conversions that change the lane *count* - Design-Vector §3.3's last three.
 *
 * A conversion that keeps the count is `Widen`, `Narrow` or `Bitcast` and is not a function at all
 * (§3.4). These are the other kind: `unpackLow` reads half a vector's lanes at twice the lane width,
 * and `packLanes` reads two vectors' lanes at half of it. They are functions rather than instances
 * because a class relates one type to one type and these relate one to two or two to one.
 *
 * Each is a `VecShuffle` and a `Cast`, which is exactly what `verifyFunction` says a lane-count
 * change is when it refuses one to a `Cast` alone. The shuffle is what moves the lanes into the half
 * that survives; the cast is what changes their width, and it keeps the count because by then the
 * count is already right.
 *
 * The result type comes from the *call*, since nothing in the argument decides it - `unpackLow(v) ::
 * Vec(I32)` is how one is written, and the check that the two shapes fit is here because this is the
 * only place that can name both.
 */
static bool halfWidthShapes(ExprResolver& resolver, TypePtr wide, TypePtr narrow, LocationId source,
                            U32& wideLanes, U32& narrowLanes) {
    auto global = resolver.global;
    wideLanes = vectorLanes(global, wide);
    narrowLanes = vectorLanes(global, narrow);

    auto widths = targetIntWidths(resolver.context.settings);
    auto wideStride = laneStride(global, vectorLane(global, wide), widths);
    auto narrowStride = laneStride(global, vectorLane(global, narrow), widths);

    if(wideLanes && narrowLanes == wideLanes * 2 && wideStride == narrowStride * 2) return true;

    resolver.context.diagnostics.error("these two vectors are not half and whole of one shape - a lane-count conversion relates %@ lanes of one width to twice as many of half of it"_v,
                                       source, wideLanes);
    return false;
}

// `high` picks which half of the source survives, and is the whole of the difference between the two.
template<bool high>
static ModulePtr<Value> emitUnpack(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                   LocationId source, StringId name) {
    auto narrow = resolver.valueType(args[0]);
    U32 wideLanes = 0;
    U32 narrowLanes = 0;
    if(!halfWidthShapes(resolver, type, narrow, source, wideLanes, narrowLanes)) return nullptr;

    // The half, still at the source's lane type - a shuffle changes which lanes there are and never
    // how wide one is. The cast below is what does the second half of the job.
    auto half = resolveVectorType(resolver.module, vectorLane(resolver.global, narrow), wideLanes,
                                  false, source);
    if(!isVectorType(resolver.global, half)) return nullptr;

    LanePattern pattern;
    for(U32 i = 0; i < wideLanes; i++) pattern.push(U8(high ? wideLanes + i : i));

    auto selected = shuffleBy(resolver, half, args[0], args[0], toBuffer(pattern), source, StringId());
    return resolver.ref(resolver.emit<InstUnary>(source, name, type, Value::Cast, selected));
}

// Two vectors into one of half-width lanes: each is narrowed where it stands, and the shuffle is what
// puts the second one after the first.
static ModulePtr<Value> emitPackLanes(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                      LocationId source, StringId name) {
    auto wide = resolver.valueType(args[0]);
    U32 wideLanes = 0;
    U32 narrowLanes = 0;
    if(!halfWidthShapes(resolver, wide, type, source, wideLanes, narrowLanes)) return nullptr;

    auto half = resolveVectorType(resolver.module, vectorLane(resolver.global, type), wideLanes,
                                  false, source);
    if(!isVectorType(resolver.global, half)) return nullptr;

    auto left = resolver.ref(resolver.emit<InstUnary>(source, StringId(), half, Value::Cast, args[0]));
    auto right = resolver.ref(resolver.emit<InstUnary>(source, StringId(), half, Value::Cast, args[1]));

    LanePattern pattern;
    for(U32 i = 0; i < narrowLanes; i++) pattern.push(U8(i));

    return shuffleBy(resolver, type, left, right, toBuffer(pattern), source, name);
}

/*
 * Reading a mask - Design-Vector §3.2.
 *
 * `any`, `all` and `count` are one reduction each, which is what `ReduceOp` having `And` and `Or`
 * beside the arithmetic is for. The result type is what tells `count` from the other two: a `Bool`
 * asks for the truth of the whole mask, an `Int` asks how many lanes hold it, and the lower IR reads
 * the same distinction off the scalar form of the source.
 */
template<ReduceOp reduce>
static ModulePtr<Value> emitMaskReduce(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                       LocationId source, StringId name) {
    return resolver.ref(resolver.emit<InstVecReduce>(source, name, type, args[0], reduce));
}

// `none` is `any` negated, at a `Bool` - so the one bit operation, not an integer complement.
static ModulePtr<Value> emitNone(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                 LocationId source, StringId name) {
    auto any = resolver.ref(resolver.emit<InstVecReduce>(source, StringId(), type, args[0], ReduceOp::Or));
    auto one = resolver.makeInt(source, type, 1);

    return resolver.ref(resolver.emit<InstBinary>(source, name, type, Value::Xor, any, one));
}

/*
 * `firstSet` - the lowest set lane, or the lane count when nothing is set.
 *
 * The operation a search loop terminates on, and the reason the whole mask family is portable rather
 * than a platform module's: "which lane matched" is `movemask` and a bit scan on x86 and something
 * else on every other machine, and none of those is an instruction this IR *had*.
 *
 * It is one now - `ReduceOp::FirstSet`, which each backend lowers for itself. What it used to be is
 * three instructions this IR already had: `select(mask, iota(), splat(lanes))` followed by an
 * unsigned minimum reduction, where a set lane contributes its own index and a clear one a number
 * larger than every index, so the smallest is the first set lane and the all-clear case falls out
 * with no branch. That is exact, it is portable, and on x64 it is about forty instructions - a
 * reduction tree of shuffles, a blend per level and a narrow lane extract - against the two that a
 * `pmovmskb` and a bit scan are. §34 item 2 of test/bench/findings.md is the measurement, and the
 * ruling it records is this one: a kind of its own that each backend lowers, rather than a pattern
 * match on the chain above in the one backend that can see through it.
 *
 * The result is the `Int` the signature promises rather than the lane's scalar form, because a lane
 * index is not a lane: a `Mask(I8)` of thirty-two lanes answers up to 32, which is not a value an
 * `I8` holds. That is the one thing about this kind every layer below has to state separately - see
 * the validator's rule, which is `Bits`'s.
 */
static ModulePtr<Value> emitFirstSet(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                     LocationId source, StringId name) {
    auto mask = resolver.valueType(args[0]);
    if(!vectorLanes(resolver.global, mask)) return nullptr;

    return resolver.ref(resolver.emit<InstVecReduce>(source, name, type, args[0], ReduceOp::FirstSet));
}

/*
 * `maskUpTo(n)` - the first `n` lanes set, which is the tail mask.
 *
 * `iota() .< splat(n)` at the mask's lane width, and the whole reason it is an intrinsic is that
 * last clause: `splat` converts its argument to the lane type through the ordinary conversion, and
 * an `Int` does not reach a lane narrower than one. So `Vec(I16)`'s tail mask has no spelling in
 * source, and every wider one has a spelling only because the conversion happens to widen. Here the
 * count is a lane index - bounded by the lane count, so by 64 - and what the cast means is settled.
 *
 * ## §52 The comparison is built in the integer domain, whatever the mask is over
 *
 * Both operands are lane *numbers*: the indices `0, 1, 2, ...` and how many lanes are live. Neither
 * is a value of the vector's element type in any sense other than that the comparison used to be
 * written there, and both are bounded by `kMaxVectorLanes` - so the two domains answer the same
 * mask, bit for bit, and the integer one is where the machine is cheaper. Over a `Mask(Float)` the
 * float domain costs, on x64:
 *
 *     vxorps ; vcvtsi2ss ; vpbroadcastd ; vcmpltps      the count converted, then compared as floats
 *
 * against the integer domain's `vmovd ; vpbroadcastd ; vpcmpgtd`, which folds `iota`'s own constant
 * into the comparison's memory operand and needs no dependency-breaking `vxorps` in front of a
 * conversion that is not there. It is also what puts a float tail on the same footing as an integer
 * one everywhere below: the lane-range fold in the x64 backend (§41.3) reads an *integer* comparison
 * against `iota` and had no reason to refuse a float one other than that it could not recognize it.
 *
 * Signed rather than unsigned, because `pcmpgt` is signed and the unsigned relation is a bias and
 * two exclusive-ors on top of it - and nothing here is ever negative. A count outside `0..lanes`
 * answers all-clear or all-set rather than something in between, exactly as the float comparison
 * did, which is what a caller that clamps its own remainder already relies on.
 *
 * An integer lane is already in that domain and keeps its own type, signedness and all: an unsigned
 * lane compares its indices unsigned, which is the same answer for numbers this small, and moving it
 * to the signed twin would be a second type in the IR for nothing.
 *
 * The mask that comes back is a mask over the *index* vector, and the one the caller asked for is
 * over the element - the same bits under a different name, which is what `bitcast` is the language's
 * spelling for (see `resolveVectorType` on why the two are no longer one interned type). It costs
 * nothing at either backend: a mask's lane *kind* stops existing one tier down, where `maskType`
 * normalizes it away and the fold removes what is then an identity.
 */

// The type a lane *number* is counted in for a vector of this element: the element itself where it
// is already an integer, and the signed integer of the same width where it is a float.
static TypePtr laneIndexType(ExprResolver& resolver, TypePtr lane) {
    if(!isFloat(resolver.global, lane)) return lane;

    // On JS a mask is `lanes` host booleans and a lane comparison is a scalar `<` in either domain,
    // so there is nothing here to win - and a 64-bit integer lane is refused outright on that target
    // (a `bigint` is not a lane), so `Mask(Double)` would be a diagnostic rather than a saving.
    if(isJsMode(resolver.context.settings.mode)) return lane;

    // A float is four bytes or eight, so the two rungs below are the whole of the mapping. A missing
    // one is Core not having been built yet, which answers the lane itself and changes nothing.
    auto stride = laneStride(resolver.global, lane, targetIntWidths(resolver.context.settings));
    auto index = resolver.module.scalar.signedLanes[stride == 8 ? 3 : 2];

    return index ? index : lane;
}

static ModulePtr<Value> emitMaskUpTo(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                     LocationId source, StringId name) {
    auto lane = vectorLane(resolver.global, type);
    auto lanes = vectorLanes(resolver.global, type);
    if(!lane || !lanes) return nullptr;

    auto index = laneIndexType(resolver, lane);
    auto indices = resolveVectorType(resolver.module, index, lanes, false, source);
    if(!isVectorType(resolver.global, indices)) return nullptr;

    auto mask = index == lane ? type : maskFor(resolver.module, indices);
    if(!mask) return nullptr;

    auto iota = emitIota(resolver, { nullptr, 0 }, indices, source, StringId());
    if(!iota) return nullptr;

    // The count at the index's own width. A `Cast` rather than a conversion, for the reason above:
    // this is the one place that knows the value is a lane index and so cannot lose anything.
    auto limit = resolver.ref(resolver.emit<InstUnary>(source, StringId(), index, Value::Cast, args[0]));
    auto splat = resolver.ref(resolver.emit<InstVecSplat>(source, StringId(), indices, limit));

    auto compare = resolver.ref(resolver.emit<InstCmp>(source, mask == type ? name : StringId(), mask,
                                                       iota, splat, CompareOp::Lt));

    if(mask == type) return compare;
    return resolver.ref(resolver.emit<InstUnary>(source, name, type, Value::Bitcast, compare));
}

/*
 * `sqrt` and `fma`, which are the two in the portable set that needed an instruction rather than an
 * arrangement of the ones that were already there - Implementation-Vector.md §9.4.
 *
 * One emitter each for the scalar and the vector, because there is one *declaration* each: `a` in
 * the signature binds a `Double` or a `Vec(Float)` and the instruction is the same either way. What
 * is checked here is the only thing the signature could not say - that whatever it bound is a float
 * or a vector of them - and it is checked here rather than downstream because this is the last place
 * that can name the argument. Below it, the type is a lane kind and the declaration is gone.
 */
static bool requireFloating(ExprResolver& resolver, TypePtr type, LocationId source, StringView what) {
    auto lane = vectorLane(resolver.global, type);
    auto element = lane ? lane : type;

    if(element && resolver.global[element]->kind == Type::Float) return true;

    resolver.context.diagnostics.error("%@ is defined on floating-point values and vectors of them, and this is neither"_v,
                                       source, what);
    return false;
}

static ModulePtr<Value> emitSqrt(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                 LocationId source, StringId name) {
    if(!requireFloating(resolver, type, source, "sqrt"_v)) return nullptr;
    return resolver.ref(resolver.emit<InstUnary>(source, name, type, Value::Sqrt, args[0]));
}

/*
 * The four roundings to an integral value, which are `emitSqrt` at four kinds.
 *
 * One template rather than four functions because nothing separates them here: each is a float or a
 * vector of them, each answers its operand's type, and which of the four an instruction is is the
 * kind itself. What the four *mean* is ruled on in resolve/inst.def and differs only at a tie.
 *
 * The name is passed through `requireFloating` so that `ceil(1)` names `ceil` in its diagnostic
 * rather than naming whichever of the four the template was instantiated for first.
 */
template<Value::Kind kind>
static ModulePtr<Value> emitRounding(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                     LocationId source, StringId name) {
    auto what = kind == Value::Trunc ? "trunc"_v : kind == Value::Floor ? "floor"_v
              : kind == Value::Ceil  ? "ceil"_v  : "round"_v;

    if(!requireFloating(resolver, type, source, what)) return nullptr;
    return resolver.ref(resolver.emit<InstUnary>(source, name, type, kind, args[0]));
}

static ModulePtr<Value> emitFma(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                LocationId source, StringId name) {
    if(!requireFloating(resolver, type, source, "fma"_v)) return nullptr;
    return resolver.ref(resolver.emit<InstFma>(source, name, type, args[0], args[1], args[2]));
}

void defineVectorIntrinsics(Module& core) {
    attachIntrinsic(core, "splat"_v, emitSplat);
    attachIntrinsic(core, "lane"_v, emitLane);
    attachIntrinsic(core, "withLane"_v, emitWithLane);
    attachIntrinsic(core, "horizontalSum"_v, emitReduce<ReduceOp::Add>);
    attachIntrinsic(core, "horizontalProduct"_v, emitReduce<ReduceOp::Mul>);
    attachIntrinsic(core, "horizontalMin"_v, emitReduce<ReduceOp::Min>);
    attachIntrinsic(core, "horizontalMax"_v, emitReduce<ReduceOp::Max>);

    attachIntrinsic(core, "zero"_v, emitZero);
    attachIntrinsic(core, "iota"_v, emitIota);
    attachIntrinsic(core, "lanes"_v, emitLanes);

    attachIntrinsic(core, "min"_v, emitMinMax<CompareOp::Lt>);
    attachIntrinsic(core, "max"_v, emitMinMax<CompareOp::Gt>);
    attachIntrinsic(core, "abs"_v, emitAbs);
    attachIntrinsic(core, "sqrt"_v, emitSqrt);
    attachIntrinsic(core, "trunc"_v, emitRounding<Value::Trunc>);
    attachIntrinsic(core, "floor"_v, emitRounding<Value::Floor>);
    attachIntrinsic(core, "ceil"_v, emitRounding<Value::Ceil>);
    attachIntrinsic(core, "round"_v, emitRounding<Value::Round>);
    attachIntrinsic(core, "fma"_v, emitFma);

    attachIntrinsic(core, "shuffle"_v, emitShuffle<1>);
    attachIntrinsic(core, "shuffle2"_v, emitShuffle<2>);
    attachIntrinsic(core, "alignLanes"_v, emitAlignLanes);
    attachIntrinsic(core, "reverse"_v, emitReverse);
    attachIntrinsic(core, "rotate"_v, emitRotate);
    attachIntrinsic(core, "interleaveLow"_v, emitInterleave<false>);
    attachIntrinsic(core, "interleaveHigh"_v, emitInterleave<true>);

    attachIntrinsic(core, "unpackLow"_v, emitUnpack<false>);
    attachIntrinsic(core, "unpackHigh"_v, emitUnpack<true>);
    attachIntrinsic(core, "packLanes"_v, emitPackLanes);

    attachIntrinsic(core, "any"_v, emitMaskReduce<ReduceOp::Or>);
    attachIntrinsic(core, "all"_v, emitMaskReduce<ReduceOp::And>);
    attachIntrinsic(core, "none"_v, emitNone);
    attachIntrinsic(core, "count"_v, emitMaskReduce<ReduceOp::Add>);
    attachIntrinsic(core, "firstSet"_v, emitFirstSet);
    attachIntrinsic(core, "maskUpTo"_v, emitMaskUpTo);
}

/*
 * The bulk operations - Implementation-Vector.md §9 items 6 and 7, Design-Vector §4.3.
 *
 * `sum(xs)` over a container of a lane type is a vector loop, and over a container of anything else
 * it is the loop it always was. That is the property §12 calls the one worth not shipping without -
 * a program that never writes `Vec` gets faster - and this is the whole of the mechanism: each
 * operation is *two ordinary library functions* and one declaration that chooses between them.
 *
 * **Why the choice cannot be a signature.** Whether `a` has a vector here is a question about the
 * target's vector configuration and the lane's stride, not about anything a constraint could carry:
 * `Vec(U8)` exists on every target this compiles for, `Vec(Long)` exists natively and not on JS, and
 * `Vec(String)` exists nowhere. A class would have to be instantiated per target, which is what
 * `@platform` does for declarations and cannot do for this. So the selection is an intrinsic, run at
 * the call site where both the element and the target are known.
 *
 * **Where §9 items 6 and 7 are departed from, and why.** Those items put the selection in a *chunk
 * kernel* - `sumChunk`, `findInChunk` - with the bulk operation a loop over chunks calling one. Both
 * halves of each kernel are written here instead, over the whole container: the vector half is
 * `for v in vectors(xs)`, which already walks the chunks, handles both tails and folds to a single
 * loop for a contiguous container, and the scalar half is the chunk walk it always was. Splitting
 * them would mean writing the chunk walk twice and would buy a kernel nobody calls - what the plan
 * wanted from a per-chunk form is served by the operations themselves.
 */

// Whether this element has a vector on this target - the question the selection is, asked without
// building the type, because `resolveVectorType` reports a diagnostic for the answers this wants to
// take quietly. The rules are that function's, in the same order.
static bool hasNaturalVector(Module& module, TypePtr element) {
    auto base = *module.types;
    auto& settings = module.context.settings;
    if(!element || isGeneric(base, element)) return false;

    auto stride = laneStride(base, element, targetIntWidths(settings));
    if(!stride) return false;

    // A `Long` on JS is a `bigint`, which is not a lane - Design-Vector §7.3.
    if(isJsMode(settings.mode) && base[element]->kind == Type::Int && stride == 8) return false;

    /*
     * A lane narrower than four bytes used to be refused here, because the local backend had no
     * broadcast, no reduction and no unsigned comparison for one at any feature level - which is
     * what kept `String` off the vector path, a string being a container of bytes. It has all three
     * now (`expandNarrowSplats` and `expandNarrowReduce` in codegen/x64), so the only widths this
     * declines are the ones with no vector at all.
     */

    // A single-lane vector is a scalar with extra steps, so the scalar body is the better answer for
    // it as well as the only correct one where the lane is wider than the register.
    return targetVectorBytes(settings) / stride >= 2;
}

// The implementation this operation takes, which is a plain function in Collections and is found by
// name for the same reason the intrinsic hooks are attached by name: the source is the declaration,
// and a table of pointers beside it would be a second place to keep the two in step.
static ModulePtr<Function> collectionsFunction(Module& module, StringView name) {
    auto collections = module.program.core;
    if(!collections) return nullptr;

    auto found = collections->functions.get(Context::nameHash(name));
    return found ? found.unwrap() : nullptr;
}

/*
 * What to instantiate the chosen body with, worked out from the call rather than passed in.
 *
 * An intrinsic hook is handed values and a result type and not its own bindings, which is enough for
 * every operation above because each of them reads its type off an operand. Here the body has the
 * *same signature* as the declaration that selects it, so matching that signature against the
 * arguments recovers the same bindings the call already made - and `fillDetermined` answers the
 * element the container's own `Chunked` instance decides, which is the position no argument states.
 */
static bool bodyBindings(ExprResolver& resolver, ModulePtr<Function> body, Buffer<ModulePtr<Value>> args,
                         LocationId source, TypeList& out) {
    auto global = resolver.global;
    auto local = resolver.local;

    auto declaration = local[body];
    auto env = functionGen(global, *declaration);
    if(!env) return false;

    for(Size i = 0; i < env->types.size(); i++) out.push(nullptr);

    for(Size i = 0; i < declaration->args.size() && i < args.length; i++) {
        auto declared = local[declaration->args.get(local, i)];
        matchType(global, declared->type, resolver.valueType(args[i]), { out.pointer(), out.size() });
    }

    fillDetermined(resolver.module, *env, out, source);

    for(auto binding: out) {
        if(!binding || isGeneric(global, binding)) return false;
    }

    return true;
}

/*
 * What one element of this container is.
 *
 * `chunkedElement` alone does not answer it, and deliberately: it refuses the two built-in
 * containers before the lookup rather than by it, because everything that takes a `[a]` reaches
 * those through the conversion instead. This position is the other kind - a `c` bound by instance
 * selection, which never converts - so an `Array(a)` and a `Flat(a)` arrive as themselves and are
 * the two most common things a bulk operation is called on.
 */
static TypePtr bulkElement(Module& module, TypePtr container) {
    if(auto element = sliceElement(module, container)) return element;
    if(auto element = ownedElement(module, container)) return element;

    return chunkedElement(module, container);
}

static ModulePtr<Value> expandBulk(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, LocationId source,
                                   StringId name, StringView overVectors, StringView overElements) {
    auto& module = resolver.module;
    auto container = args.length ? resolver.valueType(args[0]) : nullptr;
    auto element = bulkElement(module, container);

    if(!element) {
        module.context.diagnostics.error("internal: a bulk operation over %@ has no element type"_v, source,
                                         describeType(module.context, resolver.global, container));
        return nullptr;
    }

    auto body = collectionsFunction(module, hasNaturalVector(module, element) ? overVectors : overElements);
    if(!body) {
        module.context.diagnostics.error("internal: no implementation of this bulk operation"_v, source);
        return nullptr;
    }

    TypeList bindings;
    if(!bodyBindings(resolver, body, args, source, bindings)) {
        module.context.diagnostics.error("internal: a bulk operation over %@ cannot be instantiated"_v, source,
                                         describeType(module.context, resolver.global, container));
        return nullptr;
    }

    auto specialized = instantiateFunction(module, body, toBuffer(bindings), source);
    if(!specialized) return nullptr;

    ArgList passed;
    for(auto arg: args) passed.push(ResolvedArg(arg));

    return resolver.emitDirectCall(specialized, toBuffer(passed), source, nullptr, name);
}

template<StringView (*names)(bool)>
static ModulePtr<Value> emitBulk(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr,
                                 LocationId source, StringId name) {
    return expandBulk(resolver, args, source, name, names(true), names(false));
}

// One pair per operation. A function rather than a table so that the two names travel together and
// the template above stays one instantiation per operation.
#define BULK_PAIR(op, vectorBody, elementBody) \
    static StringView op##Bodies(bool overVectors) { return overVectors ? vectorBody##_v : elementBody##_v; }

BULK_PAIR(sum, "sumVectors", "sumElements")
BULK_PAIR(product, "productVectors", "productElements")
BULK_PAIR(maximum, "maximumVectors", "maximumElements")
BULK_PAIR(minimum, "minimumVectors", "minimumElements")
BULK_PAIR(occurrences, "occurrencesVectors", "occurrencesElements")
BULK_PAIR(indexOf, "indexOfVectors", "indexOfElements")

#undef BULK_PAIR

void defineBulkOperations(Module& collections) {
    attachIntrinsic(collections, "sum"_v, emitBulk<sumBodies>);
    attachIntrinsic(collections, "product"_v, emitBulk<productBodies>);
    attachIntrinsic(collections, "maximum"_v, emitBulk<maximumBodies>);
    attachIntrinsic(collections, "minimum"_v, emitBulk<minimumBodies>);
    attachIntrinsic(collections, "occurrences"_v, emitBulk<occurrencesBodies>);
    attachIntrinsic(collections, "indexOf"_v, emitBulk<indexOfBodies>);
}

/*
 * The instances, generated where they are asked for.
 *
 * See simd.h for why this is not the loop Implementation-Vector.md §9 items 1 and 2 describe. What
 * it costs against that loop is that an instance now comes into existence part-way through resolving
 * some other module's body, which is a thing the compiler already does - proving a parametric head's
 * constraints instantiates generics and registers their instances, which is why `findInstances`
 * copies its answer out rather than handing back a view.
 *
 * Everything is generated into **Core**, never into the module that asked. What makes an instance
 * over `Vec(I32, 8)` true is the shape of the type, which every module that can name the type agrees
 * on, and coherence requires that "does this type have a Num" have one answer program-wide. That is
 * the rule `structuralInstance` already follows for TrivialCopy.
 */

// A concrete vector or mask, which is the only thing any head below is written over. A generic one -
// `Vec(a)` inside an unspecialized body - is deliberately never answered: what a generic body may do
// with its own type variable is fixed by its declared constraints, and an instance invented here
// would answer for a variable the caller has not substituted yet.
static bool concreteVector(GlobalBase base, TypePtr type) {
    return type && base[type]->kind == Type::Vector && !isGeneric(base, type);
}

static VectorType* vectorOf(GlobalBase base, TypePtr type) {
    return concreteVector(base, type) ? (VectorType*)base[type] : nullptr;
}

/*
 * `FromInt`, `Num` and `Integral` over one vector - §9 item 1.
 *
 * There is nothing here beyond the instance existing, and that is the section's whole claim: each of
 * those classes' methods is one instruction, and an `InstBinary` over a vector is an `InstBinary`.
 * So `a * b + 1` over two `Vec(Float)` needs no machinery that two `Float`s did not already need.
 *
 * `FromInt` is not optional and is not a choice this makes: `class (FromInt(a)) Num(a)`, and the
 * superclass is checked where the instance is used, so a vector with `Num` has to say what an
 * integer literal means as one. `emitVectorFromLiteral` answers "the literal in every lane", which
 * is the only answer that is not arbitrary - and it makes `1 :: Vec(Int)` a legal spelling of
 * `splat(1)`.
 *
 * `Eq` and `Ord` are absent, and must stay absent. A comparison of two vectors answers a *mask*
 * rather than a `Bool` (Design-Vector §3.1), so neither class describes what comparing vectors is;
 * generating them would type-check and emit a `Cmp` the verifier rejects. `Lanewise` below is what
 * does describe it.
 */
static ModulePtr<ClassInstance> numericInstance(Module& core, GlobalPtr<TypeClass> typeClass,
                                                CoreClasses& classes, TypePtr type) {
    auto base = *core.types;
    auto vector = vectorOf(base, type);

    // A mask has no arithmetic. Its lanes are all-ones or all-zeros and every operation `Num` names
    // would produce something that is neither, so the class that describes it is `Bitwise`, which
    // maskBitwiseInstance answers below.
    if(!vector || vector->isMask) return nullptr;

    if(typeClass == classes.fromInt) {
        IntrinsicMethod methods[] = { { "fromInt"_v, 1, emitVectorFromLiteral } };
        return generateInstance(core, typeClass, { &type, 1 }, { methods, 1 });
    }

    // The decimal literal, on exactly the terms the integer one is here: `Num` does not declare
    // `FromDecimal` as a superclass, so nothing forces this the way `fromInt` is forced - what asks
    // for it is a body generic over a float type, where every polynomial coefficient is a written
    // decimal and has to mean the same thing whether the substitution was `Float` or `Vec(Float)`.
    // Math's shared approximations are the only caller today and are the whole reason it exists.
    //
    // Refused for an integer lane, which is `FromDecimal`'s own rule rather than a vector one: there
    // is no `FromDecimal(Int)` either, and a lane type answering a question its scalar declines
    // would be the one place the vector ladder and the scalar one disagreed.
    if(typeClass == classes.fromDecimal) {
        if(base[vector->content]->kind != Type::Float) return nullptr;

        IntrinsicMethod methods[] = { { "fromDecimal"_v, 1, emitVectorFromLiteral } };
        return generateInstance(core, typeClass, { &type, 1 }, { methods, 1 });
    }

    if(typeClass == classes.num) return defineNum(core, type);

    // The bitwise half is an integer question, so a float vector reaches `Num` and stops - exactly
    // as `Float` itself does.
    //
    // Two classes rather than one since `Bitwise` was split out of `Integral`, and both are asked
    // here for the same reason either is: `Integral` declares `Bitwise` as a superclass, so a lookup
    // that reaches `Integral(Vec(Int, 4))` has to be able to reach `Bitwise(Vec(Int, 4))` too, and
    // there is no declaration of either anywhere for the superclass check to find.
    if(base[vector->content]->kind != Type::Int) return nullptr;

    if(typeClass == classes.bitwise) return defineBitwise(core, type, emitUnary<Value::Not>);
    if(typeClass == classes.integral) return defineIntegral(core, type);

    return nullptr;
}

/*
 * `Bitwise` over a mask - Design-Vector §3.2's "masks themselves get `and`/`or`/`not`".
 *
 * The four and nothing else, which is the whole class now. `&&`, `||` and `!` used to arrive with
 * them as `Logic` defaults and were never what a mask wanted: they are `Truth` questions answering a
 * single `Bool`, where combining two masks is a lanewise answer, and `&&`'s right operand is `@lazy`
 * so a mask that reached the default paid for a thunk in the innermost loop of a kernel. A mask has
 * no `Truth` instance and needs none - `count`, `bits` and `select` are how a lane answer becomes a
 * scalar one.
 *
 * `not` is the bitwise complement and not `Bool`'s `xor 1`. A mask lane is all-ones or all-zeros, so
 * complementing it lands back inside the type; a single bit flipped would not.
 */
static ModulePtr<ClassInstance> maskBitwiseInstance(Module& core, TypePtr type) {
    auto mask = vectorOf(*core.types, type);
    if(!mask || !mask->isMask) return nullptr;

    return defineBitwise(core, type, emitUnary<Value::Not>);
}

/*
 * The conversion ladder over vectors - §9 item 2, Design-Vector §3.4.
 *
 * The claim the section makes is that a vector conversion is not a vector operation: it is `Widen`,
 * `Narrow` and `Bitcast` over vector types under the rules the language already has. What makes that
 * true here rather than approximately true is that the lane question is asked of the *same* instance
 * table the scalar conversion asks - `Widen(Vec(I32, 4), Vec(Double, 4))` exists exactly when
 * `Widen(I32, Double)` does - so there is no second ladder to drift from the first.
 *
 * Two rules, both of which are the shape of the machine rather than a policy:
 *
 *  - a `Widen` or a `Narrow` keeps the lane *count* and changes the lane type. One that changed the
 *    count is `unpackLow`/`packLanes` and is a function, because it relates one vector to two or two
 *    to one and a conversion class relates one to one;
 *  - a `Bitcast` keeps the *byte width* and is otherwise free, so `i8x16` to `i32x4` is a rung and
 *    `i8x16` to `i32x8` is not.
 *
 * A mask is in neither. It is not a number at any width, and reinterpreting one is a question about
 * a representation this language deliberately does not fix (see Design-Vector §2.4).
 */
static ModulePtr<ClassInstance> conversionInstance(Module& core, GlobalPtr<TypeClass> typeClass,
                                                   CoreClasses& classes, TypePtr from, TypePtr to) {
    auto base = *core.types;
    auto widths = targetIntWidths(core.context.settings);
    auto source = vectorOf(base, from);
    auto result = vectorOf(base, to);

    if(!source || !result || from == to) return nullptr;

    /*
     * A mask is in `Bitcast` and in neither of the other two, and it is in `Bitcast` only at its own
     * shape - Design-Vector §2.4, as amended when the normalization was dropped.
     *
     * `Mask(Float)`, `Mask(I32)` and `Mask(U32)` used to be one interned type; they are three now,
     * and this is what relates them. Same lane count *and* same lane width, which is stricter than
     * the equal-byte-width rule the vector rungs use: two masks of one byte width but different lane
     * shapes - an `m8x16` and an `m32x4` - do not have corresponding lanes, so reinterpreting one as
     * the other is a repacking rather than a renaming. At one shape the bits are the same bits and
     * the instance emits nothing.
     *
     * **`Widen` and `Narrow` are deliberately absent, and the two directions are absent for
     * different reasons.** Narrowing a mask is meaningful - truncation carries all-ones to all-ones
     * and all-zeros to all-zeros - and is blocked on the same `packssdw` the `packLanes` of §12 is
     * blocked on, so it would be a rung no backend could emit. Widening is *not* meaningful: a
     * zero-extended `0xFFFF` is `0x0000FFFF`, which is neither of a mask's two patterns, so the rung
     * would be wrong rather than unimplemented. A class whose two directions fail differently is
     * worth leaving empty until the operation that changes a mask's lane width is written as what it
     * is, which is a pack.
     */
    if(source->isMask || result->isMask) {
        if(typeClass != classes.bitcast) return nullptr;
        if(!source->isMask || !result->isMask) return nullptr;
        if(source->count != result->count) return nullptr;
        if(laneStride(base, source->content, widths) != laneStride(base, result->content, widths)) return nullptr;

        return defineBitcast(core, from, to);
    }

    if(typeClass == classes.bitcast) {
        auto sourceBytes = laneStride(base, source->content, widths) * constValue(base, source->count);
        auto resultBytes = laneStride(base, result->content, widths) * constValue(base, result->count);
        if(sourceBytes != resultBytes) return nullptr;

        return defineBitcast(core, from, to);
    }

    if(typeClass != classes.widen && typeClass != classes.narrow) return nullptr;
    if(source->count != result->count) return nullptr;

    // The scalar rung, which is what decides whether this pair belongs to the widening ladder or the
    // narrowing one. Asked rather than recomputed: `widens` lives in core.cpp and reads a `@bits`
    // refinement's range, and a second copy of that rule is a second thing to keep in step.
    TypePtr lanes[] = { source->content, result->content };
    if(!findInstance(core, typeClass, { lanes, 2 })) return nullptr;

    return defineConversion(core, typeClass == classes.widen ? "Widen"_v : "Narrow"_v,
                            typeClass == classes.widen ? "widen"_v : "truncate"_v, from, to);
}

// `Lanewise.select`, which is the `Select` the IR already had. A method rather than one of the
// intrinsics above because it is the only operation that relates a mask back to the vector it came
// from, and the class keyed on that pair is where the relation lives.
static ModulePtr<Value> emitLaneSelect(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                       LocationId source, StringId name) {
    return resolver.ref(resolver.emit<InstSelect>(source, name, type, args[0], args[1], args[2]));
}

/*
 * `Lanewise` over a vector and its mask - §9 item 3, Design-Vector §3.2.
 *
 * Seven methods and seven single instructions: the six comparisons are one `Cmp` each - which
 * answers a mask over a vector by the typing rule §3.2 states rather than by anything new - and
 * `select` is the `Select` the IR already had, with a condition per lane instead of one for the
 * whole value.
 *
 * The functional dependency is what the second argument is *for*: `a .< b` has to infer its own
 * result without an ascription, so a caller asks with a hole where the mask goes and reads back what
 * the head put there. `maskFor` is what fills it, and it is the same normalization that made
 * `Mask(Float)` and `Mask(I32)` one type - which is why `select(x .< y, ints, others)` type-checks
 * with the mask coming from one vector and being applied to another of the same shape.
 *
 * Asking with the mask already bound is answered too, and is answered by *checking* it rather than
 * by trusting it: a head that agrees with the dependency selects, and one that does not selects
 * nothing and is reported as a missing instance.
 */
/*
 * And the same class over a *scalar*, whose mask is `Bool` - the one-lane case, generated here
 * rather than in core.cpp for the reason the vector heads are generated on demand.
 *
 * What asks for it is the one thing this whole family exists to make possible: an approximation
 * written once and instanced for `Float`, `Double` and `Vec(Float)` alike (Analysis-Library.md
 * §2.4). Such a body has to choose between two values by a comparison, and the only spelling of
 * that which covers both is `select(x .< y, a, b)` - `if` cannot, because a vector comparison
 * answers a mask and a condition wants a `Bool`. Without this instance the shared body is
 * expressible for the vector instances and not for the scalar ones, which is exactly the split the
 * section says a class removes.
 *
 * `select` over a `Bool` condition is the `Select` instruction if-conversion already produces, so
 * nothing new is emitted and a scalar `select` costs what the `if` it replaces costs. The dotted
 * comparisons over a scalar are the plain ones - `x .< y` and `x < y` are the same instruction -
 * and having both spellings is the price of one body serving both shapes.
 *
 * Numbers only. `Bool` and a record are comparable and neither is a lane of anything, so a
 * `Lanewise` over one would be a second spelling of `Eq` with nothing asking for it.
 */
static ModulePtr<ClassInstance> scalarLanewiseInstance(Module& core, GlobalPtr<TypeClass> typeClass,
                                                       TypePtr type, TypePtr asked) {
    auto base = *core.types;
    auto kind = base[type]->kind;
    if(kind != Type::Int && kind != Type::Float) return nullptr;

    auto mask = core.program.scalar.bool_;
    if(!mask || (asked && asked != mask)) return nullptr;

    IntrinsicMethod methods[] = {
        { ".=="_v, 2, emitCompare<CompareOp::Eq> },
        { ".!="_v, 2, emitCompare<CompareOp::Ne> },
        { ".<"_v,  2, emitCompare<CompareOp::Lt> },
        { ".<="_v, 2, emitCompare<CompareOp::Le> },
        { ".>"_v,  2, emitCompare<CompareOp::Gt> },
        { ".>="_v, 2, emitCompare<CompareOp::Ge> },
        { "select"_v, 3, emitLaneSelect },
    };

    TypePtr head[] = { type, mask };
    return generateInstance(core, typeClass, { head, 2 }, { methods, 7 });
}

static ModulePtr<ClassInstance> lanewiseInstance(Module& core, GlobalPtr<TypeClass> typeClass,
                                                 TypePtr vector, TypePtr asked) {
    if(!isVectorType(*core.types, vector)) return scalarLanewiseInstance(core, typeClass, vector, asked);

    auto mask = maskFor(core, vector);
    if(!mask || (asked && asked != mask)) return nullptr;

    IntrinsicMethod methods[] = {
        { ".=="_v, 2, emitCompare<CompareOp::Eq> },
        { ".!="_v, 2, emitCompare<CompareOp::Ne> },
        { ".<"_v,  2, emitCompare<CompareOp::Lt> },
        { ".<="_v, 2, emitCompare<CompareOp::Le> },
        { ".>"_v,  2, emitCompare<CompareOp::Gt> },
        { ".>="_v, 2, emitCompare<CompareOp::Ge> },
        { "select"_v, 3, emitLaneSelect },
    };

    TypePtr head[] = { vector, mask };
    return generateInstance(core, typeClass, { head, 2 }, { methods, 7 });
}

ModulePtr<ClassInstance> vectorInstance(Module& module, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args) {
    auto& program = module.program;
    auto& classes = program.coreClasses;
    if(!typeClass || !program.core) return nullptr;

    auto& core = *program.core;

    if(args.length == 1) {
        if(!args[0]) return nullptr;

        // `Bitwise` is the one class a mask and an integer vector both join - which is the point of
        // its being separate from `Integral`, since a mask has the four operations and none of the
        // arithmetic. So the mask answer is tried and *fallen through*, not returned: a
        // `Bitwise(Vec(Int, 4))` is not a mask and is still an instance this has to supply.
        if(typeClass == classes.bitwise) {
            if(auto masked = maskBitwiseInstance(core, args[0])) return masked;
        }

        return numericInstance(core, typeClass, classes, args[0]);
    }

    if(args.length != 2 || !args[0]) return nullptr;

    // The one head whose second position may be a hole, because the dependency fills it.
    if(typeClass == classes.lanewise) return lanewiseInstance(core, typeClass, args[0], args[1]);

    if(!args[1]) return nullptr;
    return conversionInstance(core, typeClass, classes, args[0], args[1]);
}
