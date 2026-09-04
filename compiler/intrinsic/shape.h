#pragma once

#include "role.h"

namespace intrinsic {

/*
 * What an intrinsic's declaration has to look like for its emitter to be right about it.
 *
 * This is the half that was missing, and the reason it was worth building. A signature with no body
 * is *the* signature - the type checker reads it, a caller is checked against it, and nothing about
 * it is written twice. What the compiler adds is an emitter, and an emitter necessarily assumes
 * things: that argument zero is a pointer, that the result is what it points at, that argument one
 * arrives owned. Those assumptions were unwritten. `attachIntrinsic` matched a name and nothing
 * else, so a declaration edited to a different arity, a different type or a different convention
 * produced no diagnostic anywhere - the emitter simply read `args[0]` and carried on.
 *
 * The live example is `Atomic.intoValue(->self: Atomic(a)) -> a`, whose declaration says the subject
 * is handed over and whose expansion consumed nothing: the value stayed live, a second use was
 * accepted, and the only thing keeping it from being a double release was that the content happened
 * to be trivially copyable. A shape is what makes that a rejected declaration rather than a latent
 * one.
 *
 * ## What a shape is not
 *
 * It is **not the signature restated**. A restatement would be the same fact in two places, which is
 * the disease rather than the cure - and it would be unwritable anyway, since the interesting
 * signatures are generic, constrained and origin-qualified. What a shape states is only what the
 * *emitter* depends on, and every term below exists because some emitter reads it:
 * `pointeeOf` because a load's result type is computed from its operand, `handoverOf` because a
 * store's value has to arrive owned or the frame keeps an obligation the pointee now holds.
 *
 * Everything a shape does not mention is the library's to decide freely.
 */

/*
 * The type a position must have, stated relative to the other positions where it is derived.
 *
 * The relative terms are the point: `fn *(it: %a) -> a` is checked as "the result is what parameter
 * zero points at", which stays true whatever the library calls `a` and holds for the substituted
 * signature at every call. An absolute term - "the result is `I32`" - would be a claim about a
 * generic signature that no generic signature could satisfy.
 */
enum class ShapeTerm: U8 {
    // No constraint. The emitter works at whatever type the declaration wrote.
    Any,

    // A variable of the declaration's own generic context - the emitter needs the position to be
    // *open*, not merely to be some particular type.
    Var,

    // `%T`, for any pointee.
    Pointer,

    // What the pointer at position `of` points at. `of` is a parameter index; the result position
    // may name a parameter, and a parameter may name an earlier one.
    PointeeOf,

    // A pointer to whatever is at position `of`.
    PointerTo,

    // The same type as position `of`.
    SameAs,

    // Any integer type - a width the target has, signed or not.
    Integer,

    Bool,
    Unit,

    // A function value - two words, a code pointer and an environment. `cloneThread` is the one
    // emitter that reads them apart, and the term is what says so: a declaration that wrote a raw
    // address there would have the two loads reading a pointer's own bytes as a pair.
    Function,

    // A `@lazy` parameter: the position is declared deferred, and the expansion resolves the
    // argument itself rather than receiving a thunk. Only a `Flow` role may ask for one, since a
    // `Node` emitter is handed prepared values and a thunk is what preparation would make.
    Deferred,

    /*
     * The language's string type, whose *constant* the emitter reads.
     *
     * The one term that is about a value rather than about a type, and the host calls are what it
     * exists for: `hostCall1(s, "charCodeAt", i)` names a member of the host, and the name has to
     * be in the emitted source rather than in a variable the program could have computed. So the
     * shape says `String` and the emitter reports a position that is not a literal, which together
     * are the whole of the dependency.
     *
     * Why a *parameter* and not an attribute on the declaration, which is where every other
     * compile-time name in this directory lives: a role is claimed once. One `@builtin(hostCall1)`
     * declaration serves every one-argument method the host has, and a name in an attribute would
     * need one role per method - which is the thirty-three name matches this replaced, renamed.
     */
    Text,
};

StringView shapeTermName(ShapeTerm term);

/*
 * How a position is passed, where the emitter depends on it.
 *
 * `Any` is the honest default and most positions have it: an emitter that only reads a value does
 * not care whether the author wrote a plain parameter or a borrow, and a shape that insisted would
 * be forbidding the library a choice for no reason.
 *
 * The others are each a real dependency. `Owned` is a value the expansion *consumes* - the store
 * that has to be `->`. `Unique` is one it writes through - `swap`'s two places, `borrowMut`'s
 * result. `Shared` is one it only reads.
 */
enum class ShapeAccess: U8 {
    Any,
    Shared,   // a plain parameter, or `'T`
    Owned,    // `->`: the expansion consumes it
    Unique,   // `&`: exclusive, written through
};

/*
 * Where a position's storage comes from, which is the axis the access one is not.
 *
 * Two fields rather than one enum of the combinations, and the combinations are what forced it:
 * `borrow(it: %source'a) -> source'a` and `borrowMut(it: %source'a) -> &source'a` differ in the
 * access and agree on the origin, while `swap`'s two parameters agree on the access and say nothing
 * about the origin. One enum would have needed a `PublishedUnique` beside a `PublishedShared`
 * beside a `Published`, which is a product of two axes spelled as a list - and the list grows by
 * multiplication every time either axis gains a value.
 *
 * The spellings below are still named for what a position *means*, which is what makes a row
 * readable; what changed is that each expands to two independent answers rather than to one that
 * quietly checks both.
 */
enum class ShapeOrigin: U8 {
    Any,

    // Bounded by the call it was granted for - the default a plain parameter has, and what an
    // emitter that hands nothing back out of its operands depends on.
    CallBounded,

    // Rooted in an origin the signature published, which is what lets the result outlive the call -
    // `'T`, `&T` and the `return` marker that spelled it before.
    Published,
};

StringView shapeAccessName(ShapeAccess access);
StringView shapeOriginName(ShapeOrigin origin);

/*
 * One position of an intrinsic's signature - a parameter, or the result.
 *
 * Four bytes and a plain aggregate, so the whole table is a constant with no initialization to run.
 */
struct PositionShape {
    ShapeTerm term = ShapeTerm::Any;

    // Which position `PointeeOf`, `PointerTo` and `SameAs` are relative to. A parameter index; the
    // terms that do not use it leave it zero.
    U8 of = 0;

    ShapeAccess access = ShapeAccess::Any;
    ShapeOrigin origin = ShapeOrigin::Any;
};

/*
 * The spellings `op.def` is written in.
 *
 * Named for what the position *means* rather than for the enumerators they set, because that is
 * what makes a row readable: `handoverOf(0)` says "the thing parameter zero points at, consumed",
 * which is the sentence `store`'s second parameter is. A row assembled from bare enumerators would
 * be the same information spelled as a struct literal, and nobody reads those.
 *
 * The list is longer than the axes are wide and deliberately so: what a row needs is a name for the
 * handful of *combinations that mean something*, and a spelling per combination is what keeps
 * `borrowOf(0)` from being written `PositionShape { PointeeOf, 0, Shared, Published }` at the one
 * place a reader is trying to see the signature.
 */
constexpr PositionShape anyAt(ShapeAccess access = ShapeAccess::Any,
                              ShapeOrigin origin = ShapeOrigin::Any) {
    return PositionShape { ShapeTerm::Any, 0, access, origin };
}

constexpr PositionShape varAt(ShapeAccess access = ShapeAccess::Any,
                              ShapeOrigin origin = ShapeOrigin::Any) {
    return PositionShape { ShapeTerm::Var, 0, access, origin };
}

constexpr PositionShape unitAt() { return PositionShape { ShapeTerm::Unit, 0 }; }
constexpr PositionShape deferredAt() { return PositionShape { ShapeTerm::Deferred, 0 }; }
constexpr PositionShape boolAt() { return PositionShape { ShapeTerm::Bool, 0 }; }
constexpr PositionShape integerAt() { return PositionShape { ShapeTerm::Integer, 0 }; }
constexpr PositionShape functionAt() { return PositionShape { ShapeTerm::Function, 0 }; }

// A string the emitter reads at compile time - see `ShapeTerm::Text`. Shared rather than owned: the
// text is spent building a node and no run of it survives the call.
constexpr PositionShape textAt() {
    return PositionShape { ShapeTerm::Text, 0, ShapeAccess::Shared, ShapeOrigin::Any };
}

// A plain value, read and not kept: what a parameter says when it is written with no sigil at all.
// The pair every emitter that only reads its operands depends on.
constexpr PositionShape valueAt() {
    return PositionShape { ShapeTerm::Any, 0, ShapeAccess::Shared, ShapeOrigin::CallBounded };
}

// A pointer, read but not consumed - what every operation through one takes.
constexpr PositionShape pointerAt() {
    return PositionShape { ShapeTerm::Pointer, 0, ShapeAccess::Shared, ShapeOrigin::CallBounded };
}

// What the pointer at `of` points at, with no requirement on how it is passed.
constexpr PositionShape pointeeOf(U8 of) { return PositionShape { ShapeTerm::PointeeOf, of }; }

// The same, **handed over**: the expansion consumes it, so the declaration must say `->` or the
// frame keeps an obligation for a value it no longer holds.
constexpr PositionShape handoverOf(U8 of) {
    return PositionShape { ShapeTerm::PointeeOf, of, ShapeAccess::Owned };
}

// A shared borrow of what `of` points at, rooted in an origin the signature published -
// `fn borrow(it: %source'a) -> source'a`. The origin is what lets the result outlive the call, and
// the access is what says the caller may not write through it.
constexpr PositionShape borrowOf(U8 of) {
    return PositionShape { ShapeTerm::PointeeOf, of, ShapeAccess::Shared, ShapeOrigin::Published };
}

// The same borrow, exclusive - `fn borrowMut(it: %source'a) -> &source'a`. One spelling apart from
// the last, and the two axes are why: they differ in the access and agree on the origin.
constexpr PositionShape borrowMutOf(U8 of) {
    return PositionShape { ShapeTerm::PointeeOf, of, ShapeAccess::Unique, ShapeOrigin::Published };
}

constexpr PositionShape pointerTo(U8 of) { return PositionShape { ShapeTerm::PointerTo, of }; }

constexpr PositionShape sameAs(U8 of) { return PositionShape { ShapeTerm::SameAs, of }; }

// The same type as `of`, **handed over** - `fn exchange(&slot: a, ->value: a) -> a`, whose second
// parameter is the value the slot holds afterwards.
constexpr PositionShape handedSameAs(U8 of) {
    return PositionShape { ShapeTerm::SameAs, of, ShapeAccess::Owned };
}

// A place the expansion writes through, open at the declaration's own variable - `swap`'s two
// parameters, and `exchange`'s first.
constexpr PositionShape placeAt() {
    return PositionShape { ShapeTerm::Var, 0, ShapeAccess::Unique };
}

// The same place, said of a position that must agree with an earlier one - `swap`'s second.
constexpr PositionShape placeSameAs(U8 of) {
    return PositionShape { ShapeTerm::SameAs, of, ShapeAccess::Unique };
}

// A whole value borrowed from somewhere the signature published, with no requirement on its type -
// `atomicAt`'s result, a borrow of the memory a pointer names.
constexpr PositionShape viewAt() {
    return PositionShape { ShapeTerm::Any, 0, ShapeAccess::Shared, ShapeOrigin::Published };
}

// And the exclusive one.
constexpr PositionShape viewMutAt() {
    return PositionShape { ShapeTerm::Any, 0, ShapeAccess::Unique, ShapeOrigin::Published };
}

// A value the expansion consumes, at whatever type the declaration wrote -
// `fn atomicIntoValue(->value: Atomic(a)) -> a`.
constexpr PositionShape handoverAt() {
    return PositionShape { ShapeTerm::Any, 0, ShapeAccess::Owned };
}

} // namespace intrinsic
