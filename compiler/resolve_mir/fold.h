#pragma once

#include "constant.h"
#include "op.h"
#include "type.h"
#include "../lower/lower_inst.h"
#include "../intrinsic/provided.h"

namespace mir {

struct Program;

/*
 * Arithmetic over `Constant`s - the third caller of `lower/fold_arith.h`, and the one that runs
 * before any code exists.
 *
 * `resolve_ssa/opt/fold.cpp` folds ownership SSA and `lower/lower_fold.cpp` folds the lower IR;
 * both fold what a *body* computes. This folds what a **constant position** computes -
 * `4 * 1024 * 1024` in a global's initializer, a default argument or a const generic argument -
 * which is a different tier and the same arithmetic. The two's complement itself is `fold_arith.h`'s
 * and stays there; what is here is the part that is about `Constant` rather than about a pattern:
 * which types may be folded at all, what the answer's normal form is, and the two tables that say
 * which operation an operator is.
 *
 * ## A target-answered width is folded only where the answer cannot depend on the target
 *
 * `Size` is 32 bits on one target and 64 on another, so `1 shl 40 :: Size` is two different numbers
 * and neither is this stage's to pick. What is still foldable is every expression whose operands and
 * whose result fit the width the language *guarantees* - `IntType::minBits` - because a value that
 * does not wrap at the narrowest target does not wrap at any of them and is the same number on all.
 * `wordBits - 1` is the shape this exists for. Anything else comes back unset, which the caller
 * reports as the position needing a constant.
 */

// The lower IR's row for one of this tier's operations at one signedness - `fold_arith.h`, whose
// kinds carry the signedness that `mir::BinaryOp` leaves to the operand type. False for an
// operation that roster has no row for, which is every comparison.
bool arithKind(BinaryOp op, bool isSigned, LowerInst::Kind& kind);

// The six orderings over two patterns, at the operands' own signedness. A signed narrow value is
// held sign-extended, so comparing as `I64` is comparing at the type's own width.
bool compareIntegers(BinaryOp op, U64 lhs, U64 rhs, bool isSigned);
bool compareFloats(BinaryOp op, double lhs, double rhs);

/*
 * Which operation a **generated** instance's member is - `provided.def`'s operator rows.
 *
 * One table, read from two sides: `expandProvided` builds the node and this folds the constant, and
 * a member that meant one operation in a body and another in a constant is the disagreement this
 * exists to prevent. `binary` says which of the two outputs is filled; false for a member that is
 * not one operation at all - `Bits.bitWidth` is an expression, and the conversions are their own
 * arm.
 */
bool providedOperation(intrinsic::Provided role, U16 member, BinaryOp& binary, UnaryOp& unary,
                       bool& isBinary);

/*
 * Two constants and an operation, folded - or unset where the answer is not one this may state.
 *
 * `result` is the operation's own result type, which is the operands' for arithmetic and `Bool` for
 * a comparison; the answer is put in that type's normal form, exactly as a written literal is.
 */
Constant foldConstantBinary(Program& program, BinaryOp op, const Constant& lhs, const Constant& rhs,
                            DeclPtr<Type> result);

Constant foldConstantUnary(Program& program, UnaryOp op, const Constant& value,
                           DeclPtr<Type> result);

} // namespace mir
