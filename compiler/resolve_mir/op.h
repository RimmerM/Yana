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
 *  - `Order` is `< <= > >=`: integers, floats and pointers, answering `Bool`. An address orders for
 *    the reason it compares - a region's bump pointer against its limit is the question - and
 *    arithmetic on one is still not here, because `p + n` scales by the pointee's stride.
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

    /*
     * `bitcast`: the same bits read as another type, so a **pointer** is admitted beside a number.
     *
     * Its own row rather than `Arith`'s, because the two ask different questions about the same
     * operand. A `convert` between a pointer and a float is nonsense whichever way round it is
     * written; a reinterpretation between an address and the word holding it is the raw boundary
     * itself, and `doc/spec/native.md` is where the language says an address *is* a number when you
     * ask it to be.
     *
     * What reaches it is the compiler's own arithmetic, not a program's: `p + n` is a
     * reinterpretation, an integer displacement and a reinterpretation back, which is what keeps
     * `add` homogeneous in an IR whose whole `OpCategory` table is about operands agreeing. Source
     * code reaches a pointer bitcast only through a `Bitcast` instance, and which pairs have one is
     * `provided.def`'s answer rather than this table's.
     */
    Raw,

    /*
     * `sqrt` and the four roundings: a float, or a vector of them, and nothing else.
     *
     * A category rather than `Arith` narrowed at each emitter, because what it admits is a real
     * question with a real answer: a square root of an integer is about rounding and no machine
     * answers it, and rounding an integer is the identity. `abs` is deliberately *not* here - the
     * magnitude of an integer is an integer, so it is `Arith` and takes both.
     */
    Float,
};

/*
 * An operation the machine has and this IR does not - the rows of native.def.
 *
 * Here rather than beside `NodeNative` because it belongs with the other two operation enums: what
 * `op.def` describes is arithmetic the IR can reason about, and what this describes is the boundary
 * where it stops. `resolve_ssa` carries the column down unchanged and the backend is what knows what
 * any of them means.
 */
enum class NativeOp: U8 {
#define YANA_MIR_NATIVE(kind, name) kind,
#include "native.def"
#undef YANA_MIR_NATIVE
};

// What a native operation is called in a dump - the second column of native.def.
inline StringView nativeOpName(NativeOp op) {
    switch(op) {
#define YANA_MIR_NATIVE(kind, name) case NativeOp::kind: return name;
#include "native.def"
#undef YANA_MIR_NATIVE
    }

    return "?"_v;
}

// Whether a native operation reads `NodeNative::immediate` - the two with a trailing byte a program
// chooses. See native.def.
inline bool nativeHasImmediate(NativeOp op) {
    return op == NativeOp::AesKeygenAssist || op == NativeOp::CarrylessMultiply
        || op == NativeOp::GetMantissa || op == NativeOp::ReduceFloat || op == NativeOp::RangeFloat;
}

/*
 * What an operation does to a value **narrower than the register it is held in** - the width column
 * of `op.def`, and the whole of what the seam in `resolve_ssa/lower/width.cpp` reads.
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

/*
 * How much of the rest of memory an atomic operation orders - Analysis-Atomics.md §3.3.
 *
 * The library's four ordering *types* exist so that `store(x, LoadAcquire)` does not typecheck,
 * which is a statement about what a caller may write; by the time an operation reaches the IR the
 * pairing is settled and what is left is the one fact every consumer reads.
 *
 * Mirrors `LowerOrder` one tier down, and the mirroring is deliberate rather than an omission: this
 * header sits above the lower IR and may not name it, so the translation is one switch in
 * `resolve_ssa`. What that buys is that a change to the lower IR's encoding cannot silently change
 * what a resolved program means.
 */
enum class AtomicOrder: U8 {
    Relaxed,
    Acquire,
    Release,
    AcquireRelease,
    Sequential,
};

/*
 * What a row of op.def's atomic block does with its operands - the third column there, where the
 * five shapes are written out.
 *
 * A shape rather than an arity, because two of the five agree on a count and disagree on what the
 * operands are: `Install` writes its value into the location and `Update` reads the location, folds
 * the value in and answers what was there. A reader that had only the count would have to test the
 * kind again to tell them apart, which is the table not being read.
 */
enum class AtomicForm: U8 {
    Create,
    Location,
    Install,
    Update,
    Compare,
    Bare,
};

enum class AtomicOp: U8 {
#define YANA_MIR_ATOMIC(kind, mnemonic, form) kind,
#include "op.def"
    Count,
};

// The mnemonic and the operand shape of one row - op.def's second and third columns.
StringView atomicOpName(AtomicOp op);
AtomicForm atomicForm(AtomicOp op);

// How many operands a shape takes, which is the one number a builder and a verifier both need and
// therefore the one place it is written.
inline Size atomicOperandCount(AtomicForm form) {
    switch(form) {
        case AtomicForm::Create:   return 1;
        case AtomicForm::Location: return 1;
        case AtomicForm::Install:  return 2;
        case AtomicForm::Update:   return 2;
        case AtomicForm::Compare:  return 3;
        case AtomicForm::Bare:     return 0;
    }

    return 0;
}

/*
 * §3.5's projection: what a failed comparison performs, given what a successful one does.
 *
 * A failure performs no write, so a release half has nothing to order and is dropped. It covers the
 * pairs an algorithm normally wants - release/relaxed and acquire-release/acquire - without asking
 * every caller two questions; `Advanced.compareExchange` is where a caller states both.
 */
inline AtomicOrder failureOrderFor(AtomicOrder success) {
    switch(success) {
        case AtomicOrder::Relaxed:        return AtomicOrder::Relaxed;
        case AtomicOrder::Acquire:        return AtomicOrder::Acquire;
        case AtomicOrder::Release:        return AtomicOrder::Relaxed;
        case AtomicOrder::AcquireRelease: return AtomicOrder::Acquire;
        case AtomicOrder::Sequential:     return AtomicOrder::Sequential;
    }

    return AtomicOrder::Sequential;
}

/*
 * §3.5's table. The failure order may not be stronger than the success order, where stronger is
 * `relaxed < acquire < sequential`: a compare-exchange whose *loss* published more than its win is
 * something no algorithm wants and no target implements without the strong form.
 */
inline bool isLegalFailureOrder(AtomicOrder success, AtomicOrder failure) {
    switch(success) {
        case AtomicOrder::Relaxed:
        case AtomicOrder::Release:
            return failure == AtomicOrder::Relaxed;
        case AtomicOrder::Acquire:
        case AtomicOrder::AcquireRelease:
            return failure == AtomicOrder::Relaxed || failure == AtomicOrder::Acquire;
        case AtomicOrder::Sequential:
            return true;
    }

    return false;
}

// What a dump prints for an order. One function, so a mode dump and a diagnostic spell an ordering
// the way `doc/spec/concurrency.md` does rather than two ways.
StringView atomicOrderName(AtomicOrder order);

/*
 * How a vector's lanes are combined into one scalar - op.def's reduction block, where each row is
 * argued and where the ordering guarantee is stated.
 */
enum class ReduceOp: U8 {
#define YANA_MIR_REDUCE(kind, mnemonic) kind,
#include "op.def"
    Count,
};

StringView reduceOpName(ReduceOp op);

// Whether a reduction answers a lane *index* rather than a value of the lane type - `FirstSet`
// alone, and the one thing about the enum a producer has to ask before stating a result type.
inline bool reducesToIndex(ReduceOp op) { return op == ReduceOp::FirstSet; }

enum class BinaryOp: U8 {
#define YANA_MIR_BINOP(kind, mnemonic, category, width) kind,
#include "op.def"
    Count,
};

enum class UnaryOp: U8 {
#define YANA_MIR_UNOP(kind, mnemonic, category, width) kind,
#include "op.def"
    Count,
};

StringView binaryOpName(BinaryOp op);
StringView unaryOpName(UnaryOp op);

OpCategory binaryCategory(BinaryOp op);
UnaryCategory unaryCategory(UnaryOp op);

/*
 * Whether an operation answers `Bool` about its two operands rather than a value of their type -
 * `Equality` or `Order`, which is the whole of the rule and is why it is here rather than restated
 * by each reader.
 *
 * Four of them were stating it: the verifier's result-type check, the folder's two rows, and
 * `opt/place.cpp`'s reading of a pointer comparison as a sink rather than as a carrier. Which
 * categories those are is a fact about `op.def` and about nothing else, so a category added to the
 * table is a line changed here instead of four sites to find.
 */
bool isComparison(BinaryOp op);

// What each does to a value narrower than its register - see `WidthEffect`. Read by the seam in
// `resolve_ssa/lower/width.cpp` and by nothing else, because nowhere else is a register known.
WidthEffect binaryWidthEffect(BinaryOp op);
WidthEffect unaryWidthEffect(UnaryOp op);

// Whether a value of this type may stand as an operand of an operation in this category - the
// builder's assertion, and the emitters' guide. The program rather than the arena alone, because
// two of the answers name the truth type, which is a declaration the library made - `booleanType`.
bool operandTypeAllowed(Program& program, OpCategory category, DeclPtr<Type> type);
bool operandTypeAllowed(Program& program, UnaryCategory category, DeclPtr<Type> type);

} // namespace mir
