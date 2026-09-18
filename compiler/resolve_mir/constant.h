#pragma once

/*
 * `contract.h` rather than `type.h`, for the reason that header states about itself: a constant
 * *names* a logical type and never asks it anything, so a forward declaration is the whole of the
 * dependency. Which way it runs matters here - `type.h` includes this one, because a record field's
 * declaration carries its default.
 */
#include "contract.h"

namespace mir {

// Defined below `Constant`, because its parts are constants - see `Constant::Aggregate`.
struct AggregateConstant;

// Named by `Constant::Address` and by nothing else here - a constant may *point at* a global and
// never asks it anything, which is `contract.h`'s own argument for the forward declaration above.
struct Global;
struct Function;

/*
 * A compile-time value: a logical type and up to eight bytes.
 *
 * **A value, not an allocated node.** It used to be a four-kind hierarchy reached through a
 * `DeclPtr`, and the cost of that was not the indirection: a literal inside a function body is body
 * data, and allocating it in the declaration region meant every `7` in every function survived the
 * release of the MIR arena and stayed resident for the whole compilation. Sixteen bytes held
 * *where the constant is named* has no such lifetime - a body's table lives in the MIR region and
 * dies with it, and a global's initializer is a field of the global.
 *
 * Every kind fits in eight bytes and always will: an aggregate constant is a list of these plus a
 * layout, which is a table in the region that names it rather than a wider payload here.
 *
 * A constant has a logical type and no contract: it is owned by whoever installs it, and a constant
 * whose bytes are static is still an owned occurrence of its type. Analysis-IR.md §5.8 is the rule
 * this follows - a shared static producer is a property of the producer, not evidence that the value
 * is trivially copyable.
 */
struct Constant {
    enum Kind: U8 {
        // Not a constant. What a failed resolution answers, so that a caller checks one thing rather
        // than carrying a null handle beside a value.
        None,

        Unit,

        /*
         * Which constructor of an enum-layout record this is, as an **index** into `cons`.
         *
         * The index and not the number that is stored: `RecordCon::value` is what a tag holds and
         * `@value(n)` is what may move it, so a constant that carried the number would be stating a
         * representation the tier above the manifestation table is not entitled to know. A dump
         * prints the constructor's own name from that index, which is how `True` prints as `True`.
         *
         * It is the one construction that is a constant, and generally so rather than for `Bool`:
         * a constructor carrying no fields has nothing a site could vary, so `Off` is a compile-time
         * value wherever it is written - a field default, a short-circuit arm, an argument.
         */
        Enum,

        // The value as 64 unsigned bits, sign-extended from its declared width if it is signed. The
        // width is on the type; storing it again here would be a second answer to one question.
        Int,

        // The IEEE bit pattern of the `double`, whatever the declared width. Narrowing to `F32`
        // happens where the bytes are laid out, because that is the only place the target's answer
        // about the width is in hand.
        Float,

        /*
         * A record or a fixed array whose parts are all constants - `bits` is the handle of an
         * `AggregateConstant`, which lives in the declaration region.
         *
         * **A tree of values and never a block of bytes.** Where a field sits, how wide an address
         * is and which end of it comes first are the *target's* answers, and this tier has none of
         * them; `resolve_ssa` lays one out with the same manifestation it lays every other value of
         * that type out with. An evaluator that produced bytes would be a second answer to the
         * question `manifest.h` owns.
         *
         * It is also the reason there is no global-versus-local split in what a constant *is*: a
         * `let` at module level, a field default and a construction inside a body reduce to the
         * same value, and where that value is put - static storage, an immediate, a run of stores -
         * is a placement question the target answers per constant.
         */
        Aggregate,

        /*
         * `"abc"` - the **text** of a string literal, as the name it is interned under. `bits` is a
         * `StringId`; the type is the record `known.def`'s `string` row names, or the
         * `[CodeUnit *n]` its bytes are laid out at.
         *
         * A kind of its own rather than the aggregate a `String` eventually is, because what the
         * bytes become is the *target's* answer and not resolve's: a native build wants a run in
         * static storage and a descriptor pointing into it, JS wants the host's own string, and a
         * position that never reads the value wants neither. `Constant::Aggregate` would have
         * decided that here by spelling the fields of a type the library owns - which is exactly
         * what `resolve/const.cpp` does in the shipping stage, and why it has a "this target's
         * string layout is not the one this compiler writes" diagnostic to report.
         *
         * So this says what the value *is*, and `Resolver::stringConstant` is the one place that
         * says how it is built. It is what lets a string be a constant at all - a default argument,
         * a field default - where the value path used to be the only way to spell one.
         *
         * It has a second job since J46 part 3: the **bytes** global's initializer is a `Text` at
         * `[CodeUnit *n]`, written by `materialize` in one pass over the name. It was `n` interned
         * `Constant`s and a list over them, and interning is structural - so every literal in the
         * program walked every byte of every literal before it. `test/bench/findings.md` §94.
         */
        Text,

        /*
         * The **address of a global** - `bits` is the global's declaration-region offset, and the
         * type is the pointer type the position holds it at.
         *
         * The one leaf of a constant whose value is not known until the image is placed, which is
         * what makes it a kind rather than an `Int`: `materialize` writes zero where it goes and
         * records a `LowerDataRelocation` beside it, and the loader fills it in.
         *
         * It exists so that a **string** can be a constant. A literal's run points at bytes the
         * compiler placed, so the value has a pointer in it - and a tier that could not spell one
         * had to make every string global a startup initializer, which a library module has no
         * point at which to run.
         *
         * Nothing in the source spells one. It is minted by `Resolver::stringBytesGlobal`, and the
         * day `&someGlobal` becomes a constant expression this is the kind it produces.
         */
        Address,

        // The direct entry address of an explicitly conventioned function. Like Address, its bits
        // are filled by image placement rather than known during resolution.
        Function,
    };

    DeclPtr<Type> type = nullptr;
    Kind kind = None;
    U64 bits = 0;

    bool isSet() const { return kind != None; }

    U16 asConstructor() const { return U16(bits); }
    U64 asInt() const { return bits; }
    StringId asText() const { return StringId(U32(bits)); }
    DeclPtr<Global> asGlobal() const { return DeclPtr<Global>(U32(bits)); }
    DeclPtr<::mir::Function> asFunction() const { return DeclPtr<::mir::Function>(U32(bits)); }
    DeclPtr<AggregateConstant> aggregate() const { return DeclPtr<AggregateConstant>(U32(bits)); }

    double asFloat() const {
        double value;
        copy((const Byte*)&bits, (Byte*)&value, 8);
        return value;
    }

    bool operator == (const Constant& other) const {
        return type == other.type && kind == other.kind && bits == other.bits;
    }
};

/*
 * What an `Aggregate` constant names - the parts, and which constructor they fill.
 *
 * In the **declaration** region, because that is the one region every reader of a `Constant` already
 * has: a global's initializer is a field of the global, a field default is a field of the record,
 * and a body's constant table is copied by value into SSA so that the MIR region can be released.
 * A payload held in the MIR region would not survive that copy, and one held per region would make
 * `Constant` mean different things depending on who was holding it.
 *
 * `constructor` is an index into `RecordType::cons`, so a product's is zero and a sum's says which
 * arm - the same index `Constant::Enum` carries, for the same reason it carries the index rather
 * than the stored number. `values` fills that constructor's field *range* in order, or the elements
 * of a fixed array in index order.
 */
struct AggregateConstant {
    U16 constructor = 0;

    // Non-embedding for `RecordField`'s reason: a `Constant` is sixteen bytes and `SmallList`'s
    // embedded encoding is for elements of four or fewer.
    DeclList<Constant, false> values;
};

inline Constant unitConstant(DeclPtr<Type> type) { return Constant { type, Constant::Unit, 0 }; }

inline Constant enumConstant(DeclPtr<Type> type, U16 constructor) {
    return Constant { type, Constant::Enum, U64(constructor) };
}

inline Constant intConstant(DeclPtr<Type> type, U64 value) {
    return Constant { type, Constant::Int, value };
}

inline Constant textConstant(DeclPtr<Type> type, StringId text) {
    return Constant { type, Constant::Text, U64(text.value) };
}

inline Constant addressConstant(DeclPtr<Type> type, DeclPtr<Global> global) {
    return Constant { type, Constant::Address, U64(global.offset) };
}

inline Constant functionConstant(DeclPtr<Type> type, DeclPtr<::mir::Function> function) {
    return Constant { type, Constant::Function, U64(function.offset) };
}

/*
 * Whether a value has a `Constant::Text` anywhere in it - the root, or a leaf of an aggregate.
 *
 * One predicate rather than a test at the root, because a text is deferred and an *array* of texts
 * is the same deferral with the leaves further in: `let names = ["a", "b"]` has to wait for the
 * library's own string builder exactly as `let name = "a"` does. Written here beside the kind
 * rather than in the resolver, so that a second site that has to know "is this settled yet" asks
 * the same question.
 */
bool holdsText(DeclBase base, const Constant& constant);

/*
 * One component of an aggregate constant, by its index **in the record** - and unset where there is
 * no such component.
 *
 * `AggregateConstant::values` fills one constructor's field range in order, so a record's index has
 * that arm's first field taken off it and a fixed array's is the position itself, which is the one
 * case where the two numbers coincide. Unset covers all three ways there is nothing to hand back:
 * the parent is not an aggregate, the index names a field of a *different* constructor than the one
 * the value was built at, or it is out of range.
 *
 * One function rather than a walk at each of the two tiers that indexes a constant - the evaluator
 * running the library's own bodies, and the optimizer answering a read of a slot filled by one -
 * because a second copy would be a second answer to "which value of the list is field `n`", and the
 * way that fails is a wrong number rather than a crash.
 */
Constant constantField(DeclBase base, const Constant& parent, U32 index);

inline Constant floatConstant(DeclPtr<Type> type, double value) {
    U64 bits;
    copy((const Byte*)&value, (Byte*)&bits, 8);
    return Constant { type, Constant::Float, bits };
}

} // namespace mir
