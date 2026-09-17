#pragma once

#include "shape.h"

namespace intrinsic {

/*
 * The intrinsic operations - `op.def`.
 *
 * A dense enum, because what a consumer does with one is `switch` on it: an emitter table is
 * `Count` entries and the compiler warns about a row nobody implemented.
 */
enum class Op: U16 {
#define YANA_INTRINSIC_OP(role, form, result, ...) role,
#include "op.def"
#undef YANA_INTRINSIC_OP

    Count,
};

// The largest number of parameters any row declares. A bound rather than a list, so `OpDef` is a
// plain aggregate and the whole table is a constant with nothing to initialize at start-up.
constexpr Size kMaxIntrinsicParams = 7;

/*
 * Where a role expands, which decides whether its declaration may have a body.
 *
 * A `Node` role is the ordinary case: its emitter takes values the call path prepared and answers
 * one node, and the declaration has no body because the emitter is the whole definition. A `Flow`
 * role is one whose expansion is *control flow around the call* - a branch with an operand resolved
 * under it - so it is laid out by the resolver, which owns the block structure, and the declaration
 * carries the body the language writes for it. That body is not dead: a call the resolver does not
 * expand reaches it, and it is the statement of what the operator means.
 *
 * A `Branch` role is the two halves apart: its expansion *makes blocks* like a `Flow` role, and it
 * has no body like a `Node` role. `compareExchange` is the shape - one machine instruction answering
 * two things, joined into the one value the library declares - and what forces it out of an emitter
 * is not the branch itself but what crosses one: a value the surrounding walk is holding has to be
 * handed along the edges as a block argument, and only the resolver knows what it is holding. An
 * emitter that made blocks would silently strand every such value.
 */
enum class OpForm: U8 {
    Node,
    Flow,
    Branch,
};

// Whether a declaration claiming this role **must** write a body. Only a `Flow` role does - the
// compiler is the whole definition of the other two - and it is one predicate so that the two
// resolve stages cannot come to different answers about the same row.
inline bool roleWritesBody(OpForm form) { return form == OpForm::Flow; }

struct OpDef {
    // The attribute's spelling - the enumerator's own name. See `YANA_ROLE_TEXT`.
    StringView role;

    OpForm form;

    PositionShape result;
    PositionShape params[kMaxIntrinsicParams];

    // How many of `params` a row wrote, which is the arity the declaration must have.
    U8 count;
};

/*
 * How a row is built, and why the count is not a column.
 *
 * The parameters are variadic, so `count` is `sizeof...(P)` and a row cannot state an arity that
 * disagrees with the list it wrote beside it. Writing the number out would be the third place an
 * arity is stated - the declaration, the shape list, and a column - and two of those three can be
 * kept honest by construction.
 */
template<class... P>
constexpr OpDef makeOp(StringView role, OpForm form, PositionShape result, P... params) {
    static_assert(sizeof...(P) <= kMaxIntrinsicParams, "raise kMaxIntrinsicParams for this row");
    return OpDef { role, form, result, { params... }, U8(sizeof...(P)) };
}

extern const OpDef opTable[Size(Op::Count)];

inline const OpDef& opDef(Op role) { return opTable[Size(role)]; }

/*
 * Whether a declaration claiming this role **may** write one, which is the weaker question and has
 * one more answer than `roleWritesBody`.
 *
 * A body beside a role is not two implementations of one function. It is what a *stage that does not
 * expand the role* runs, which is exactly what a `Flow` role's body already is - the resolver lays
 * that operator out where it is written, and every other reach of the declaration calls the body.
 *
 * `newRun` is the same relationship along a different axis. The shipping stage expands it into an
 * allocation with a count whose **placement** its escape analysis decides; the ownership resolver
 * has no placement analysis, so every run it built would be a heap run whatever it expanded to - and
 * a heap run is something the language can say, with the allocator's refusal handled, which is what
 * `lib/Native/Run.native.yana` writes. The day that stage places allocations the body becomes dead
 * in both and this answer goes back to the form alone.
 *
 * Optional rather than required, because the other declaration claiming the role is
 * `lib/Native/Run.js.yana`'s, where a run is a descriptor the compiler places and there is no heap
 * to ask - so the fallback exists on the target that can state it and nowhere else.
 */
inline bool roleMayWriteBody(Op role) {
    switch(role) {
        case Op::newRun:
            return true;

        default:
            return roleWritesBody(opDef(role).form);
    }
}

} // namespace intrinsic
