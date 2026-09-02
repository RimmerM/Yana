#pragma once

#include "type.h"

namespace mir {

struct Program;

/*
 * What a binary operation accepts and what it answers - the IR's own typing of a node.
 *
 * **Not the language's rule.** Which types `+` applies to is which types have a `Num` instance, and
 * that is instance selection's answer at the call - `Resolver::finishCall`, where an operator is a
 * call like any other. What is here is narrower and later: once selection has chosen a compiler
 * provided instance and its emitter builds an `add`, the node has to be well formed, and this is
 * the statement of what "well formed" is. `Builder::addBinary` asserts it; nothing reports it,
 * because by the time a node is built the program has already been checked.
 *
 * Five answers rather than a predicate per question, because the two questions asked of a node -
 * "may these operands stand here" and "what is the result" - have the same categories and would
 * otherwise be two tables that can disagree. `operandTypeAllowed` and `binaryResult` are the only
 * readers.
 *
 *  - `Arith` is `+ - * /`: integers and floats, and the result is the operand type.
 *  - `Bits` is `and or xor`: integers and `Bool`, result the operand type. A `Bool` is a one-bit
 *    value and the three are its connectives, which is why the library gives it `Bitwise`.
 *  - `Integer` is `%`: integers only, result the operand type.
 *  - `Shift` is `shl shr sar rol ror`: integers only, result the left operand's type. The count is
 *    the same type, which is what `Integral` declares.
 *  - `Equality` is `== !=`: anything a machine can compare bit for bit - a number, a pointer, `{}`,
 *    and an enum, whose value is its tag - answering `Bool`.
 *  - `Order` is `< <= > >=`: integers and floats, answering `Bool`.
 */
enum class OpCategory: U8 {
    Arith,
    Bits,
    Integer,
    Shift,
    Equality,
    Order,
};

enum class UnaryCategory: U8 {
    Arith,    // `-` and `convert`: integers and floats.
    Bits,     // `not`: integers and `Bool`.
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

// Whether a value of this type may stand as an operand of an operation in this category - the
// builder's assertion, and the emitters' guide. The program rather than the arena alone, because
// two of the answers name the truth type, which is a declaration the library made - `booleanType`.
bool operandTypeAllowed(Program& program, OpCategory category, DeclPtr<Type> type);
bool operandTypeAllowed(Program& program, UnaryCategory category, DeclPtr<Type> type);

} // namespace mir
