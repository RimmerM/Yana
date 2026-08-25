#include "host.h"
#include "core.h"
#include "intrinsic.h"
#include "name.h"
#include "../parse/parser.h"

/*
 * The host operations - Implementation-Containers.md §14.1.
 *
 * Two shapes, and the difference between them is the whole of what a host node had to be able to
 * say:
 *
 *   A **member** operation - `length`, `copyWithin` - is an `InstNative` carrying the member's name. The
 *   emitter prints the name and knows nothing else about it, which is what Analysis-JS.md §2.4 asks
 *   for when it rules host knowledge out of codegen: a declaration below says `.copyWithin` and the
 *   backend says nothing.
 *
 *   An **element** operation - reading, writing and borrowing `a[i]` - is not a node at all. It is a
 *   `ProjectionKind::Index` over a place rooted in the array reference, which is the same projection
 *   `[T *n]` introduced (§6) and which §14.1 expected to be a second node. A place is an lvalue, so
 *   one form gives the read, the write and the borrow together where an operation would have given
 *   only the read - and, more importantly, it puts a host element in exactly the position a native
 *   one is in for every pass above the backend. `store(p + i, v)` natively and `hostWrite(p, i, v)`
 *   here are both an assignment through a place rooted in a raw pointer, so the ownership passes see
 *   one shape rather than two and `Array(a)`'s teardown is one rule rather than two.
 */

// Host's source is `lib/Native/Host.js.yana` - a file selected by its name, so a native build
// never reads it at all. See TargetSelector in settings.h.

// Declared in host.h, which is where the rule and its two readers are argued. At file scope
// rather than in the anonymous namespace below because the JS emitter is the other reader.
StringView typedArrayFor(GlobalBase global, TypePtr element) {
    if(!element) return ""_v;

    // A `@bits` refinement dispatches as the type it refines - repr.md's rule, and it is the right
    // one here too: `@bits(30) U32` is a `U32` in a `Uint32Array`, at the width the array has.
    auto type = global[canonicalType(global, element)];

    if(type->kind == Type::Float) {
        return ((FloatType*)type)->width == FloatType::Float ? "Float32Array"_v : "Float64Array"_v;
    }

    if(type->kind != Type::Int) return ""_v;

    auto& integer = *(IntType*)type;

    // `Bool` is an `Int` of one bit here and is deliberately not one of these - see typedArrayFor's
    // declaration. So is every width the machine does not have.
    switch(integer.bits) {
        case 8:  return integer.isSigned ? "Int8Array"_v : "Uint8Array"_v;
        case 16: return integer.isSigned ? "Int16Array"_v : "Uint16Array"_v;
        case 32: return integer.isSigned ? "Int32Array"_v : "Uint32Array"_v;
        default: return ""_v;
    }
}

// Declared in host.h with the argument for it. The pointee rather than the pointer, because what a
// `%a` is on this target is a reference to storage holding `a`s and the row is chosen by the `a`.
bool hostPropertiesElided(GlobalBase global, TypePtr pointer) {
    if(!pointer || global[pointer]->kind != Type::Ptr) return false;

    auto element = pointeeType(global, pointer);
    if(!element || isGeneric(global, element)) return false;

    return typedArrayFor(global, element).length == 0;
}


namespace {

/*
 * A member operation.
 *
 * The name is a template parameter rather than a field of a table, because `Intrinsic` is a plain
 * function pointer with nothing to capture in - which is the same reason `emitBinary` is a template
 * over its `Value::Kind`. `HostMember` names the members this module has; adding one is a line here
 * and a declaration above.
 */
enum class HostMember: U8 {
    Length,
    CopyWithin,
    CharCodeAt,
    IndexOf,

    // `n.toExponential()` - the shortest decimal that reads back as this number, which is what the
    // specification requires of the no-argument form and is exactly what Ryu computes on the other
    // target. With an argument it is that many digits after the point instead, which is what the
    // `Float` instance walks upwards to find the shortest representation at its own width.
    ToExponential,

    // The operators, whose "member name" is the operator's own spelling - see NativeOp::HostBinary.
    // They are in the same enum because they are read the same way: `method` carries the text and
    // the emitter prints it, and which arm prints it as a member and which as an operator is the
    // `NativeOp` rather than anything here.
    Concat,
    Equal,
    Less,

    // A dotted path on the host's global scope rather than a member of anything - see
    // NativeOp::HostGlobalCall.
    FromCharCode,
    Log,

    // `Number(text)` - the host's own decimal-to-double conversion, correctly rounded by
    // specification. It is the other target's `readDouble` and its whole table.
    ToNumber,

    // `String(value)` - the host's own value-to-decimal conversion, and the inverse of the row
    // above. For a `bigint` or an integral `number` it is the decimal digits; for a `Number` it is
    // *the shortest decimal that reads back as that number, ties to even*, which is the same
    // sentence Ryu's correctness theorem is and is why `Ryu.js.yana` needs no tie correction on top
    // of it. `toExponential()` with no argument is shortest as well and breaks a tie the other way.
    ToString,

    // `Date.now()` - integral milliseconds since 1970-01-01 UTC, as a `number`. A dotted path like
    // the two above it. It is the whole of what this target's `Native/Clock.js.yana` is written over,
    // and `performance.now()` is deliberately not beside it: see that file for why.
    DateNow,

    // The one that is a statement rather than a call or an operator - see NativeOp::HostThrow. Its
    // "member name" is never printed; the emitter writes `throw` itself.
    Fail,
};

StringView hostMemberName(HostMember member) {
    switch(member) {
        case HostMember::Length: return "length"_v;
        case HostMember::CopyWithin: return "copyWithin"_v;
        case HostMember::CharCodeAt: return "charCodeAt"_v;
        case HostMember::IndexOf: return "indexOf"_v;
        case HostMember::ToExponential: return "toExponential"_v;
        case HostMember::Concat: return "+"_v;
        case HostMember::Equal: return "==="_v;
        case HostMember::Less: return "<"_v;
        case HostMember::FromCharCode: return "String.fromCharCode"_v;
        case HostMember::Log: return "console.log"_v;
        case HostMember::ToNumber: return "Number"_v;
        case HostMember::ToString: return "String"_v;
        case HostMember::DateNow: return "Date.now"_v;
        case HostMember::Fail: return "throw"_v;
    }

    return "length"_v;
}

template<NativeOp op, HostMember member>
static ModulePtr<Value> emitHostMember(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                       LocationId source, StringId name) {
    auto text = hostMemberName(member);
    auto instruction = resolver.create<InstNative>(source, name, type, op,
                                                   resolver.context.addUnqualifiedName(text.ptr, text.length));

    for(auto arg: args) instruction->args.push(resolver.module.arena, arg);

    resolver.append(instruction);
    return isUnit(resolver.global, type) ? nullptr : resolver.ref(instruction);
}

/*
 * The typed row of §14, as a constant this call site can be folded against.
 *
 * The element comes out of the argument's *declared* type rather than out of anything the value
 * knows: `%a` at this call is a pointer to whatever `a` was substituted with, and a pointer's
 * pointee is exactly the question typedArrayFor asks.
 */
static ModulePtr<Value> emitFixedCapacity(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                          LocationId source, StringId name) {
    auto global = resolver.global;
    auto element = pointeeType(global, resolver.valueType(args[0]));

    return resolver.makeInt(source, type, typedArrayFor(global, element).length != 0);
}


// `yana$grow(self, capacity)` - a new typed array of that capacity with this one's contents in it.
// A helper rather than an expression because it is two statements; see the JS emitter.
static ModulePtr<Value> emitHostGrow(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                     LocationId source, StringId name) {
    auto instruction = resolver.create<InstNative>(source, name, type, NativeOp::HostGrow);
    for(auto arg: args) instruction->args.push(resolver.module.arena, arg);

    resolver.append(instruction);
    return resolver.ref(instruction);
}

// `[]`. The variadic form - a literal with its elements already in it - is built by `resolveArray`
// rather than declared, because a literal has an arity per literal and a declaration has one arity.
static ModulePtr<Value> emitHostArray(ExprResolver& resolver, Buffer<ModulePtr<Value>>, TypePtr type,
                                      LocationId source, StringId name) {
    auto instruction = resolver.create<InstNative>(source, name, type, NativeOp::HostArray);
    resolver.append(instruction);

    return resolver.ref(instruction);
}

/*
 * The element, as a place.
 *
 * Rooted at the array reference exactly as `*(p + i)` is rooted at `p`, so everything above the
 * backend - the borrow checker, the drop pass, the escape analysis - sees the shape it already knows
 * and none of them needed a case for a host container.
 */
static Place hostElement(ExprResolver& resolver, Buffer<ModulePtr<Value>> args) {
    return resolver.project(Place::atPointer(args[0]), ProjectionKind::Index, 0, args[1]);
}

static ModulePtr<Value> emitHostRead(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr,
                                     LocationId source, StringId name) {
    return resolver.load(hostElement(resolver, args), source, name);
}

// An assignment and not an initialization, for the reason `store` is one: what a raw pointer names
// is memory the program manages itself, and a pointer root is outside the ownership graph, so
// nothing is dropped either way.
static ModulePtr<Value> emitHostWrite(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr,
                                      LocationId source, StringId) {
    resolver.assign(hostElement(resolver, args), args[2], source);
    return nullptr;
}

template<bool mut>
static ModulePtr<Value> emitHostAt(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                   LocationId source, StringId name) {
    return resolver.ref(resolver.emit<InstBorrow>(source, name, type, hostElement(resolver, args), mut));
}

/*
 * The vector transfers - `lanes` element accesses each, and no loop.
 *
 * The lane count comes off the vector type, which is settled by the time an intrinsic is expanded, so
 * what is emitted here is straight-line code whose length the type decided. That is the same shape
 * `iota` has in simd.cpp and it is the same argument: a vector on this target is `lanes` values, so
 * writing the lanes out one at a time is not an unrolling of anything - there was never a loop.
 */
static ModulePtr<Value> hostLaneIndex(ExprResolver& resolver, ModulePtr<Value> base, U32 lane,
                                      ModulePtr<Value> limit, LocationId source) {
    auto size = resolver.module.scalar.size;
    auto index = base;

    if(lane) {
        auto step = resolver.makeInt(source, size, lane);
        index = resolver.ref(resolver.emit<InstBinary>(source, StringId(), size, Value::Add, base, step));
    }

    if(!limit) return index;

    // The clamp the tail needs, and the whole of what `hostVectorUpTo` is: `min(index, limit)`, so a
    // lane past the end reads the last element rather than `undefined`.
    auto over = resolver.ref(resolver.emit<InstCmp>(source, StringId(), resolver.module.scalar.bool_,
                                                    index, limit, CompareOp::Gt));
    return resolver.ref(resolver.emit<InstSelect>(source, StringId(), size, over, limit, index));
}

template<bool clamped>
static ModulePtr<Value> emitHostVector(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                       LocationId source, StringId name) {
    auto lanes = vectorLanes(resolver.global, type);
    if(!lanes) return nullptr;

    auto limit = clamped ? args[2] : nullptr;
    ModulePtr<Value> value = nullptr;

    for(U32 i = 0; i < lanes; i++) {
        auto index = hostLaneIndex(resolver, args[1], i, limit, source);
        auto element = resolver.load(hostElementPlace(resolver, args[0], index), source);
        auto last = i + 1 == lanes;

        // Lane zero is a splat rather than a write into a zero, which is one instruction fewer and
        // is what leaves nothing behind when the vector is one lane wide.
        value = i ? resolver.ref(resolver.emit<InstVecLane>(source, last ? name : StringId(), type,
                                                            value, U16(i), element))
                  : resolver.ref(resolver.emit<InstVecSplat>(source, last ? name : StringId(), type, element));
    }

    return value;
}

/*
 * A machine word out of a byte array, and back into one - see NativeOp::HostWordRead.
 *
 * The accessor's name is the whole of what this stage decides, and it decides it from the width the
 * declaration was written at rather than from anything at the call. The order travels as the last
 * argument, which is where the host takes it too - so nothing here has to know which of the eight
 * library functions called it.
 */
template<NativeOp op, U32 bits>
static ModulePtr<Value> emitHostWord(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                     LocationId source, StringId name) {
    auto read = op == NativeOp::HostWordRead;
    auto text = bits == 16 ? (read ? "getUint16"_v : "setUint16"_v)
              : bits == 32 ? (read ? "getUint32"_v : "setUint32"_v)
                           : (read ? "getBigUint64"_v : "setBigUint64"_v);

    auto instruction = resolver.create<InstNative>(source, name, type, op,
                                                   resolver.context.addUnqualifiedName(text.ptr, text.length));

    for(auto arg: args) instruction->args.push(resolver.module.arena, arg);

    resolver.append(instruction);
    return read ? resolver.ref(instruction) : nullptr;
}

static ModulePtr<Value> emitHostSetVector(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr,
                                          LocationId source, StringId) {
    auto vector = resolver.valueType(args[2]);
    auto lanes = vectorLanes(resolver.global, vector);
    auto lane = vectorLane(resolver.global, vector);
    if(!lanes || !lane) return nullptr;

    for(U32 i = 0; i < lanes; i++) {
        auto index = hostLaneIndex(resolver, args[1], i, nullptr, source);
        auto element = resolver.ref(resolver.emit<InstVecLane>(source, StringId(), lane, args[2], U16(i)));

        resolver.assign(hostElementPlace(resolver, args[0], index), element, source);
    }

    return nullptr;
}

} // namespace

Place hostElementPlace(ExprResolver& resolver, ModulePtr<Value> array, ModulePtr<Value> index) {
    ModulePtr<Value> args[] = { array, index };
    return hostElement(resolver, { args, 2 });
}

ModulePtr<Value> emitHostLengthOf(ExprResolver& resolver, ModulePtr<Value> array, TypePtr type,
                                  LocationId source, StringId name) {
    return emitHostMember<NativeOp::HostField, HostMember::Length>(resolver, { &array, 1 }, type,
                                                                   source, name);
}

namespace {

} // namespace

void definePreludeHost(Program& program, Module& native) {
    auto& context = program.context;
    auto module = &native;

    /*
     * Only where the declarations exist.
     *
     * Every one of them is `@platform(js)`, so on a native build the host file declared nothing and
     * there is nothing to attach a hook to - which `attachIntrinsic` would report as an internal
     * error rather than skip, and rightly: a missing declaration is normally a typo.
     */
    if(!isJsMode(context.settings.mode)) return;

    attachIntrinsic(*module, "hostArray"_v, emitHostArray);
    attachIntrinsic(*module, "hostLength"_v, emitHostMember<NativeOp::HostField, HostMember::Length>);
    attachIntrinsic(*module, "hostCopyWithin"_v, emitHostMember<NativeOp::HostCall, HostMember::CopyWithin>);
    attachIntrinsic(*module, "hostFixedCapacity"_v, emitFixedCapacity);
    attachIntrinsic(*module, "hostGrow"_v, emitHostGrow);

    attachIntrinsic(*module, "hostRead"_v, emitHostRead);
    attachIntrinsic(*module, "hostWrite"_v, emitHostWrite);
    attachIntrinsic(*module, "hostAt"_v, emitHostAt<false>);
    attachIntrinsic(*module, "hostAtMut"_v, emitHostAt<true>);
    attachIntrinsic(*module, "hostVector"_v, emitHostVector<false>);
    attachIntrinsic(*module, "hostVectorUpTo"_v, emitHostVector<true>);
    attachIntrinsic(*module, "hostSetVector"_v, emitHostSetVector);

    attachIntrinsic(*module, "hostReadU16"_v, emitHostWord<NativeOp::HostWordRead, 16>);
    attachIntrinsic(*module, "hostReadU32"_v, emitHostWord<NativeOp::HostWordRead, 32>);
    attachIntrinsic(*module, "hostReadU64"_v, emitHostWord<NativeOp::HostWordRead, 64>);
    attachIntrinsic(*module, "hostWriteU16"_v, emitHostWord<NativeOp::HostWordWrite, 16>);
    attachIntrinsic(*module, "hostWriteU32"_v, emitHostWord<NativeOp::HostWordWrite, 32>);
    attachIntrinsic(*module, "hostWriteU64"_v, emitHostWord<NativeOp::HostWordWrite, 64>);

    // The host string - Implementation-String.md part 2's JS column. `length` is a field and
    // `charCodeAt` a method, exactly as they are for an array; the last three are operators.
    attachIntrinsic(*module, "hostStringLength"_v, emitHostMember<NativeOp::HostField, HostMember::Length>);
    attachIntrinsic(*module, "hostCharCodeAt"_v, emitHostMember<NativeOp::HostCall, HostMember::CharCodeAt>);
    attachIntrinsic(*module, "hostIndexOf"_v, emitHostMember<NativeOp::HostCall, HostMember::IndexOf>);
    attachIntrinsic(*module, "hostConcat"_v, emitHostMember<NativeOp::HostBinary, HostMember::Concat>);
    attachIntrinsic(*module, "hostStringEq"_v, emitHostMember<NativeOp::HostBinary, HostMember::Equal>);
    attachIntrinsic(*module, "hostStringLt"_v, emitHostMember<NativeOp::HostBinary, HostMember::Less>);
    attachIntrinsic(*module, "hostFromCharCode"_v, emitHostMember<NativeOp::HostGlobalCall, HostMember::FromCharCode>);
    attachIntrinsic(*module, "hostLog"_v, emitHostMember<NativeOp::HostGlobalCall, HostMember::Log>);
    attachIntrinsic(*module, "hostFail"_v, emitHostMember<NativeOp::HostThrow, HostMember::Fail>);

    // The float text tier - Analysis-Library.md §2.1's JS column. Both are the host's own, which is
    // the same ruling `indexOf` above takes: what these compute is specified exactly, so there is no
    // room for the cross-engine disagreement that keeps the decoding tier out of the host.
    attachIntrinsic(*module, "hostToExponentialAt"_v,
                    emitHostMember<NativeOp::HostCall, HostMember::ToExponential>);
    attachIntrinsic(*module, "hostToNumber"_v,
                    emitHostMember<NativeOp::HostGlobalCall, HostMember::ToNumber>);
    attachIntrinsic(*module, "hostToString"_v,
                    emitHostMember<NativeOp::HostGlobalCall, HostMember::ToString>);

    // The clock - Analysis-Library.md §3.1. A host global call like the two above, and the one host
    // operation whose answer is not a function of its arguments.
    attachIntrinsic(*module, "hostDateNow"_v,
                    emitHostMember<NativeOp::HostGlobalCall, HostMember::DateNow>);

}
