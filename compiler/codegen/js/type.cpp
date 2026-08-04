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
 * `Bool` is 0 or 1, and it is the one place a Core type is recognized by identity here.
 *
 * It was a host `boolean` until whole-record scalarization needed a reference into a bit range. A
 * reference like that carries a shift and reads and writes through a mask, and it has to do so
 * *uniformly* - one compiled `flip(&Bool)` serves a bit of a scalarized record, a co-packed field and
 * a whole local, because the callee has only the pointee type. The read-modify-write that entails is
 * the identity on 0 or 1 and turns `true` into `1`, so the two representations cannot coexist behind
 * one reference and the number is the one that composes.
 *
 * Measured before choosing, since the old comment here claimed the boolean was what the performance
 * contract asked for: 0/1 is 105% on a field read and branch, 107% on `&&`, 108% on negation, 169%
 * on comparing two of them, identical in bytes per record, and it makes `[Bool]` a `Uint8Array` at
 * one byte per element against eight. It is a strict improvement rather than a concession.
 *
 * Design.md's `Truth` class is unaffected - the compiler emits `if` only on `Bool`-typed values, and
 * 0 and 1 have exactly the truthiness they need. What does change is the host boundary, where a
 * `Bool` becomes `!== 0` outbound and `? 1 : 0` inbound; see Analysis-JS-Interop.md.
 */
bool isBool(Gen& g, TypePtr type) {
    return type && type == g.program.scalar.bool_;
}

/*
 * Which of the two integer representations a type wider than 32 bits has, and why the question is
 * asked of its *canonical* form rather than of itself.
 *
 * `IntType::Long` used to be the whole answer, because everything above 32 bits was a `bigint`. It
 * is now split at `kMaxNumberBits`: at 53 bits and below a host `number` holds every value exactly,
 * so such a type is a `number` and wide.cpp supplies the operators JS stops having above 32.
 *
 * The canonical width decides it because **a `@bits` refinement dispatches to the instances of the
 * type it refines**. `a and b` on a `@bits(40) U64` resolves to `Integral(U64)`, so the arguments
 * are converted to `U64`, the operation happens at 64 bits, and the result is narrowed back. If the
 * refinement were a `number` while `U64` stayed a `bigint`, each of those conversions would be a
 * real `BigInt()`/`Number()` round trip and every operation on the refinement would get *slower*
 * than it is today. Keyed on the canonical form instead, a refinement and what it refines are
 * always the same host type and every conversion between them stays free.
 *
 * So a `number` of 33 to 53 bits is reached by refining a type that is already one - `WideInt` and
 * anything built on it - and never by refining `U64`. That is the whole reason `WideInt` is a
 * primitive with its own instances rather than an alias for `@bits(53) I64`: an alias would have
 * had U64's arithmetic and U64's representation, which is the thing being avoided.
 *
 * `resolveBitsType` recomputes the width class from the refined count, so `width == Long` is exactly
 * `bits > 32`, and these two predicates partition that band with nothing in between.
 */
static IntType* canonicalInt(Gen& g, TypePtr type) {
    auto integer = intType(g, type);
    if(!integer || integer->width != IntType::Long) return nullptr;

    return (IntType*)g.global[canonicalType(g.global, type)];
}

bool isLong(Gen& g, TypePtr type) {
    auto canonical = canonicalInt(g, type);
    return canonical && canonical->bits > kMaxNumberBits;
}

bool isWideNumber(Gen& g, TypePtr type) {
    auto canonical = canonicalInt(g, type);
    return canonical && canonical->bits <= kMaxNumberBits;
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
 *
 * The last question is asked of the **Repr rather than of the type**, which is
 * [Implementation-JS-Repr.md part 5](../../../Implementation-JS-Repr.md) and the reason the three
 * representation features do not each need their own version of it. A record whose Repr is one
 * scalar is a `number` here, with no allocation, no property to project into and no hidden class -
 * and answering that from the layout rather than from `isMemoryType` is what propagates the fact to
 * construction, field access, copy, equality and array element type at once. Nothing moves until a
 * target sets `scalarizeRecords`, because until then no aggregate has a scalar Repr.
 */
bool isJsObject(Gen& g, TypePtr type) {
    if(!type || !isMemoryType(g.global, type)) return false;
    if(g.global[type]->kind == Type::Fun) return false;

    /*
     * A `String` is the host `string` *primitive* here, not an object -
     * Implementation-String.md part 2's "zero wrapper".
     *
     * It reaches this function as a memory type, because `isDirectType` is deliberately
     * target-independent and a native string is two words. That is the right answer for the
     * calling convention and the wrong one for this question, which is about what the host value
     * *is*: strings are immutable primitives there, so writing through a `&String` has to replace
     * the binding rather than write a property of something that stays the same object. Exactly the
     * reasoning the niche-folded case below gives, and exactly the reason `Type::Fun` is on the line
     * above.
     *
     * Without this, `fn pushString(&self: String, other: String)` compiled to a function whose body
     * was `return;` - the assignment wrote a local nothing read again, and the append was silently
     * lost. `String.yana`'s `appendToLiteral` is what catches it, and it catches it by a number
     * rather than by a diff for exactly that reason: the emitted JavaScript reads as if it works.
     */
    if(g.global[type]->kind == Type::String) return false;

    TypePtr content = nullptr;
    if(isNewtype(g, type, content)) {
        /*
         * A wrapper over a raw pointer is a host reference, which is an object - `hostArray()` is
         * `[]`, and the box the erased ABI hands about is `{$v}`. Answered here rather than by
         * `isJsObject` of the pointer itself, which is the *reference* question and has to keep its
         * own answer: `%T` against `&T` is what tells a storage handle from a borrow, and both sides
         * of a call read it off the declaration.
         */
        if(content && g.global[content]->kind == Type::Ptr) return true;

        return content && isJsObject(g, content);
    }

    // A generic body has no layout to consult and treats every opaque value as a reference, which is
    // what the erased convention hands it. `of` answers an empty Repr for one, so this asks `opaque`
    // rather than reading `scalarBits` out of it.
    auto& repr = g.repr.of(type);
    if(repr.opaque) return true;

    /*
     * A niche-folded record is its payload *and* the pattern that says it has none, so it is not a
     * host object even where the payload is one: `Maybe(Person)` is `Person | null`.
     *
     * What that decides is how a reference to one is carried. An object is borrowed as itself, which
     * works because writing through the reference writes properties of an object that stays the same
     * object - and a `&Maybe(Person)` has to be able to make the binding `null`, which is replacing
     * the value rather than writing into it. So it takes the box every non-object takes.
     */
    if(repr.isNicheFolded()) return false;

    return repr.scalarBits == 0;
}

/*
 * Which property one field of an object-shaped tuple lives in, and where inside it.
 *
 * Two answers, and the second is what co-packing added: a field that owns its property is reached by
 * name and read as a whole, and one that shares a word with its neighbours is a bit range of a
 * property named after the word. Everything that has to know - the property list, the place walk, a
 * copy, and the key a reference to the field carries - asks this, because a packed field no longer
 * has a name of its own for them to agree by.
 *
 * Only meaningful for a tuple that is still an object. A tuple the Repr made one scalar has bit
 * ranges too, but there is no property anywhere in it: the walk stops at the number and accumulates
 * offsets, which it does before ever reaching here.
 */
FieldProperty fieldProperty(Gen& g, TypePtr type, U16 index) {
    auto entry = ((TupType*)g.global[type])->fields.get(g.global, index);

    if(auto placed = g.repr.fieldOf(type, index)) {
        if(placed->isPacked()) {
            FieldProperty property;
            property.name = packedWordName(g, placed->offset);
            property.type = g.program.scalar.int_;
            property.bitOffset = placed->bitOffset;
            property.bitWidth = placed->bitWidth;
            property.wordBits = U8(min(U32(placed->wordBytes) * 8, U32(kMaxNumberBits)));
            property.leader = placed->bitOffset == 0;
            return property;
        }
    }

    FieldProperty property;
    property.name = fieldName(g, entry.name, index);
    property.type = entry.type;
    return property;
}

/*
 * A tuple with one field, which on this target is that field.
 *
 * The reason it is here and not in `compiler/repr` is that it is only true where an aggregate has no
 * layout: natively `{items: %a}` is one word at offset zero of a record, and the record *is* the
 * word already - there is nothing wrapping anything. Here the same declaration is an object with one
 * property, so the wrapper is real, costs an allocation and a hidden class per value, and shows up in
 * exactly the place Analysis-JS.md's contract is about - `data Array(a) {items: %a}` made every array
 * `{items: hostArray}` rather than the host array.
 *
 * Four exclusions, each of which is a case where the field is not the whole of the value:
 *
 *  - a scalarized tuple, which is a `number` and has no property to remove;
 *  - a `@box`ed field, where the wrapper is the indirection rather than the value behind it;
 *  - a unit field, which contributes no property at all - so the object is already empty and the
 *    field has no value for the wrapper to *be*;
 *  - a type whose **address is taken through a projection**, which then has no slot to name - see
 *    `Gen::opaqueTuples`.
 *
 * ## Why this is general, and was not
 *
 * It was restricted to a *raw pointer* field, on the argument that this is the case where the field
 * is already a host reference - so removing the wrapper removes an object and changes nothing else
 * about what the value is. The general form was measured then at twenty-five failing fixtures.
 *
 * Re-measured after B, I and J it was two, and with the two whole-program exclusions above it is
 * none. Most of what stood in the way was never about wrappers: it was the reference forms B settled
 * and the ownership instructions J discharged. See Implementation-Simplification.md §18 and §20.
 */
static bool isTransparentTuple(Gen& g, TypePtr type, TypePtr& content) {
    if(!type || g.global[type]->kind != Type::Tup) return false;

    auto& tuple = *(TupType*)g.global[type];
    if(tuple.fields.size() != 1) return false;

    // Read off the Repr rather than through `isJsObject`, which asks this question on the way to
    // answering its own.
    auto& repr = g.repr.of(type);
    if(repr.opaque || repr.scalarBits != 0) return false;

    // The whole-program answer, keyed on the *tuple* rather than on whatever holds it - see
    // Gen::opaqueTuples for why that distinction is the one that goes silently wrong.
    if(g.opaqueTuples.contains(U32(type))) return false;

    auto field = tuple.fields.get(g.global, 0);
    if(field.boxed || !field.type || isUnit(g.global, field.type)) return false;

    content = field.type;
    return true;
}

/*
 * Whether a constructor's payload is one property of the sum's object rather than its own fields.
 *
 * Three shapes, and each of them is one *value* however it was declared, so each is one property:
 *
 *  - a payload that is not a tuple, which has no field names to flatten;
 *  - a tuple the Repr made a single number, which has nothing to flatten *into*;
 *  - a tuple that **is** its one field, which is the case this was missing. Transparency says the
 *    tuple has no object of its own, so there are no properties of it to spread - and flattening it
 *    anyway named a property nothing ever wrote. `Either = Plain {n} | Held {h: Handle}` built its
 *    `Held` arm as a bare `Handle`: the payload write went to the record itself, so the value had no
 *    `$tag` and every match on it read the wrong arm.
 *
 * The third case is why this is a function rather than a condition written twice. `isJsObject` of a
 * transparent tuple answers about the *field* - which is the right answer to "is this value an
 * object" and the wrong one to "does this value have properties of its own" - so the two readers
 * asking it directly disagreed exactly where those two questions come apart.
 */
bool payloadIsOneProperty(Gen& g, TypePtr content) {
    if(!content || g.global[content]->kind != Type::Tup) return true;

    TypePtr inner = nullptr;
    if(isNewtype(g, content, inner)) return true;

    return !isJsObject(g, content);
}

/*
 * The one-field tuple whose transparency is why this type has no object of its own, or null where
 * there is no such tuple.
 *
 * `isNewtype` asks the same walk in the other direction: it answers *what* a value is, and this
 * answers *which decision made it that*, which is the thing an exclusion has to be able to name.
 * One level deep, because that is as far as `isNewtype` looks - a record is its single
 * constructor's content, and that content either is the tuple or is not.
 */
TypePtr transparentTupleOf(Gen& g, TypePtr type) {
    TypePtr content = nullptr;
    if(isTransparentTuple(g, type, content)) return type;

    auto record = recordType(g, type);
    if(!record || record->layout != RecordType::Single || record->constructors.isEmpty()) {
        return nullptr;
    }

    auto inner = record->constructors.get(g.global, 0).content;
    return isTransparentTuple(g, inner, content) ? inner : nullptr;
}

bool isNewtype(Gen& g, TypePtr type, TypePtr& content) {
    if(isTransparentTuple(g, type, content)) return true;

    auto record = recordType(g, type);
    if(!record || record->layout != RecordType::Single) return false;

    content = record->constructors.isEmpty() ? nullptr : record->constructors.get(g.global, 0).content;
    return !content || g.global[content]->kind != Type::Tup ||
           isTransparentTuple(g, content, content);
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
            if(integer->width == IntType::Bool) return number(g, 0);
            if(isLong(g, type)) return bigInt(g, 0, integer->isSigned);

            // Including the 33-to-53-bit band, whose zero is an ordinary `0` - the representation
            // is a `number` and only the operators differ.
            return number(g, 0);
        }
        case Type::Float:
            return number(g, 0);
        case Type::Ptr:
        case Type::Borrow:
            return nullValue(g);
        case Type::Record:
            if(isBool(g, type)) return number(g, 0);

            // An enum is its discriminant and nothing else, so it is a number here exactly as it is
            // a machine word on native - and so is a sum whose payloads all substituted to unit.
            if(discriminantOnly(g.global, *(RecordType*)value)) return number(g, 0);
            break;
        case Type::Fun:
            // A function value is a host function, and a slot that has not been given one holds
            // nothing rather than an object with two empty words - see genFunValueWord.
            return nullValue(g);
        case Type::Tup:
            break;
        case Type::Array: {
            /*
             * `[T *n]` is a host array of `n` zeroes - Implementation-Containers.md §6 and §14.
             *
             * Written out rather than left to the default, because a fixed array's whole promise is
             * that it holds exactly `n` elements: an empty array here would be a container whose
             * length is a lie on this target alone, and every read of it would answer `undefined`.
             *
             * What is *not* here is element access. §6's elements are reached the way a run's slots
             * are - a base address plus a scaled index - and this target has no addresses, so a
             * fixed array is written and copied here and cannot yet be indexed. That is the same gap
             * `Run(a)` reports at its allocation, and it closes in the same change: §14's host-call
             * node, which is where a container stops being storage and starts being a host value.
             */
            auto array = (ArrayType*)value;
            auto elements = make<ArrayExpr>(g);

            for(U32 i = 0; i < array->length; i++) {
                elements->values.push(g.file.arena, zeroValue(g, array->content));
            }

            return asExpr(g, elements);
        }
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

    /*
     * A niche-folded record, which is its payload plus one pattern taken out of it. A fresh one is
     * whatever the zero of that representation decodes as, which is the same answer native gets from
     * zeroing the storage: `null` where the pattern is the host's absent value, and the payload's own
     * zero where it is a number, since every pattern niche this target folds into starts its valid
     * range at zero and therefore reads back as the payload constructor.
     */
    if(g.repr.of(type).isNicheFolded()) {
        return g.repr.of(type).encoding.niche.isAbsent() ? nullValue(g) : number(g, 0);
    }

    // A record the Repr made one scalar has no properties to pre-create - it is a `number`, and a
    // fresh one is zero. Every field of it is a bit range of that number, so the writes that fill it
    // are the read-modify-writes `encodePackedField` is on native rather than property assignments,
    // and they preserve the bits they do not own by construction.
    if(!isJsObject(g, type)) return number(g, 0);

    auto object = make<ObjectExpr>(g);
    eachProperty(g, type, [&](Name key, TypePtr member) {
        object->properties.push(g.file.arena, Property { key, zeroValue(g, member) });
    });

    return asExpr(g, object);
}

/*
 * What an *allocation* of this type starts as, which is not always its zero.
 *
 * The two differ for exactly one shape, and it is the shape that has no storage to be zeroed: a
 * niche-folded record whose payload is a host object. Its zero is `null` - that is what the absent
 * pattern decodes as, and what a nested field of one should hold - but a construction fills such a
 * record by *writing the payload's properties*, and `null` has none. Natively the same construction
 * writes into zeroed bytes, which are there whichever constructor ends up in them.
 *
 * So a fresh one is the payload's own zero, and the absent reading is still reachable because it is
 * never written into the value: encoding the tag of a payload-free constructor replaces the whole
 * binding with `null`. `Nothing` is `null`, `Just` has an object to be built in, and a `Maybe(Point)`
 * sitting in a *field* still starts as `null` because that goes through zeroValue.
 *
 * A boxed payload is excluded: it is one reference, so there are no properties to pre-create and the
 * construction writes the reference itself.
 */
JsPtr<Expr> freshStorage(Gen& g, TypePtr type) {
    if(!type || !isMemoryType(g.global, type) || g.global[type]->kind != Type::Record) {
        return zeroValue(g, type);
    }

    auto& repr = g.repr.of(type);
    if(!repr.isNicheFolded() || !repr.encoding.niche.isAbsent()) return zeroValue(g, type);

    auto& record = *(RecordType*)g.global[type];
    if(repr.encoding.payloadConstructor >= record.constructors.size()) return zeroValue(g, type);

    auto payload = record.constructors.get(g.global, repr.encoding.payloadConstructor);
    if(!payload.content || payload.boxed || !isJsObject(g, payload.content)) {
        return zeroValue(g, type);
    }

    return zeroValue(g, payload.content);
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

        /*
         * The declared width rather than 64.
         *
         * This used to mask every `Long`-class type to 64 bits whatever its `@bits` said, so
         * `@bits(58) U64` wrapped at 58 on native and at 64 here - a silent cross-target semantic
         * difference rather than a missing optimization. `bits` is the unrefined type's own width
         * when nothing refined it, so the common case emits the same text it always did.
         */
        if(isLong(g, type)) {
            return hostCall(g, "BigInt"_v, integer->isSigned ? "asIntN"_v : "asUintN"_v,
                            number(g, integer->bits), value);
        }

        /*
         * The general reduction for a 33-to-53-bit value, and the one wide.cpp's callers mostly
         * avoid: every arithmetic operation there knows how far out of range its own result can be
         * and wraps more cheaply than this can. What reaches here is a cast from something wider,
         * where nothing is known about the input at all.
         */
        if(isWideNumber(g, type)) return wideCall(g, WideOp::Wrap, integer, value, nullptr);

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
    /*
     * A niche-folded record is not an object, but it may still *hold* one - `Maybe(Person)` is a
     * `Person` or a `null` - so it is asked before the shortcut below rather than falling into it.
     * Duplicating it is duplicating the payload where there is one, and the test for that is the same
     * comparison the tag decode is.
     */
    if(auto payload = foldedPayload(g, type)) {
        if(!isJsObject(g, payload)) return source;

        return ternary(g, binary(g, BinaryOp::Eq, source, nullValue(g)), nullValue(g),
                       cloneValue(g, payload, source, where));
    }

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

    /*
     * The count, which is "how wide is `to`" in either of the two forms that question now has.
     *
     * The unfolded one is the question itself, and matching it rather than recomputing an answer is
     * what made this independent of what any Repr said. `compiler/opt` now folds a concrete metric
     * (see `foldMetric` in opt_fold.cpp), so the same count also arrives as a number - and the
     * number is trustworthy for the reason the metric was: it was produced by a `ReprTable` built
     * for *this* target, on a program `@platform` already split per target, so it is this file's own
     * Repr answer one stage earlier rather than a resolution-time guess about it.
     *
     * Both forms are accepted rather than one, because the optimizer is switchable: `-no-opt` and
     * the fixture runner's equivalence check compile the same program with the fold off, and a
     * recognizer that only knew the folded form would exclude this function from that half of the
     * comparison.
     */
    if(count->kind == Value::TypeMetric) {
        auto& metric = *(const InstTypeMetric*)count;
        if(metric.metric != TypeMetricKind::Size || metric.of != to) return nullptr;

        return to;
    }

    if(count->kind == Value::ConstInt && !isGeneric(g.global, to)) {
        if(((ConstInt*)count)->value != U64(g.repr.of(to).size)) return nullptr;

        return to;
    }

    return nullptr;
}

} // namespace js
