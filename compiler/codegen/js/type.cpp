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
 * A test, as the value a `Bool` is here.
 *
 * The comment above says a `Bool` is the number 0 or 1 and gives the measurements for it, and every
 * consumer of one is written to that: a `Bool` reaching arithmetic is widened by `genCast`'s ternary,
 * a `[Bool]` is a `Uint8Array`, and a bit of a packed word is stored as a number. A *comparison*,
 * though, is a host boolean - JS has no other answer for `===` - so an instruction that produces a
 * `Bool` from one has to say which of the two representations it means.
 *
 * It matters in exactly one place, and it is not a place anything is free to avoid: `===`. Every
 * other consumer reads only truthiness, where `true` and `1` agree, so the two forms coexisted for
 * as long as no program compared two `Bool`s - and `if truthy(x) == True` then answered *false*,
 * because `(x !== 0) === 1` is `true === 1`. Widening here rather than at the comparison is what
 * keeps that from being a rule every future consumer has to know: there is one representation, and
 * the two instructions that could produce the other one do not.
 *
 * Free in the emitted text wherever it is redundant, and the two rules that make it so are rules
 * about *positions*: `simplifyCondition` takes it off a condition, which is the shape it names and
 * the reason it names it, and `foldBitwiseOperand` takes it off an operand of a bitwise operator,
 * where `ToInt32` reads the boolean as the same two numbers. What is left is a `Bool` that is
 * genuinely a value - returned, stored, passed, or compared - and there it is the representation.
 */
JsPtr<Expr> boolNumber(Gen& g, JsPtr<Expr> test) {
    return ternary(g, test, number(g, 1), number(g, 0));
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

    // `Width::Word` is deliberately not admitted here. A `Size` is a signed 32-bit index on this
    // target - that is what `IntWidths` says - so it is an ordinary `number` and neither of the two
    // wide representations, and the class test is the honest way to say so.
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

    /*
     * A `String` is the host `string` *primitive* here, not an object -
     * Implementation-String.md part 2's "zero wrapper".
     *
     * It reaches this function as a memory type, because `isDirectType` is deliberately
     * target-independent and a native string is two words. That is the right answer for the
     * calling convention and the wrong one for this question, which is about what the host value
     * *is*: strings are immutable primitives there, so writing through a `&String` has to replace
     * the binding rather than write a property of something that stays the same object. Exactly the
     * reasoning the niche-folded case below gives.
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

    /*
     * A field elided onto a property the host value already has - `arr.length`, see
     * `isHostProperty`.
     *
     * Ahead of the Repr, and that is the point rather than an ordering convenience: what this
     * property is called, how wide it is and whether it shares a word with anything are the host's
     * answers and not this compiler's. A layout that co-packed a count into `$p0` beside a neighbour
     * would be describing a word that does not exist, and one that minified the name would be
     * reading a property nothing ever wrote.
     */
    if(isHostProperty(g, type, index)) {
        FieldProperty property;
        property.name = fieldName(g, entry.name, index);
        property.type = entry.type;
        return property;
    }

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

    /*
     * A function value is two properties, not one holding an object -
     * Implementation-JS-Closure.md part 2.2.
     *
     * The two words are inline at the field's offset on native, and this is the same statement in
     * this target's terms: `run$c` and `run$e` beside the record's other properties, rather than a
     * nested `{$c, $e}` that has to be allocated when the record is built and dereferenced whenever
     * it is called. Named after the field for the same reason a flattened parameter's parts are -
     * see partName - so the emitted source still says which field they belong to.
     */
    if(entry.type && g.global[entry.type]->kind == Type::Fun) {
        property.envName = fieldPartName(g, property.name, "$e"_v);
        property.name = fieldPartName(g, property.name, "$c"_v);
        property.fun = true;
    }

    return property;
}

/*
 * Whether this tuple's fields after the first are properties the host value already has - `@host`,
 * `Field::host`, and Implementation-Containers.md §14's elision.
 *
 * Two halves, and only the second is this target's:
 *
 *  - the *declaration* says the fields may be elided, which resolve has already checked the shape of
 *    - one stored field at index zero, every field after it named and `@host`;
 *  - `hostPropertiesElided` says whether the claim holds for the value field zero actually holds,
 *    which is a question about the host and lives beside the rule that answers it.
 *
 * The second is why this is asked of the *instantiation* rather than decided once per declaration.
 * `Array(Handle)` holds a plain host array whose `length` is its occupancy, so the record is that
 * array and nothing else; `Array(Int)` holds an `Int32Array` whose `length` is its capacity, so the
 * same declaration keeps its stored count and its object. One declaration, two layouts, and which
 * one a value has is settled by the same predicate that chose which array to allocate.
 */
bool hostFieldsElided(Gen& g, TypePtr type) {
    if(!type || g.global[type]->kind != Type::Tup) return false;

    auto& tuple = *(TupType*)g.global[type];
    if(tuple.fields.size() < 2) return false;

    // The flag on field one first, because it is one bool and it is what every *other* tuple in the
    // program fails on - this is asked at each field access and at each step of a place walk, so
    // what it costs on a record that has never heard of the host is what it costs.
    for(Size i = 1; i < tuple.fields.size(); i++) {
        if(!tuple.fields.get(g.global, i).host) return false;
    }

    auto stored = tuple.fields.get(g.global, 0);
    return !stored.host && hostPropertiesElided(g.global, stored.type);
}

/*
 * Whether one field of a tuple is reached as a property of the value rather than of an object
 * holding it.
 *
 * The reader's half of the rule above, and it has to carry the whole of it rather than the flag
 * alone: the flag is on the declaration and is therefore present on both rows, so a `fieldProperty`
 * or a place walk that read only the flag would elide `Array(Int)`'s count as well and hand back an
 * `Int32Array`'s capacity where the program asked how many elements it holds.
 */
bool isHostProperty(Gen& g, TypePtr type, U16 index) {
    if(!index || !type || g.global[type]->kind != Type::Tup) return false;
    if(!((TupType*)g.global[type])->fields.get(g.global, index).host) return false;

    return hostFieldsElided(g, type);
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
 * A tuple whose fields after the first are `@host` is the same statement one step further out - see
 * `hostFieldsElided`. There the wrapper is not merely an object around one value, it is an object
 * around one value and a *copy of a number that value already keeps*, which is why the elision is
 * worth having twice over: `Array(Handle)` is the host array again, and `remove` closing the gap and
 * writing the count is `copyWithin` plus the length assignment that truncates.
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
    if(tuple.fields.size() != 1 && !hostFieldsElided(g, type)) return false;

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
 * Whether an expression is a value rather than a fresh object - a literal of one of the host's
 * primitive kinds.
 *
 * The one thing it decides is whether `fill` may place it in every slot of a row. `fill` puts *one*
 * value in each, so a row of a primitive is right and a row of an object literal would be `n`
 * aliases of one object, where the written form gives each slot its own.
 */
static bool isPrimitiveValue(Gen& g, JsPtr<Expr> value) {
    switch(g.base[value]->kind) {
        case Expr::Number:
        case Expr::BigInt:
        case Expr::String:
        case Expr::Bool:
        case Expr::Null:
        case Expr::Undefined:
            return true;
        default:
            return false;
    }
}

/*
 * A fresh `[T *n]` whose count this body cannot see - Implementation-Const-Generics.md §3.2 on this
 * target, and the erased half of the arm below.
 *
 * The written form spells out `n` zeroes, and there is nothing to spell out when `n` is a cell of the
 * caller's environment. The host has the two constructions that say the same thing at run time:
 *
 *   - **A typed row from a length.** `new Uint8Array(n)` is `n` zero bytes by the host's own
 *     specification, so the erased form is not merely available here - it is *shorter* than the
 *     concrete one, which builds a literal and copies it. Every numeric element takes this path,
 *     which is every fixed array a digest, a hash or a word transfer is made of.
 *   - **`new Array(n).fill(z)`** for the rest, where `z` is a value rather than an object - `Bool`'s
 *     zero, a `bigint`, a null reference, a niche-folded record's absent pattern.
 *
 * An element whose zero is an *object* has neither: `fill` would make the row `n` aliases of one
 * object, and the alternative needs a callback this tree has no node for. That is reported at the
 * allocation rather than guessed at here - see the Alloc arm of genInstruction, which is where the
 * `Run(a)` gap next to it is reported and is the one place with a source location to point at.
 */
JsPtr<Expr> erasedZeroArray(Gen& g, ArrayType& array) {
    auto count = genConstValue(g, array.count);
    if(!count) return nullValue(g);

    if(typedArrayFor(g.global, array.content).length) {
        return hostArrayForElement(g, array.content, count);
    }

    auto zero = zeroValue(g, array.content);
    if(!isPrimitiveValue(g, zero)) return nullValue(g);

    auto row = make<CallExpr>(g, variable(g, literalName(g, "Array"_v)));
    row->args.push(g.file.arena, count);
    row->construct = true;

    return call(g, field(g, asExpr(g, row), literalName(g, "fill"_v)), zero);
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
            // Two null words, through eachProperty at the end like any other shape. A slot that has
            // not been given a function value holds the same two properties every function value of
            // that type has, which is what keeps them to one hidden class.
            break;
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
            auto written = writtenCount(g.global, array->count);

            // A count this body cannot see - see erasedZeroArray, which is the same statement made
            // at run time. `writtenCount` rather than `constValue` because the second one *asserts*
            // that the count is a number: with assertions off it read a `GenType` as a `ConstType`
            // and the loop below ran to a garbage bound, which is how this arrived.
            if(!written) return erasedZeroArray(g, *array);

            auto elements = make<ArrayExpr>(g);

            for(U64 i = 0; i < written.unwrap(); i++) {
                elements->values.push(g.file.arena, zeroValue(g, array->content));
            }

            // And in §14's typed row where the element has one, on the same terms `Array(a)`'s own
            // storage is: a `[U8 *64]` is a `Uint8Array`, which is what lets a word transfer reach
            // its buffer (see NativeOp::HostWordRead) and is what the row was for in the first
            // place. See hostArrayForElement, which is the one rule all three builders ask.
            return hostArrayForElement(g, array->content, asExpr(g, elements));
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
        case Type::Vector: {
            /*
             * A vector is `lanes` values here and this is the one place it has to be one value -
             * Implementation-Vector.md §7, and see VecParts for the pair of forms.
             *
             * Storage is what a zero value is for, and storage holds the array form: what a vector
             * *computes* as is its lanes, and nothing computes with a slot that has not been written.
             * A mask's lanes are `false` and every other lane's is the lane type's own zero, which
             * for every lane type this target admits is the number zero.
             */
            auto vector = (VectorType*)value;
            auto lanes = make<ArrayExpr>(g);

            for(U64 i = 0; i < constValue(g.global, vector->count); i++) {
                lanes->values.push(g.file.arena, vector->isMask ? boolean(g, false)
                                                                : zeroValue(g, vector->content));
            }

            return asExpr(g, lanes);
        }
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
 * A source-level constant, as a host value - see resolve/const.h, and repr/constant.cpp, which is
 * the same walk producing bytes for the target that has them.
 *
 * Written beside `zeroValue` and following it step for step, because the two have to produce the
 * *same shape*: a record built from a constant and a record built by filling a fresh slot are values
 * of one type, and a property one of them has and the other does not is a second hidden class for
 * every reader downstream. So every branch here has a counterpart above, and the property order is
 * `eachProperty`'s in both.
 *
 * What has no counterpart is an address, and that is the whole of what this target does not have: a
 * native constant string points its run at the bytes, and a host string is the text.
 */

// The bits of a constant whose type this target represents as one number - a scalarized record, a
// bit-tagged sum, a co-packed field. False where the constant is not one of those, which is what
// keeps a caller from writing a number where an object belongs.
static bool constantBits(Gen& g, ModulePtr<ConstValue> constant, U64& into);

// One property of an object built from a constant: the value the constructor's own fields put
// there, or nothing where this property belongs to a different constructor - see constructValue.
static Maybe<JsPtr<Expr>> propertyValue(Gen& g, TypePtr content, ModuleList<ModulePtr<ConstValue>, false>& children,
                                        Name key);

static JsPtr<Expr> aggregateValue(Gen& g, TypePtr tuple, ModuleList<ModulePtr<ConstValue>, false>& children);
static JsPtr<Expr> constructValue(Gen& g, ModulePtr<ConstValue> constant);

JsPtr<Expr> constantAggregate(Gen& g, ModulePtr<ConstValue> constant) {
    // A unit field, which has no property at all and therefore no value - the same silence
    // `eachProperty` keeps for one.
    if(!constant) return nullValue(g);

    auto& value = *g.local[constant];

    switch(value.kind) {
        case ConstKind::Scalar: {
            // Through the same `constantValue` every constant in a function body goes through, so a
            // global holding `1.5` and an expression holding it are one number and one rule.
            if(isFloat(g.global, value.type)) {
                ConstDouble constant_(nullptr, value.type, floatFromBits(g.global, value.type, value.bits));
                return constantValue(g, constant_);
            }

            ConstInt constant_(nullptr, value.type, value.bits);
            return constantValue(g, constant_);
        }

        case ConstKind::String: {
            ConstString constant_(nullptr, g.program.scalar.string_, value.text);
            return constantValue(g, constant_);
        }

        case ConstKind::Address:
            // A native string's run, which this target never builds: `stringConstant` produces the
            // text alone here, and there is nothing underneath it to reach.
            return nullValue(g);

        case ConstKind::Aggregate: {
            auto declared = g.global[value.type];

            if(declared->kind == Type::Array) {
                // `[T *n]` is a host array of exactly `n` elements - the same statement zeroValue
                // makes, with the elements written rather than zeroed.
                auto elements = make<ArrayExpr>(g);
                for(auto child: value.children.contents(g.local)) {
                    elements->values.push(g.file.arena, constantAggregate(g, child));
                }

                return hostArrayForElement(g, ((ArrayType*)declared)->content, asExpr(g, elements));
            }

            if(declared->kind != Type::Tup) return nullValue(g);
            return aggregateValue(g, value.type, value.children);
        }

        case ConstKind::Construct:
            return constructValue(g, constant);
    }

    return nullValue(g);
}

static bool constantBits(Gen& g, ModulePtr<ConstValue> constant, U64& into) {
    into = 0;
    if(!constant) return true;

    auto& value = *g.local[constant];
    if(value.kind == ConstKind::Scalar) {
        into = value.bits;
        return true;
    }

    // The word a scalarized aggregate is, assembled from the bit range each of its fields owns -
    // which is the same list `fieldProperty` hands the packed path below.
    auto fields = [&](TypePtr tuple, ModuleList<ModulePtr<ConstValue>, false>& children) {
        auto items = children.contents(g.local);
        auto count = ((TupType*)g.global[tuple])->fields.size();

        for(U16 slot = 0; slot < count && slot < items.size(); slot++) {
            U64 bits = 0;
            if(!constantBits(g, items[slot], bits)) return false;

            auto property = fieldProperty(g, tuple, slot);
            if(!property.isPacked()) {
                // A field that owns the whole word, which a one-field scalar record's does.
                into |= bits;
                continue;
            }

            auto mask = property.bitWidth >= 64 ? ~U64(0) : (U64(1) << property.bitWidth) - 1;
            into |= (bits & mask) << property.bitOffset;
        }

        return true;
    };

    if(value.kind == ConstKind::Aggregate && g.global[value.type]->kind == Type::Tup) {
        return fields(value.type, value.children);
    }

    if(value.kind != ConstKind::Construct || g.global[value.type]->kind != Type::Record) return false;

    auto& record = *(RecordType*)g.global[value.type];
    auto& repr = g.repr.of(value.type);

    if(discriminantOnly(g.global, record)) {
        into = value.index;
        return true;
    }

    // A bit tag is a co-packed field wearing another name - see bitTagAccess, which is the native
    // half of this - so it is written into the same word the payload sits in.
    if(repr.isBitTagged()) {
        auto mask = repr.discriminantBits >= 64 ? ~U64(0) : (U64(1) << repr.discriminantBits) - 1;
        into |= (value.index & mask) << repr.discriminantBitOffset;
    } else if(repr.discriminant != DiscriminantKind::None) {
        return false;
    }

    auto content = record.constructors.get(g.global, value.index).content;
    if(!content || isUnit(g.global, content)) return true;

    if(g.global[content]->kind == Type::Tup) return fields(content, value.children);

    U64 payload = 0;
    auto items = value.children.contents(g.local);
    if(items.size() != 1 || !constantBits(g, items[0], payload)) return false;

    into |= payload;
    return true;
}

static Maybe<JsPtr<Expr>> propertyValue(Gen& g, TypePtr content, ModuleList<ModulePtr<ConstValue>, false>& children,
                                        Name key) {
    if(!content || g.global[content]->kind != Type::Tup) return Nothing();

    auto items = children.contents(g.local);
    auto count = ((TupType*)g.global[content])->fields.size();

    U64 word = 0;
    auto packed = false;
    auto found = false;

    for(U16 slot = 0; slot < count && slot < items.size(); slot++) {
        auto property = fieldProperty(g, content, slot);
        if(property.name.text != key.text) continue;

        if(!property.isPacked()) {
            // A function value cannot be a constant - there is nothing to write a code word from -
            // so the two properties one occupies are never asked for here.
            if(property.fun) return Nothing();
            return Just(constantAggregate(g, items[slot]));
        }

        // Every field of one co-packed word contributes to the one property that word is, which is
        // why this keeps scanning rather than answering at the first match.
        U64 bits = 0;
        if(!constantBits(g, items[slot], bits)) return Nothing();

        auto mask = property.bitWidth >= 64 ? ~U64(0) : (U64(1) << property.bitWidth) - 1;
        word |= (bits & mask) << property.bitOffset;

        packed = true;
        found = true;
    }

    if(!packed || !found) return Nothing();
    return Just(number(g, F64(word)));
}

static JsPtr<Expr> aggregateValue(Gen& g, TypePtr tuple, ModuleList<ModulePtr<ConstValue>, false>& children) {
    auto items = children.contents(g.local);

    // A one-field tuple that this target represents as that field, which is the transparency
    // `isNewtype` decides for every reader of a shape.
    TypePtr inner = nullptr;
    if(isNewtype(g, tuple, inner)) return items.size() ? constantAggregate(g, items[0]) : nullValue(g);

    if(!isJsObject(g, tuple)) {
        // Assembled from the fields here rather than through `constantBits`, since what that would
        // be asked about is this tuple, and this tuple is what is being built.
        U64 bits = 0;
        auto count = ((TupType*)g.global[tuple])->fields.size();
        for(U16 slot = 0; slot < count && slot < items.size(); slot++) {
            U64 field = 0;
            if(!constantBits(g, items[slot], field)) return nullValue(g);

            auto property = fieldProperty(g, tuple, slot);
            if(!property.isPacked()) {
                bits |= field;
                continue;
            }

            auto mask = property.bitWidth >= 64 ? ~U64(0) : (U64(1) << property.bitWidth) - 1;
            bits |= (field & mask) << property.bitOffset;
        }

        return number(g, F64(bits));
    }

    auto object = make<ObjectExpr>(g);

    eachProperty(g, tuple, [&](Name key, TypePtr member) {
        auto value = propertyValue(g, tuple, children, key);
        object->properties.push(g.file.arena, Property { key, value ? value.unwrap() : zeroValue(g, member) });
    });

    return asExpr(g, object);
}

static JsPtr<Expr> constructValue(Gen& g, ModulePtr<ConstValue> constant) {
    auto& value = *g.local[constant];
    if(g.global[value.type]->kind != Type::Record) return nullValue(g);

    auto& record = *(RecordType*)g.global[value.type];
    auto& repr = g.repr.of(value.type);
    auto items = value.children.contents(g.local);

    // A record that is its discriminant is the tag number, exactly as zeroValue says a fresh one is
    // zero - and this is the shape a payload-free sum has as well as an enumeration's.
    if(discriminantOnly(g.global, record)) return number(g, value.index);

    auto content = record.constructors.get(g.global, U16(value.index)).content;

    // A newtype is the value it wraps, with no object of its own.
    TypePtr inner = nullptr;
    if(isNewtype(g, value.type, inner)) return items.size() ? constantAggregate(g, items[0]) : nullValue(g);

    /*
     * A niche-folded record, which is its payload plus one pattern taken out of it. The payload
     * constructor writes nothing extra - being inside the valid range is what identifies it - and
     * every other constructor is the pattern alone, which on a host target is `null`.
     */
    if(repr.isNicheFolded()) {
        if(value.index == repr.encoding.payloadConstructor) {
            if(content && g.global[content]->kind == Type::Tup) return aggregateValue(g, content, value.children);
            return items.size() ? constantAggregate(g, items[0]) : nullValue(g);
        }

        if(repr.encoding.niche.isAbsent()) return nullValue(g);
        return number(g, F64(repr.encoding.patternOf(U16(value.index))));
    }

    // A record the Repr made one number, whose every field - and whose tag, where it has one - is a
    // bit range of it.
    if(!isJsObject(g, value.type)) {
        U64 bits = 0;
        if(!constantBits(g, constant, bits)) return nullValue(g);
        return number(g, F64(bits));
    }

    if(record.layout == RecordType::Single) {
        if(!content || isUnit(g.global, content)) return asExpr(g, make<ObjectExpr>(g));
        if(g.global[content]->kind != Type::Tup) return items.size() ? constantAggregate(g, items[0]) : nullValue(g);

        return aggregateValue(g, content, value.children);
    }

    /*
     * A sum with a tag property, built through `eachProperty` so that it has every property a value
     * of the type will ever have - including the ones the *other* constructors' payloads occupy,
     * which take their zero here exactly as they do in a fresh slot.
     */
    auto object = make<ObjectExpr>(g);

    eachProperty(g, value.type, [&](Name key, TypePtr member) {
        if(key.text == g.tagField.text) {
            object->properties.push(g.file.arena, Property { key, number(g, F64(value.index)) });
            return;
        }

        /*
         * The one property a payload with no field names to flatten occupies - see eachProperty,
         * which is where the same three shapes are recognized.
         *
         * A *tuple* is two of those three - one the Repr made a number, and one that is its own
         * single field - and in both the children here are that tuple's fields rather than one
         * value. `aggregateValue` is what turns them back into the one value the property holds;
         * reading `children[0]` would have written the first field where the whole payload goes.
         */
        if(key.text == g.payloadField.text && content && !isUnit(g.global, content) &&
           payloadIsOneProperty(g, content)) {
            auto payload = g.global[content]->kind == Type::Tup
                ? aggregateValue(g, content, value.children)
                : (items.size() ? constantAggregate(g, items[0]) : nullValue(g));

            object->properties.push(g.file.arena, Property { key, payload });
            return;
        }

        auto found = propertyValue(g, content, value.children, key);
        object->properties.push(g.file.arena, Property { key, found ? found.unwrap() : zeroValue(g, member) });
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
 * range analysis `@bits` already needs would elide most of them. That is the asm.js tax, stated
 * where it is paid.
 *
 * Every one is still *emitted* here, unconditionally, and `foldCoercion` in opt.cpp takes back the
 * ones a range says are no-ops. The split is deliberate: what this function knows is one type, and
 * whether a coercion does anything is a question about the expression the value came out of - a
 * masked field read and a bounded counter are both `Int` and neither needs wrapping. Asking it here
 * would mean answering it once per instruction with none of the surrounding tree in hand.
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
            /*
             * A literal that is already this type's normal form, which the reduction would answer
             * with itself. `asIntN(64, 0n)` is `0n`, and leaving the call in is what stopped a
             * folded constant from reaching the comparison that wanted it - `Target.byteOrder` is
             * where that showed up.
             *
             * The full width and matching signedness only: those are the two facts that make the
             * identity obvious without reading the value, and every literal the emitter writes at a
             * 64-bit type is one. A `@bits(58) U64` still goes through the host.
             */
            U64 literal;
            bool literalSigned;
            if(integer->bits == 64 && constantBigInt(g, value, literal, literalSigned) &&
               literalSigned == integer->isSigned) {
                return value;
            }

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

        auto bits = heldBits(g, *integer);
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
 * Moving a value out of storage that is about to be written - the fresh slot native's `memcpy`
 * relocates into, and the *shallow* half of `cloneValue` below.
 *
 * The difference is one level deep and it is the whole difference: a property is read straight
 * rather than duplicated, so the value handed back shares every nested object with the storage it
 * came out of. That is sound exactly where this is used and nowhere else - the write that follows
 * rebinds *every* property of that storage (see storeInto's property walk, and genBlockCopy's), so
 * the two stop sharing anything the moment it has run, which is what makes this a relocation rather
 * than an alias.
 *
 * Why a duplicate is needed at all is that this target has no slots. Native relocates the old bytes
 * into the result's own stack slot and then overwrites the place; here `placeExpr` of a place rooted
 * in a reference *is* the object the write is about to change property by property, so reading it
 * and writing it are the same object and the old value is gone. `exchange(a, [])` answered a length
 * of 0 where native answered 3 - see genExchange, and genSwap next door, which had it twice over.
 *
 * `cloneValue` is the wrong tool for it: duplicating the nested objects as well is work the caller
 * does not need and would make an `exchange` in a loop quadratic in the depth of the value.
 */
JsPtr<Expr> relocatedValue(Gen& g, TypePtr type, JsPtr<Expr> source) {
    // A niche-folded record is not an object but may hold one, and a newtype is the value it wraps -
    // both are representation steps rather than structure, so both are followed rather than stopped
    // at. Exactly the two cloneValue opens with, and for the same reason.
    if(auto payload = foldedPayload(g, type)) {
        if(!isJsObject(g, payload)) return source;

        return ternary(g, binary(g, BinaryOp::Eq, source, nullValue(g)), nullValue(g),
                       relocatedValue(g, payload, source));
    }

    if(!isJsObject(g, type)) return source;

    TypePtr content = nullptr;
    if(isNewtype(g, type, content)) return relocatedValue(g, content, source);

    /*
     * A shape this body cannot see, handed back as it stands.
     *
     * An erased write goes through the descriptor's `moveInit` into storage the caller made (see
     * erasedRelocate), so the duplicate this would build is one the write does not need - and there
     * is no property list here to build it out of anyway.
     */
    if(g.global[type]->kind == Type::Gen) return source;

    // A host row's elements are its slots, and `eachProperty` answers nothing for one - the same
    // arm cloneValue needs, and `slice()` is already the shallow copy this wants.
    if(g.global[type]->kind == Type::Array) {
        return asPureCall(g, call(g, field(g, source, literalName(g, "slice"_v))));
    }

    auto object = make<ObjectExpr>(g);
    eachProperty(g, type, [&](Name key, TypePtr member) {
        object->properties.push(g.file.arena, Property { key, field(g, source, key) });
    });

    return asExpr(g, object);
}

/*
 * Copying - the one ownership operation that costs anything here (§2.5).
 *
 * A structural duplicate, property by property, rather than a call into a runtime cloner: the shape
 * is known at compile time and an object literal listing every property in construction order is
 * both the fastest form and the one that keeps the type's hidden class.
 *
 * `relocatedValue` above is the shallow form, which is what a *move* out of storage wants.
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
    // boolean. A function value is not one of those any more: it is the two words, so its copy is a
    // fresh pair holding the same code word and the same environment, which is exactly what native's
    // copy of the two words is.
    if(!isJsObject(g, type)) return source;

    if(g.global[type]->kind == Type::Gen) {
        g.context.diagnostics.error("the JS target cannot copy a value of a type it cannot see the shape of"_v, where);
        return source;
    }

    // A newtype is the value it wraps, so there is nothing structural to duplicate.
    TypePtr content = nullptr;
    if(isNewtype(g, type, content)) return cloneValue(g, content, source, where);

    /*
     * `[T *n]` is a host array here, and it needs a case of its own for the reason `zeroValue` above
     * needs one: `eachProperty` answers *nothing* for an array - a fixed array has elements rather
     * than properties - so the object literal below would build `{}` and every element would be
     * gone. That is not hypothetical; it is what a record with a `[U32 *4]` field passed by value
     * became on this target, and the digest built out of it answered zeros.
     *
     * `slice()` where the elements are values a read duplicates - a number, a bigint - which is the
     * shallow copy every engine has and is the whole answer for the numeric arrays this type is
     * nearly always over. Where an element is an *object*, the copy has to reach it: the count is
     * part of the type, so the elements are written out and cloned one by one rather than mapped
     * through a lambda this IR would have to build.
     */
    if(g.global[type]->kind == Type::Array) {
        auto array = (ArrayType*)g.global[type];

        // `slice()` needs no count at all, so it answers an erased array as readily as a written one
        // and is asked first. The written-out form below is the one that cannot: `writtenCount`
        // rather than `constValue`, which asserts that the count is a number - and where there is
        // none the elements are objects, which is the case genInstruction's Alloc arm reports.
        if(!isJsObject(g, array->content)) {
            return asPureCall(g, call(g, field(g, source, literalName(g, "slice"_v))));
        }

        auto count = writtenCount(g.global, array->count).from(0);

        auto elements = make<ArrayExpr>(g);
        for(U64 i = 0; i < count; i++) {
            elements->values.push(g.file.arena,
                                  cloneValue(g, array->content, index(g, source, U32(i)), where));
        }

        return asExpr(g, elements);
    }

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
        auto reinterpreted = value.kind == Value::Cast || value.kind == Value::Bitcast;
        auto source = reinterpreted ? g.local[((InstUnary&)value).from]->type : value.type;

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
