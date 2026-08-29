#pragma once

#include "type.h"

namespace mir {

/*
 * What a binary operation accepts and what it answers.
 *
 * Five answers rather than a predicate per question, because the two questions a type checker asks
 * of an operation - "may these operands stand here" and "what is the result" - have the same five
 * answers together and would otherwise be two tables that can disagree. `operandTypeAllowed` and
 * `binaryResult` below are the only readers.
 *
 *  - `Arith` is `+ - * /`: integers and floats, and the result is the operand type.
 *  - `Integer` is `% and or xor`: integers only, result the operand type.
 *  - `Shift` is `shl shr sar`: integers only, result the left operand's type. The count is the same
 *    type here, which is a milestone simplification rather than the language's rule - `Bits` takes
 *    the count as its own type, and that is an instance-selection question (milestone 6).
 *  - `Equality` is `== !=`: anything a machine can compare bit for bit, answering `Bool`.
 *  - `Order` is `< <= > >=`: integers and floats, answering `Bool`.
 */
enum class OpCategory: U8 {
    Arith,
    Integer,
    Shift,
    Equality,
    Order,
};

enum class UnaryCategory: U8 {
    Arith,    // `-`: integers and floats.
    Logical,  // `!`: `Bool` alone.
};

enum class BinaryOp: U8 {
#define YANA_MIR_BINOP(kind, mnemonic, category) kind,
#define YANA_MIR_UNOP(kind, mnemonic, category)
#include "op.def"
#undef YANA_MIR_UNOP
#undef YANA_MIR_BINOP
    Count,
};

enum class UnaryOp: U8 {
#define YANA_MIR_BINOP(kind, mnemonic, category)
#define YANA_MIR_UNOP(kind, mnemonic, category) kind,
#include "op.def"
#undef YANA_MIR_UNOP
#undef YANA_MIR_BINOP
    Count,
};

StringView binaryOpName(BinaryOp op);
StringView unaryOpName(UnaryOp op);

OpCategory binaryCategory(BinaryOp op);
UnaryCategory unaryCategory(UnaryOp op);

// Whether a scalar of this type may stand as an operand of an operation in this category. The
// question a diagnostic is written from, so it takes the logical type rather than a kind: the
// message names the type the user wrote.
bool operandTypeAllowed(DeclBase base, OpCategory category, DeclPtr<Type> type);
bool operandTypeAllowed(DeclBase base, UnaryCategory category, DeclPtr<Type> type);

} // namespace mir
