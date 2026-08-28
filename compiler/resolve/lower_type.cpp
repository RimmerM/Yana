/*
 * Measurement, and what a declared width implies.
 *
 * Two questions, and they are not the same one: what a resolve type *is* in the lower IR, which is
 * the register class an operation on it uses, and how many of its bits are meaningful, which is
 * what a `@bits` refinement changes without changing the first. Everything that masks, truncates or
 * sign-extends is here, because the rule it applies is a property of the type rather than of the
 * instruction that reached it.
 */

#include "lower_internal.h"

LowerType lowerType(LowerContext& lower, TypePtr type) {
    auto base = lower.global;
    auto value = base[type];
    /*
     * A payload-free sum is its number, so it is the register class that number needs.
     *
     * `Int32` unconditionally is what this used to be, and `@value` is what makes that wrong: the
     * numbers an enumeration is pinned to are somebody else's ABI, and one of them may not fit in
     * thirty-two bits. `enumRange` already computes the width for the packer, and `computeRecord`
     * already gives such a type eight bytes of storage - so the two halves disagreed, and what a
     * `@value(4294967296)` constructor lowered to was that value with its top bit gone. `valueOf`
     * answered zero for it, and every comparison against it agreed, because both sides truncated.
     *
     * A negative value is covered without a case of its own: `EnumRange::bits` is already the whole
     * signed word such a declaration takes.
     */
    if(value->kind == Type::Record && ((RecordType*)value)->layout == RecordType::Enum) {
        auto range = enumRange(base, *(RecordType*)value);
        return range.bits > 32 ? LowerType::Int64 : LowerType::Int32;
    }

    if(value->kind == Type::Ptr || value->kind == Type::Borrow || isMemoryType(base, type)) {
        return LowerType::Pointer;
    }

    if(value->kind == Type::Int) {
        // The target's word for a `Size`, and the declared class for everything else - see IntWidths.
        // A thirty-two-bit word makes `Size` an `Int32` here, which is exactly what the JS target has
        // always emitted and is now said once rather than by `Size` being a name for `Int` there.
        auto registerBits = ((IntType*)value)->registerBitsOn(lower.repr.target.integers);
        return registerBits > 32 ? LowerType::Int64 : LowerType::Int32;
    }

    if(value->kind == Type::Float) {
        return ((FloatType*)value)->width == FloatType::Double ? LowerType::Float64 : LowerType::Float32;
    }

    /*
     * A vector, whose lane count and lane kind travel across the seam unchanged.
     *
     * The resolve type has already spent the natural form, so what arrives here is a concrete count
     * and the translation is a lookup - which is the whole of why Design-Vector §2.1 spends it during
     * resolution. `isMemoryType` above answered false for one because `isDirectType` did, so a vector
     * reaches this rather than becoming a `Pointer` to storage.
     */
    if(value->kind == Type::Vector) {
        auto vector = (VectorType*)value;
        auto stride = laneStride(base, vector->content, lower.repr.target.integers);
        auto lane = LowerLane::Int8;

        if(vector->content && base[vector->content]->kind == Type::Float) {
            lane = stride == 8 ? LowerLane::Float64 : LowerLane::Float32;
        } else {
            lane = stride == 1 ? LowerLane::Int8 : stride == 2 ? LowerLane::Int16
                 : stride == 4 ? LowerLane::Int32 : LowerLane::Int64;
        }

        auto lanes = U32(constValue(base, vector->count));
        return vector->isMask ? maskType(lane, lanes) : vectorType(lane, lanes);
    }

    assertTrue("unit and unsupported types have no lower value" == nullptr);
    return LowerType::Int32;
}

/*
 * Whether a value of this type carries a sign - which decides whether a narrow load sign-extends
 * into its register and whether a widening cast of one does.
 *
 * **A payload-free sum answers its own values' question**, and the case that needs it is
 * `data Signal = @value(-1) Failed | @value(0) Idle`. Such a sum *is* its discriminant, held at
 * whatever width its values need, and a declaration that pinned a number below zero is holding a
 * signed one - so a load of the byte `Failed` occupies has to bring back `-1` and not `255`, and
 * `valueOf` widening it has to answer what the declaration wrote.
 *
 * Answering false here was one defect with two faces, and the wider one was not the enum-specific
 * code at all: it read wrong through *any* erased boundary, because a generic body loads the value
 * out of storage sized by the type descriptor while a specialized one had it in a register already
 * wide enough to hide the question. So `viaGeneric(Failed)` answered 255 where the same call
 * specialized answered -1 - and 65236 for a two-byte enum, which is the same arithmetic one width
 * up. See test/bench/findings.md §69.
 *
 * The scan is over a payload-free sum's constructor list, which is short, and every other type
 * leaves at the first test. An enum that pins nothing, or nothing negative, answers false as it
 * always did and is unchanged in every path.
 */
bool signedType(GlobalBase base, TypePtr type) {
    auto value = base[type];
    if(value->kind == Type::Int) return ((IntType*)value)->isSigned;
    if(value->kind != Type::Record) return false;

    auto record = (RecordType*)value;
    if(record->layout != RecordType::Enum) return false;

    for(auto constructor: record->constructors.contents(base)) {
        if(constructor.value < 0) return true;
    }

    return false;
}

/*
 * Whether *arithmetic* on this type is the signed instruction - which is not the same question as
 * the one above, and the difference is a vector.
 *
 * `signedType` asks whether a value of this type carries a sign, and its readers are a narrow load
 * (does it sign-extend into the register) and a packed field (does it sign-extend out of the word).
 * Neither means anything for a vector, and a vector answering yes made a load of one claim to
 * sign-extend sixteen bytes.
 *
 * This asks which of two instructions the machine keeps apart an operation becomes, and there a
 * vector's answer is its lane's: `Vec(Int)` compares and divides signed. Nothing above the lower IR
 * produced either until stage 9 - a comparison had no spelling before `class Lanewise` and the
 * arithmetic had no instance below the four natural widths - which is why one predicate served both
 * readers for as long as it did. A multiply, which is what `VecOps.yana` exercises, answers the same
 * bits either way and so said nothing.
 *
 * A mask has no signedness to read and answers false. Nothing compares or divides two masks.
 */
bool signedOperand(GlobalBase base, TypePtr type) {
    if(auto lane = vectorLane(base, type)) {
        return !((VectorType*)base[type])->isMask && signedType(base, lane);
    }

    return signedType(base, type);
}

/*
 * Layout, asked of the target rather than read off the type.
 *
 * This file is the resolve-to-lower translation, which is the first point in the pipeline that is
 * allowed to know how wide anything is - see compiler/repr/repr.h for why that line is drawn here.
 * Everything upstream reasons in field indices and constructor names; from here down it is offsets
 * and bytes.
 */
U32 typeSize(LowerContext& lower, TypePtr type) {
    return lower.repr.sizeOf(type);
}

U32 typeAlign(LowerContext& lower, TypePtr type) {
    return lower.repr.alignOf(type);
}

// What indexing homogeneous storage advances by, which is not always the size - see Repr. A run of
// `n` slots is `n` strides, never `n` sizes: the trailing padding of one element is what the next
// one's alignment needs, so measuring in sizes would overlap them.
U32 typeStride(LowerContext& lower, TypePtr type) {
    return lower.repr.strideOf(type);
}

/*
 * Whether one parameter of a lowered signature exists at all.
 *
 * A unit value has no representation anywhere below resolve - lowerType has no answer for one and
 * mapResult never maps one - so a parameter of unit type is neither passed nor received. That is
 * not a corner case reserved for `fn f(x: {})`: a generic function specialized at `{}` grows one
 * wherever its signature named a type variable, which is what a lens whose block produces nothing
 * instantiates its continuation's result with. The caller and the callee have to leave the position
 * out by the same rule, or every argument after it shifts by one.
 *
 * A `&` parameter is the exception, and it is not really one: what travels there is the address of
 * the caller's storage rather than a value, and an address exists whatever it points at.
 */
bool lowerArgExists(GlobalBase global, TypePtr type, bool mutableBorrow) {
    return mutableBorrow || !isUnit(global, type);
}

/*
 * How many bytes one access of this type moves.
 *
 * A scalar is one of the four machine widths, and the assertion is what says so: a type that reaches
 * a load or a store and is not one of them is an aggregate that should have been walked into.
 *
 * A vector is the fifth answer and is not a scalar in that sense - it is 16, 32 or 64 bytes, and it
 * is loaded and stored whole because there is no narrower access to widen from (`validateLoad` says
 * the same thing from the other end). So the width is the type's own size, and the assertion is
 * asked only of the scalars it was written about.
 *
 * A mask answers with the vector's width for the same reason and by the same rule - Design-Vector
 * §2.4's "the memory form is the vector form". Asked of the *kind* rather than through
 * `isVectorType`, which since masks became types of their own answers false for one: a mask local is
 * ordinary storage, so a load or a store of one reaches here exactly as a vector's does.
 */
U32 memoryWidth(LowerContext& lower, TypePtr type) {
    auto size = typeSize(lower, type);
    if(isVectorType(lower.global, type) || isMaskType(lower.global, type)) return size;

    assertTrue(size == 1 || size == 2 || size == 4 || size == 8);
    return size;
}

LowerPtr<LowerValue> immediate(LowerContext& lower, U64 value, LowerType type) {
    auto instruction = new (lower.to.arena) LowerImm(StringId(), type, value);
    lower.constantBlock->addInst(lower.lower, instruction);
    return instruction->created().ptr - lower.lower;
}

// Folds an accumulated constant offset into an address, which is what every projection path comes
// down to once the aggregate structure is gone.
LowerPtr<LowerValue> addOffset(LowerContext& lower, LowerBlock& block, LowerPtr<LowerValue> address, U32 offset) {
    if(!offset) return address;

    auto offsetValue = immediate(lower, offset);
    auto add = binary<LowerInst::Add>(lower.lower, lower.to, block, lower.lower[address], lower.lower[offsetValue], LowerType::Pointer, StringId());
    return add->created().ptr - lower.lower;
}

// The mask that selects `bits` low bits. Kept in one place because the 64-bit case is the one that
// would be undefined if it were written inline as `(1 << bits) - 1`.
U64 lowMask(U32 bits) {
    return bits >= 64 ? maxLimit<U64> : (U64(1) << bits) - 1;
}

/*
 * A primitive integer whose declared width is narrower than the register the lower IR holds it in,
 * so a value of it does not fill its own storage and arithmetic can leave a result the type cannot
 * represent. `U8`/`I8`/`U16`/`I16` in a 32-bit register, and `WideInt` in a 64-bit one.
 *
 * **A primitive only, never a `@bits` refinement.** The language rule is that arithmetic happens at
 * the type's native size and `@bits` describes *storage*, so `x + 1` on a `@bits(3) U32` computes at
 * 32 bits and is masked when it is stored; masking the value as well would change what a comparison
 * of it sees. That distinction is why the test is `registerBits(width)` and not the type's own
 * `naturalStorageBits`: a refinement's own width is exactly what must not be wrapped to here.
 *
 * The `canonical` test is belt and braces rather than a fix for an observed bug. A refinement's
 * arithmetic already never reaches here, because every `@bits` type dispatches to the instances of
 * the type it refines and the resulting instruction is typed at *that* type - `x + 1` above is typed
 * `U32`, which fills its register. Verified by disabling this predicate entirely: the lowering of a
 * `@bits(3) U32` add is byte-identical either way. It is stated in the predicate anyway so that the
 * rule is the code's rather than a consequence of how dispatch happens to type a refinement today.
 *
 * `WideInt` is the case that made this necessary at all: 53 bits *is* its native size, because it is
 * declared as a primitive rather than as `@bits(53) I64`, so its own `Integral` instance is selected
 * and 53 is the width its arithmetic is defined at on every target. Without this it wrapped at 64
 * here and at 53 on JS.
 *
 * The sub-word widths were the same bug found later and from the other side. Nothing narrowed a
 * `U8` result, so `addU8(200, 100)` was 300 here - a value of a type that cannot hold it, in a
 * register whose high bits nothing had cleared - and 44 on JS, which masks every narrow integer at
 * its own width. Widening one to an `Int` afterwards is a `cast` that trusts a register the
 * arithmetic never actually narrowed, so the dirt propagated silently.
 */
bool narrowerThanRegister(LowerContext& lower, TypePtr type) {
    if(!type || lower.global[type]->kind != Type::Int) return false;

    auto integer = (IntType*)lower.global[type];
    if(integer->canonical || integer->width == IntType::Bool) return false;

    // Both sides against this target. A `Size` fills its own register by construction - that is what
    // makes it the word - so this answers false for one on every target, and it says so by asking
    // rather than by an arm that knows the answer.
    auto widths = lower.repr.target.integers;
    return integer->bitsOn(widths) < integer->registerBitsOn(widths);
}

/*
 * Wrapping an arithmetic result back into a type narrower than the register that holds it.
 *
 * Only the operations that can leave the range: `and`, `or`, `xor`, `sar`, division and remainder
 * all map an in-range pair to an in-range result, and masking those would cost an instruction per
 * operation to compute a value that is already correct.
 *
 * **`not` is the bitwise operation that does leave it**, which is what separates it from the three
 * beside it: `and`, `or` and `xor` of two in-range operands cannot set a bit above the width, and a
 * complement sets every one of them. An unsigned narrow value is held zero-extended, so `not` on a
 * `U8` holding 0 gives a register of every bit set where the type's answer is 255 - it is the same
 * escape `neg` and `sub` make, reached by the one operation whose whole business is the high bits. A
 * *signed* narrow value is held sign-extended and its complement already is too, so the wrap does
 * nothing there and the known-bits fold removes it.
 *
 * `shr` is here for a different reason than the arithmetic five, and needs `zeroExtendsShiftOperand`
 * below as well - see its comment. On its own the wrap would be pointless, since a logical shift of
 * an already-masked operand is in range for every distance but zero.
 */
bool wrapsAtDeclaredWidth(LowerContext& lower, TypePtr type, Value::Kind kind) {
    switch(kind) {
        case Value::Add: case Value::Sub: case Value::Mul: case Value::Shl: case Value::Neg:
        case Value::Not: case Value::Shr:
            break;
        default:
            return false;
    }

    return narrowerThanRegister(lower, type);
}

/*
 * Zero-extending the operand of a logical right shift.
 *
 * `shr` is the one operation that reads a narrow value's *storage* rather than its value. A signed
 * type narrower than its register is held sign-extended - that is exactly what `truncateToWidth`
 * leaves behind - so shifting it right logically brings the register's own sign bits down into the
 * answer instead of zeroes. `((0 :: WideInt) - 1) \`shr\` 1` was 2^63-1 here and 2^52-1 on JS, and
 * an `I8` holding -1 would have had the same 24 bits of dirt in front of it.
 *
 * Masking afterwards cannot recover it: by then the bits that should have been zero are part of the
 * result. So the operand is masked first, and the result is re-signed by `truncateToWidth` like any
 * other - which does something only at a shift distance of zero, where the masked value can still
 * have its own sign bit set.
 */
bool zeroExtendsShiftOperand(LowerContext& lower, TypePtr type, Value::Kind kind) {
    return kind == Value::Shr && signedType(lower.global, type) && narrowerThanRegister(lower, type);
}

// The register-relative distance that puts a narrow type's sign bit in the register's, which is what
// makes shifting up and arithmetically back down a truncate and a sign-extend in two instructions.
U32 signShift(LowerContext& lower, TypePtr type) {
    auto integer = (IntType*)lower.global[type];
    auto widths = lower.repr.target.integers;
    return integer->registerBitsOn(widths) - integer->bitsOn(widths);
}

LowerPtr<LowerValue> maskToWidth(LowerContext& lower, LowerBlock& block,
                                        LowerPtr<LowerValue> value, TypePtr type, LowerType lowered) {
    auto integer = (IntType*)lower.global[type];
    auto mask = immediate(lower, lowMask(U32(integer->bitsOn(lower.repr.target.integers))), lowered);
    return binary<LowerInst::And>(lower.lower, lower.to, block, lower.lower[value],
                                  lower.lower[mask], lowered, StringId())->created().ptr - lower.lower;
}

LowerInst* truncateToWidth(LowerContext& lower, LowerBlock& block, LowerInst* result,
                                  TypePtr type, LowerType lowered, StringId name) {
    auto value = result->created().ptr - lower.lower;

    if(!signedType(lower.global, type)) {
        auto integer = (IntType*)lower.global[type];
        auto mask = immediate(lower, lowMask(U32(integer->bitsOn(lower.repr.target.integers))), lowered);
        return binary<LowerInst::And>(lower.lower, lower.to, block, lower.lower[value],
                                      lower.lower[mask], lowered, name);
    }

    auto distance = immediate(lower, signShift(lower, type), lowered);
    auto up = binary<LowerInst::Shl>(lower.lower, lower.to, block, lower.lower[value],
                                     lower.lower[distance], lowered, StringId())->created().ptr - lower.lower;

    return binary<LowerInst::Sar>(lower.lower, lower.to, block, lower.lower[up],
                                  lower.lower[distance], lowered, name);
}

// Between an address and the integer its bits are, which moves nothing: `Cast` is the int/float
// conversion and refuses a pointer on either side, and this is the one that does not - see
// validateCast and validateBitcast.
LowerPtr<LowerValue> reinterpret(LowerContext& lower, LowerBlock& block,
                                        LowerPtr<LowerValue> value, LowerType type) {
    auto instruction = block.addInst(lower.lower, new (lower.to.arena) LowerInstUnary(
        LowerInst::Bitcast, StringId(), type, value));

    return instruction->created().ptr - lower.lower;
}
