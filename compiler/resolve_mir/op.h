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

/*
 * What an operation does to a value **narrower than the register it is held in** - the width column
 * of `op.def`, and the whole of what the seam in `resolve_ssa/lower_width.cpp` reads.
 *
 * A `U8` is eight bits of a thirty-two bit register, so "the value" and "the register" are two
 * different quantities and the invariant that keeps them in step is that every producer leaves a
 * narrow value in **normal form** - sign-extended where the type is signed, zero-extended where it
 * is not. A comparison, a widening and a store all rest on it; an operation that broke it would not
 * be a slower program but a wrong one, and `(200 :: U8) + 100 == 44` is the shortest statement of
 * the difference.
 *
 * Four answers, because there are four treatments and no operation wants two of them:
 *
 *  - **`Closed`** - operands in range give a result in range, so there is nothing to do. `and`,
 *    `or`, `xor` and `sar` cannot set a bit the operands did not have between them; `rem` is
 *    bounded by its divisor; a comparison answers a `Bool`.
 *  - **`Wraps`** - the result may leave the range and is put back. The arithmetic four, `shl`, and
 *    the two unary operations whose business is the high bits: `neg` of the type's minimum and
 *    `not` of anything both reach past the declared width by construction.
 *  - **`Logical`** - `shr` alone, and it is its own answer because it is the one operation that
 *    reads a narrow value's *storage* rather than its value. A signed narrow value is held
 *    sign-extended, so shifting it right logically brings the register's own copies of the sign bit
 *    down into the answer; masking afterwards cannot recover them, so the operand is put in range
 *    first and the result is wrapped like any other.
 *  - **`Rotates`** - `rol` and `ror`, which are the two operations the machine has no instruction
 *    for at a narrow width at all: a rotate instruction turns within thirty-two or sixty-four bits
 *    whatever the program wrote, so the shift pair that is the *definition* of a rotation is what
 *    gets emitted instead.
 *
 * `Div` is `Wraps` for one pair only, and it is worth naming because it is not obvious: a quotient
 * of two in-range operands is in range except for the type's minimum over -1, whose defined answer
 * is the minimum wrapped back. The guard that produces it - `makeDivisionTotal`, below the lower IR
 * - negates the dividend, and a negated `-128` is `128` in the register it is held in.
 */
enum class WidthEffect: U8 {
    Closed,
    Wraps,
    Logical,
    Rotates,
};

enum class BinaryOp: U8 {
#define YANA_MIR_BINOP(kind, mnemonic, category, width) kind,
#define YANA_MIR_UNOP(kind, mnemonic, category, width)
#include "op.def"
#undef YANA_MIR_UNOP
#undef YANA_MIR_BINOP
    Count,
};

enum class UnaryOp: U8 {
#define YANA_MIR_BINOP(kind, mnemonic, category, width)
#define YANA_MIR_UNOP(kind, mnemonic, category, width) kind,
#include "op.def"
#undef YANA_MIR_UNOP
#undef YANA_MIR_BINOP
    Count,
};

StringView binaryOpName(BinaryOp op);
StringView unaryOpName(UnaryOp op);

OpCategory binaryCategory(BinaryOp op);
UnaryCategory unaryCategory(UnaryOp op);

// What each does to a value narrower than its register - see `WidthEffect`. Read by the seam in
// `resolve_ssa/lower_width.cpp` and by nothing else, because nowhere else is a register known.
WidthEffect binaryWidthEffect(BinaryOp op);
WidthEffect unaryWidthEffect(UnaryOp op);

// Whether a value of this type may stand as an operand of an operation in this category - the
// builder's assertion, and the emitters' guide. The program rather than the arena alone, because
// two of the answers name the truth type, which is a declaration the library made - `booleanType`.
bool operandTypeAllowed(Program& program, OpCategory category, DeclPtr<Type> type);
bool operandTypeAllowed(Program& program, UnaryCategory category, DeclPtr<Type> type);

} // namespace mir
