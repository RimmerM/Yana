#include "build.h"

/*
 * What a Yana type is on this target.
 *
 * Three questions, and everything else here is one of them: what shape a value has (eachProperty,
 * in build.h, and the newtype rule below), what a fresh one holds (zeroValue), and what it takes to
 * make an independent second one (cloneValue). They have to agree - a record built two ways in two
 * places is a polymorphic call site downstream, and a copy that misses a property is a lost field -
 * so each is written once and read from here.
 */

namespace js {

IntType* intType(Gen& g, TypePtr type) {
    if(!type || g.global[type]->kind != Type::Int) return nullptr;
    return (IntType*)g.global[type];
}

RecordType* recordType(Gen& g, TypePtr type) {
    if(!type || g.global[type]->kind != Type::Record) return nullptr;
    return (RecordType*)g.global[type];
}

/*
 * `Bool` is a host `boolean`, and it is the one place a Core type is recognized by identity here.
 *
 * It is a two-constructor enum, so the general rule below would make it a number - which is what it
 * is on native, where an enum is its discriminant. On JS the host has the niche already, and using
 * it is what the performance contract asks for: `!x` and `a && b` on a `boolean` are what a JS
 * programmer writes, and a number would additionally drag Design.md's `Truth` class into the
 * emitted code's truthiness, which is exactly what that class exists to avoid.
 */
bool isBool(Gen& g, TypePtr type) {
    return type && type == g.program.scalar.bool_;
}

bool isLong(Gen& g, TypePtr type) {
    auto integer = intType(g, type);
    return integer && integer->width == IntType::Long;
}

/*
 * Whether a value of this type is a host object.
 *
 * This is what `isMemoryType` is on native, asked of this target instead - and it is a different
 * question in three places, which is exactly why it is asked here rather than borrowed:
 *
 *  - a *function value* is a host function, not an aggregate: §3.2's `{code, env}` pair became one
 *    closure, so there is nothing to project into and nothing to build;
 *  - a *newtype* is the value it wraps, so one over `Int` is a number however many words native
 *    gives it;
 *  - a value of a type this body cannot see is a reference already, whatever it turns out to be,
 *    which is what the erased convention hands about.
 *
 * Everything that has to decide "is this a reference to an object, or a box standing in for one"
 * asks this: boxing a borrow, boxing across the erased boundary, and reading a place back.
 */
bool isJsObject(Gen& g, TypePtr type) {
    if(!type || !isMemoryType(g.global, type)) return false;
    if(g.global[type]->kind == Type::Fun) return false;

    TypePtr content = nullptr;
    if(isNewtype(g, type, content)) return content && isJsObject(g, content);

    return true;
}

bool isNewtype(Gen& g, TypePtr type, TypePtr& content) {
    auto record = recordType(g, type);
    if(!record || record->layout != RecordType::Single) return false;

    content = record->constructors.isEmpty() ? nullptr : record->constructors.get(g.global, 0).content;
    return !content || g.global[content]->kind != Type::Tup;
}

/*
 * The value a freshly allocated slot of this type holds.
 *
 * Every property a value of the type will ever have is present here, which is the point: §2.3 makes
 * construction order the JS equivalent of field offsets, and a record built two ways in two places
 * is a polymorphic call site downstream. Filling the slot afterwards writes properties that already
 * exist rather than adding them.
 */
JsPtr<Expr> zeroValue(Gen& g, TypePtr type) {
    if(!type || isUnit(g.global, type)) return nullValue(g);

    auto value = g.global[type];

    switch(value->kind) {
        case Type::Int: {
            auto integer = (IntType*)value;
            if(integer->width == IntType::Bool) return boolean(g, false);
            if(integer->width == IntType::Long) return bigInt(g, 0, integer->isSigned);
            return number(g, 0);
        }
        case Type::Float:
            return number(g, 0);
        case Type::Ptr:
        case Type::Borrow:
            return nullValue(g);
        case Type::Record:
            if(isBool(g, type)) return boolean(g, false);

            // An enum is its discriminant and nothing else, so it is a number here exactly as it is
            // a machine word on native.
            if(((RecordType*)value)->layout == RecordType::Enum) return number(g, 0);
            break;
        case Type::Fun:
            // A function value is a host function, and a slot that has not been given one holds
            // nothing rather than an object with two empty words - see genFunValueWord.
            return nullValue(g);
        case Type::Tup:
            break;
        case Type::Gen:
            /*
             * Storage for a value whose shape this body cannot see - Analysis-JS.md §3.4's target
             * split, from the side that needs it.
             *
             * An empty object rather than a null, and that is the whole of what an erased body needs
             * to create one: the properties come from whatever fills it, and what fills it is the
             * descriptor's `moveInit`, which writes the type's own properties in the type's own
             * construction order. So the object this ends up as is the one a concrete zeroValue
             * would have built - the single hidden class of §2.3 - and for a type whose JS form is
             * not an object at all, the one `$v` its relocation writes is exactly the box the erased
             * convention passes about.
             */
            return asExpr(g, make<ObjectExpr>(g));
        default:
            return nullValue(g);
    }

    TypePtr content = nullptr;
    if(isNewtype(g, type, content)) return zeroValue(g, content);

    auto object = make<ObjectExpr>(g);
    eachProperty(g, type, [&](Name key, TypePtr member) {
        object->properties.push(g.file.arena, Property { key, zeroValue(g, member) });
    });

    return asExpr(g, object);
}

JsPtr<Expr> boxOf(Gen& g, JsPtr<Expr> value) {
    auto object = make<ObjectExpr>(g);
    object->properties.push(g.file.arena, Property { g.boxField, value });
    return asExpr(g, object);
}

/*
 * The integer tower - Analysis-JS.md §2.1.
 *
 * `Int` is a wrapping 32-bit integer on both targets, and JS has no 32-bit integer, so every
 * arithmetic result is coerced back into range. §2.1 recommends exactly this and notes that the
 * range analysis `@bits` already needs would elide most of them; none of that analysis exists yet,
 * so every one is emitted. That is the asm.js tax, stated where it is paid.
 */
JsPtr<Expr> coerce(Gen& g, TypePtr type, JsPtr<Expr> value) {
    if(auto integer = intType(g, type)) {
        if(integer->width == IntType::Bool) return value;

        if(integer->width == IntType::Long) {
            return hostCall(g, "BigInt"_v, integer->isSigned ? "asIntN"_v : "asUintN"_v,
                            number(g, 64), value);
        }

        auto bits = integer->bits;
        if(bits >= 32) {
            return binary(g, integer->isSigned ? BinaryOp::Or : BinaryOp::Shr, value, number(g, 0));
        }

        if(integer->isSigned) {
            // Sign extension is what a narrow load does on native, and shifting up and back is how
            // JS spells it.
            auto shift = number(g, 32 - bits);
            return binary(g, BinaryOp::Sar, binary(g, BinaryOp::Shl, value, shift), shift);
        }

        return binary(g, BinaryOp::And, value, number(g, F64((U64(1) << bits) - 1)));
    }

    if(type && g.global[type]->kind == Type::Float &&
       ((FloatType*)g.global[type])->width == FloatType::Float) {
        return hostCall(g, "Math"_v, "fround"_v, value);
    }

    return value;
}

/*
 * Copying - the one ownership operation that costs anything here (§2.5).
 *
 * A structural duplicate, property by property, rather than a call into a runtime cloner: the shape
 * is known at compile time and an object literal listing every property in construction order is
 * both the fastest form and the one that keeps the type's hidden class.
 */
JsPtr<Expr> cloneValue(Gen& g, TypePtr type, JsPtr<Expr> source, LocationId where) {
    // Anything that is not an object here is duplicated by being read - a number, a bigint, a
    // boolean, and a function value, whose copy is the same closure over the same environment
    // exactly as native's copy is the same two words over the same storage.
    if(!isJsObject(g, type)) return source;

    if(g.global[type]->kind == Type::Gen) {
        g.context.diagnostics.error("the JS target cannot copy a value of a type it cannot see the shape of"_v, where);
        return source;
    }

    // A newtype is the value it wraps, so there is nothing structural to duplicate.
    TypePtr content = nullptr;
    if(isNewtype(g, type, content)) return cloneValue(g, content, source, where);

    /*
     * A sum copies every property of the flattened union, not only the live constructor's. Which
     * one is live is a run-time fact and the shape is not, so copying all of them keeps this one
     * object literal instead of a switch over the tag - and the properties belonging to the other
     * constructors hold their zero values, which copy to the same zero values.
     */
    auto object = make<ObjectExpr>(g);
    eachProperty(g, type, [&](Name key, TypePtr member) {
        object->properties.push(g.file.arena, Property {
            key, cloneValue(g, member, field(g, source, key), where)
        });
    });

    return asExpr(g, object);
}

/*
 * A block copy of one aggregate over another - what the compiler-generated relocation glue opens
 * with before it runs the members' own `Sink`s.
 *
 * `copyMemory` names bytes, and this target has none, so what is recovered is the *shape*: the glue
 * casts two same-typed references to `%()` and copies exactly one value's worth, so the pointee
 * before the cast says what the bytes were. Anything else - a partial copy, two different shapes,
 * a count that is not one value - is `Native` proper and is excluded from this target rather than
 * approximated.
 */
TypePtr blockCopyShape(Gen& g, InstNative& instruction) {
    if(instruction.op != NativeOp::CopyMemory || instruction.args.size() != 3) return nullptr;

    auto traced = [&](ModulePtr<Value> pointer) -> TypePtr {
        auto& value = *g.local[pointer];
        auto source = value.kind == Value::Cast ? g.local[((InstUnary&)value).from]->type : value.type;

        return pointeeType(g.global, source);
    };

    auto args = instruction.args;
    auto to = traced(args.get(g.local, 0));
    auto from = traced(args.get(g.local, 1));
    auto count = g.local[args.get(g.local, 2)];

    /*
     * A shape whose JS form is not an object is still one this target can move: the reference to it
     * is the box that stands in for one, so the copy is that box's single property. That case is
     * `moveInit$Int` and its neighbours - the descriptor slot an erased relocation calls - so
     * excluding it would leave a null in every scalar type's descriptor.
     */
    if(!to || to != from || isUnit(g.global, to)) return nullptr;
    if(count->kind != Value::ConstInt || ((ConstInt*)count)->value != typeSize(g.global, to)) return nullptr;

    return to;
}

} // namespace js
