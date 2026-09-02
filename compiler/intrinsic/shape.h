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

    // A `@lazy` parameter: the position is declared deferred, and the expansion resolves the
    // argument itself rather than receiving a thunk. Only a `Flow` role may ask for one, since a
    // `Node` emitter is handed prepared values and a thunk is what preparation would make.
    Deferred,
};

StringView shapeTermName(ShapeTerm term);

/*
 * The contract a position must be declared with, where the emitter depends on it.
 *
 * `Unchecked` is the honest default and most positions have it: an emitter that only reads a value
 * does not care whether the author wrote a plain parameter or a borrow, and a shape that insisted
 * would be forbidding the library a choice for no reason.
 *
 * The others are each a real dependency. `Handover` is a value the expansion *consumes* - the store
 * that has to be `->`. `Unique` is one it writes through. `Published` is one whose extent the result
 * is rooted in, which is what a `return` parameter says and what a borrow handed back out of an
 * unchecked pointer needs.
 */
enum class ShapeContract: U8 {
    Unchecked,
    Value,      // plain: shared access, bounded by the call
    Handover,   // `->`: owned, and consumed by the expansion
    Unique,     // `&`: exclusive access, written through
    Published,  // `return` / `'`: rooted in an origin the signature published
};

StringView shapeContractName(ShapeContract contract);

/*
 * One position of an intrinsic's signature - a parameter, or the result.
 *
 * Three bytes and a plain aggregate, so the whole table is a constant with no initialization to run.
 */
struct PositionShape {
    ShapeTerm term = ShapeTerm::Any;

    // Which position `PointeeOf`, `PointerTo` and `SameAs` are relative to. A parameter index; the
    // terms that do not use it leave it zero.
    U8 of = 0;

    ShapeContract contract = ShapeContract::Unchecked;
};

/*
 * The spellings `op.def` is written in.
 *
 * Named for what the position *means* rather than for the two enumerators they set, because that is
 * what makes a row readable: `handoverOf(0)` says "the thing parameter zero points at, consumed",
 * which is the sentence `store`'s second parameter is. A row assembled from bare enumerators would
 * be the same information spelled as a struct literal, and nobody reads those.
 */
constexpr PositionShape anyAt(ShapeContract contract = ShapeContract::Unchecked) {
    return PositionShape { ShapeTerm::Any, 0, contract };
}

constexpr PositionShape varAt(ShapeContract contract = ShapeContract::Unchecked) {
    return PositionShape { ShapeTerm::Var, 0, contract };
}

constexpr PositionShape unitAt() { return PositionShape { ShapeTerm::Unit, 0 }; }
constexpr PositionShape deferredAt() { return PositionShape { ShapeTerm::Deferred, 0 }; }
constexpr PositionShape boolAt() { return PositionShape { ShapeTerm::Bool, 0 }; }
constexpr PositionShape integerAt() { return PositionShape { ShapeTerm::Integer, 0 }; }

// A pointer, read but not consumed - what every operation through one takes.
constexpr PositionShape pointerAt() {
    return PositionShape { ShapeTerm::Pointer, 0, ShapeContract::Value };
}

// What the pointer at `of` points at, with no requirement on how it is passed.
constexpr PositionShape pointeeOf(U8 of) { return PositionShape { ShapeTerm::PointeeOf, of }; }

// The same, **handed over**: the expansion consumes it, so the declaration must say `->` or the
// frame keeps an obligation for a value it no longer holds.
constexpr PositionShape handoverOf(U8 of) {
    return PositionShape { ShapeTerm::PointeeOf, of, ShapeContract::Handover };
}

// A borrow of what `of` points at, rooted in an origin the signature published - `return it: %a`.
constexpr PositionShape borrowOf(U8 of) {
    return PositionShape { ShapeTerm::PointeeOf, of, ShapeContract::Published };
}

constexpr PositionShape pointerTo(U8 of) { return PositionShape { ShapeTerm::PointerTo, of }; }
constexpr PositionShape sameAs(U8 of) { return PositionShape { ShapeTerm::SameAs, of }; }

} // namespace intrinsic
