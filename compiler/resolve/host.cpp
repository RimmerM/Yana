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
 * A named host operation - the five forms, and the name read off the call.
 *
 * The whole of what used to be twenty-seven emitters. Each of those carried its member's spelling as
 * a template argument out of a `HostMember` table in this file, so the compiler held the list of
 * host methods the library happened to use and a twenty-eighth was a compiler change. The name is
 * an *argument* now - `op.def`'s `ShapeTerm::Text` - so what is left here is "print the receiver,
 * the name and the rest", which is the same sentence for `charCodeAt` and for a method nobody has
 * written yet.
 *
 * `nameAt` is the one thing that can fail, and it fails the way a shape does: the position is
 * declared `String`, so the type checker has already had it, and what this adds is that the *value*
 * has to be a literal. On this target that is a `ConstString` - Implementation-String.md part 9's JS
 * column, one value kind with no storage and no global - so the test is the kind and the text is the
 * field.
 */
static Maybe<StringId> nameAt(ExprResolver& resolver, ModulePtr<Value> value, LocationId source) {
    auto text = value ? resolver.local[value] : nullptr;

    if(!text || text->kind != Value::ConstString) {
        resolver.context.diagnostics.error("the member name of a host operation has to be a string literal - it is written into the emitted source, so a value computed at run time has nothing to write"_v,
                                           source);
        return Nothing();
    }

    return Just(((ConstString*)text)->text);
}

/*
 * The node, with the name at `nameIndex` taken out of the argument list.
 *
 * Out rather than through: `method` is where an `InstNative` carries a name, and leaving the literal
 * among the operands as well would be a use of a value the emitted source never reads - which every
 * pass that walks operands would then have to know to ignore.
 */
template<NativeOp op, U8 nameIndex>
static ModulePtr<Value> emitHostNamed(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                      LocationId source, StringId name) {
    auto member = nameAt(resolver, args[nameIndex], source);
    if(!member) return nullptr;

    auto instruction = resolver.create<InstNative>(source, name, type, op, member.unwrap());

    for(Size i = 0; i < args.length; i++) {
        if(i != nameIndex) instruction->args.push(resolver.module.arena, args[i]);
    }

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

/*
 * The hooks, on the roles the declarations claimed.
 *
 * Twelve of these serve the twenty-seven host operations `lib` declares, because the *name* is an
 * argument rather than part of the role - see the host block in `op.def`. What is left of the list
 * is one line per shape the emitted source has, and adding a host method to the library reaches none
 * of them.
 *
 * The `isJsMode` test that used to guard the whole function is gone with the names. Every one of
 * these declarations lives in a `.js.yana` file, so a native build read none of them and claimed
 * none of these roles - and a role nothing claimed is one `attachIntrinsic` skips. The question
 * "does this build have the declaration" is answered by the platform filter, once, where it belongs.
 */
void definePreludeHost(Program& program, Module& native) {
    auto module = &native;

    using intrinsic::Op;

    attachIntrinsic(*module, Op::hostField,   emitHostNamed<NativeOp::HostField, 1>);
    attachIntrinsic(*module, Op::hostCall0,   emitHostNamed<NativeOp::HostCall, 1>);
    attachIntrinsic(*module, Op::hostCall1,   emitHostNamed<NativeOp::HostCall, 1>);
    attachIntrinsic(*module, Op::hostCall2,   emitHostNamed<NativeOp::HostCall, 1>);
    attachIntrinsic(*module, Op::hostCall3,   emitHostNamed<NativeOp::HostCall, 1>);
    attachIntrinsic(*module, Op::hostGlobal0, emitHostNamed<NativeOp::HostGlobalCall, 0>);
    attachIntrinsic(*module, Op::hostGlobal1, emitHostNamed<NativeOp::HostGlobalCall, 0>);
    attachIntrinsic(*module, Op::hostBinary,  emitHostNamed<NativeOp::HostBinary, 1>);
    attachIntrinsic(*module, Op::hostThrow,   emitHostNamed<NativeOp::HostThrow, 0>);

    attachIntrinsic(*module, Op::hostViewRead,  emitHostNamed<NativeOp::HostWordRead, 1>);
    attachIntrinsic(*module, Op::hostViewWrite, emitHostNamed<NativeOp::HostWordWrite, 1>);

    // The rest, whose expansion is a shape rather than a name.
    attachIntrinsic(*module, Op::hostArray,         emitHostArray);
    attachIntrinsic(*module, Op::hostGrow,          emitHostGrow);
    attachIntrinsic(*module, Op::hostFixedCapacity, emitFixedCapacity);

    attachIntrinsic(*module, Op::hostRead,  emitHostRead);
    attachIntrinsic(*module, Op::hostWrite, emitHostWrite);
    attachIntrinsic(*module, Op::hostAt,    emitHostAt<false>);
    attachIntrinsic(*module, Op::hostAtMut, emitHostAt<true>);

    attachIntrinsic(*module, Op::hostVector,     emitHostVector<false>);
    attachIntrinsic(*module, Op::hostVectorUpTo, emitHostVector<true>);
    attachIntrinsic(*module, Op::hostSetVector,  emitHostSetVector);
}
