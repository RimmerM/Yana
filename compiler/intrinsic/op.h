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
#define YANA_INTRINSIC_OP(role, result, ...) role,
#include "op.def"
#undef YANA_INTRINSIC_OP

    Count,
};

// The largest number of parameters any row declares. A bound rather than a list, so `OpDef` is a
// plain aggregate and the whole table is a constant with nothing to initialize at start-up.
constexpr Size kMaxIntrinsicParams = 6;

struct OpDef {
    // The attribute's spelling - the enumerator's own name. See `YANA_ROLE_TEXT`.
    StringView role;

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
constexpr OpDef makeOp(StringView role, PositionShape result, P... params) {
    static_assert(sizeof...(P) <= kMaxIntrinsicParams, "raise kMaxIntrinsicParams for this row");
    return OpDef { role, result, { params... }, U8(sizeof...(P)) };
}

extern const OpDef opTable[Size(Op::Count)];

inline const OpDef& opDef(Op role) { return opTable[Size(role)]; }

} // namespace intrinsic
