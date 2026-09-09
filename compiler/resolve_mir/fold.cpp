#include "fold.h"
#include "decl.h"
#include "../lower/fold_arith.h"

namespace mir {

using foldArith::evaluateBinary;
using foldArith::maskOf;

bool arithKind(BinaryOp op, bool isSigned, LowerInst::Kind& kind) {
    switch(op) {
        case BinaryOp::Add: kind = LowerInst::Add; return true;
        case BinaryOp::Sub: kind = LowerInst::Sub; return true;
        case BinaryOp::Mul: kind = isSigned ? LowerInst::IMul : LowerInst::Mul; return true;
        case BinaryOp::Div: kind = isSigned ? LowerInst::IDiv : LowerInst::Div; return true;
        case BinaryOp::Rem: kind = isSigned ? LowerInst::IRem : LowerInst::Rem; return true;
        case BinaryOp::MulHi: kind = isSigned ? LowerInst::IMulHi : LowerInst::MulHi; return true;
        case BinaryOp::And: kind = LowerInst::And; return true;
        case BinaryOp::Or:  kind = LowerInst::Or; return true;
        case BinaryOp::Xor: kind = LowerInst::Xor; return true;
        case BinaryOp::Shl: kind = LowerInst::Shl; return true;
        case BinaryOp::Shr: kind = LowerInst::Shr; return true;
        case BinaryOp::Sar: kind = LowerInst::Sar; return true;
        case BinaryOp::Rol: kind = LowerInst::Rol; return true;
        case BinaryOp::Ror: kind = LowerInst::Ror; return true;
        case BinaryOp::BitsUpTo: kind = LowerInst::BitsUpTo; return true;
        default: return false;
    }
}

bool compareIntegers(BinaryOp op, U64 lhs, U64 rhs, bool isSigned) {
    if(isSigned) {
        auto a = I64(lhs), b = I64(rhs);
        switch(op) {
            case BinaryOp::Eq: return a == b;
            case BinaryOp::Ne: return a != b;
            case BinaryOp::Lt: return a < b;
            case BinaryOp::Le: return a <= b;
            case BinaryOp::Gt: return a > b;
            case BinaryOp::Ge: return a >= b;
            default: return false;
        }
    }

    switch(op) {
        case BinaryOp::Eq: return lhs == rhs;
        case BinaryOp::Ne: return lhs != rhs;
        case BinaryOp::Lt: return lhs < rhs;
        case BinaryOp::Le: return lhs <= rhs;
        case BinaryOp::Gt: return lhs > rhs;
        case BinaryOp::Ge: return lhs >= rhs;
        default: return false;
    }
}

bool compareFloats(BinaryOp op, double lhs, double rhs) {
    switch(op) {
        case BinaryOp::Eq: return lhs == rhs;
        case BinaryOp::Ne: return lhs != rhs;
        case BinaryOp::Lt: return lhs < rhs;
        case BinaryOp::Le: return lhs <= rhs;
        case BinaryOp::Gt: return lhs > rhs;
        case BinaryOp::Ge: return lhs >= rhs;
        default: return false;
    }
}

bool providedOperation(intrinsic::Provided role, U16 member, BinaryOp& binary, UnaryOp& unary,
                       bool& isBinary) {
    using intrinsic::Provided;

    auto asBinary = [&](BinaryOp op) { binary = op; isBinary = true; return true; };
    auto asUnary = [&](UnaryOp op) { unary = op; isBinary = false; return true; };

    switch(role) {
        case Provided::eq:
            switch(member) {
                case 0: return asBinary(BinaryOp::Eq);
                case 1: return asBinary(BinaryOp::Ne);
            }
            break;

        case Provided::ord:
            switch(member) {
                case 0: return asBinary(BinaryOp::Lt);
                case 1: return asBinary(BinaryOp::Le);
                case 2: return asBinary(BinaryOp::Gt);
                case 3: return asBinary(BinaryOp::Ge);
            }
            break;

        case Provided::num:
            switch(member) {
                case 0: return asBinary(BinaryOp::Add);
                case 1: return asBinary(BinaryOp::Sub);
                case 2: return asBinary(BinaryOp::Mul);
                case 3: return asBinary(BinaryOp::Div);
                case 4: return asUnary(UnaryOp::Neg);
            }
            break;

        case Provided::bitwise:
            switch(member) {
                case 0: return asBinary(BinaryOp::And);
                case 1: return asBinary(BinaryOp::Or);
                case 2: return asBinary(BinaryOp::Xor);
                case 3: return asUnary(UnaryOp::Not);
            }
            break;

        case Provided::integral:
            switch(member) {
                case 0: return asBinary(BinaryOp::Rem);
                case 1: return asBinary(BinaryOp::Shl);
                case 2: return asBinary(BinaryOp::Shr);
                case 3: return asBinary(BinaryOp::Sar);
                case 4: return asBinary(BinaryOp::Rol);
                case 5: return asBinary(BinaryOp::Ror);
            }
            break;

        /*
         * `Bits`' first four rows are one instruction each and the fifth is not - `bitWidth` is a
         * width minus a leading-zero count, which is an expression and stays `expandProvided`'s.
         */
        case Provided::bits:
            switch(member) {
                case 0: return asUnary(UnaryOp::CountBits);
                case 1: return asUnary(UnaryOp::LeadingZeros);
                case 2: return asUnary(UnaryOp::TrailingZeros);
                case 3: return asBinary(BinaryOp::BitsUpTo);
            }
            break;

        // `ByteSwap`'s one member, which is one node.
        case Provided::byteSwap:
            if(member == 0) return asUnary(UnaryOp::ByteSwap);
            break;

        // `BitPermute`'s two, in `lib/Core/Bits.yana`'s declaration order - see `known.def`'s row,
        // which is where that dependency is stated.
        case Provided::bitPermute:
            switch(member) {
                case 0: return asBinary(BinaryOp::GatherBits);
                case 1: return asBinary(BinaryOp::ScatterBits);
            }
            break;

        default:
            break;
    }

    return false;
}

/*
 * The two widths a constant of this type may be computed at.
 *
 * A fixed width answers itself twice, and that is what makes the rule below one rule rather than
 * two: the whole arithmetic runs at `narrow` and again at `wide`, and a type that states its own
 * width runs the same computation twice and agrees with itself for nothing. A **target-answered**
 * one - `Size`, `USize`, `CodeUnit` - answers the width the language guarantees and the widest it
 * may turn out to be, and the two runs are what says whether the target gets to matter.
 */
struct FoldWidths {
    U32 narrow = 0;
    U32 wide = 0;

    bool differ() const { return narrow != wide; }
};

static FoldWidths foldWidths(const IntType& type) {
    return FoldWidths { U32(type.minBits()), U32(type.maxBits()) };
}

// The width an operand is read at when only one is wanted - the guaranteed one, which is the width
// every value of the type holds a number at on every target.
static U32 foldWidth(const IntType& type) { return U32(type.minBits()); }

// Whether a pattern read at `bits` is a value the type holds on **every** target - the check a
// target-answered width owes and a fixed one gets for nothing, since there the two widths are one.
static bool holdsAtWidth(const IntType& type, U64 value) {
    auto bits = foldWidth(type);
    if(bits >= 64) return true;

    auto reduced = foldArith::signedValue(value & maskOf(bits), bits);
    if(type.isSigned) return U64(reduced) == value;

    return (value & ~maskOf(bits)) == 0;
}

/*
 * A pattern computed at `bits` into the number the type holds - the normal form, at that width.
 *
 * `pattern` is the low `bits` of the answer, which is what `evaluateBinary` hands back; putting it
 * into the type's normal form is this function's whole job, and the **type's own** signedness is
 * what decides whether that means sign-extending it.
 */
static U64 normalizedAt(const IntType& type, U32 bits, U64 pattern) {
    return type.isSigned ? U64(foldArith::signedValue(pattern, bits)) : (pattern & maskOf(bits));
}

/*
 * The answer, from the two the arithmetic produced at the two widths the result type may have.
 *
 * **The two have to agree, and a target-answered width is the whole reason there are two.** The
 * rule this replaces computed at the guaranteed width alone and then asked whether the number it
 * got was one every target holds - which is a different question, and a weaker one. `not (0 ::
 * USize)` is `0xffffffff` computed at thirty-two bits and is a number a `USize` holds on every
 * target, so that rule wrote it down; on the sixty-four-bit target this compiler emits for the
 * answer is `0xffffffffffffffff`, and the program then read a folded constant its own machine
 * disagreed with. The same hole is under `neg` of an unsigned, a `sub` that goes below zero, and a
 * `shl` that carries a bit past thirty-two.
 *
 * Agreement is the honest statement of what a constant at this tier *is*: `optimizeProgram` runs
 * once per program and `manifestNative` is the one place a target's width is known, so a number
 * written down here has to be the number on every target the program could be built for. Where the
 * two disagree there is no such number and the fold declines - which costs the wraparound cases
 * their fold and nothing else, since a value that came from source already holds at the guaranteed
 * width (`integerHolds`) and every operation that stays inside it agrees at both.
 */
static Constant integerAnswer(const IntType& type, DeclPtr<Type> pointer, U64 narrow, U64 wide) {
    auto widths = foldWidths(type);
    auto value = normalizedAt(type, widths.narrow, narrow);

    if(!widths.differ()) return intConstant(pointer, reducedToWidth(type, value));
    if(value != normalizedAt(type, widths.wide, wide)) return Constant();

    return intConstant(pointer, value);
}

// The same, where the arithmetic was width-independent and produced one pattern - a bit count of a
// fixed width, a bitcast. Handing it to both runs is the statement that the answer is the same one.
static Constant integerAnswer(const IntType& type, DeclPtr<Type> pointer, U64 pattern) {
    return integerAnswer(type, pointer, pattern, pattern);
}

// The operand as the number the arithmetic reads at `bits` - the low `bits` of it, which is what
// `evaluateBinary` documents it takes.
static U64 patternAt(U32 bits, U64 value) { return value & maskOf(bits); }

Constant foldConstantBinary(Program& program, BinaryOp op, const Constant& lhs, const Constant& rhs,
                            DeclPtr<Type> result) {
    auto base = *program.decl;
    if(!lhs.isSet() || !rhs.isSet() || !result) return Constant();
    if(lhs.type != rhs.type) return Constant();

    auto operand = lhs.type;

    if(base[operand]->kind == Type::Float) {
        if(lhs.kind != Constant::Float || rhs.kind != Constant::Float) return Constant();

        auto narrow = ((FloatType*)base[operand])->bits == 32;
        auto a = lhs.asFloat();
        auto b = rhs.asFloat();

        if(base[result]->kind != Type::Float) {
            return truthConstant(program, compareFloats(op, a, b));
        }

        // At `F32` the arithmetic is done in single precision, because that is what the program
        // will do: folding in double and rounding once produces a number the machine does not
        // agree with, which is the one way a fold can be wrong rather than absent.
        auto value = 0.0;
        switch(op) {
            case BinaryOp::Add: value = narrow ? F32(F32(a) + F32(b)) : a + b; break;
            case BinaryOp::Sub: value = narrow ? F32(F32(a) - F32(b)) : a - b; break;
            case BinaryOp::Mul: value = narrow ? F32(F32(a) * F32(b)) : a * b; break;
            case BinaryOp::Div: value = narrow ? F32(F32(a) / F32(b)) : a / b; break;
            default: return Constant();
        }

        return floatConstant(result, value);
    }

    if(base[operand]->kind != Type::Int) return Constant();
    if(lhs.kind != Constant::Int || rhs.kind != Constant::Int) return Constant();

    auto& integer = *(IntType*)base[operand];
    auto widths = foldWidths(integer);

    // Both operands have to be values of the type on every target before anything is computed with
    // them, which for a fixed width is always true and for `Size` is what makes the narrow run of
    // the arithmetic below a run over the operands' real values rather than over truncations.
    if(!holdsAtWidth(integer, lhs.asInt()) || !holdsAtWidth(integer, rhs.asInt())) return Constant();

    if(base[result]->kind != Type::Int) {
        return truthConstant(program, compareIntegers(op, lhs.asInt(), rhs.asInt(),
                                                      integer.isSigned));
    }

    LowerInst::Kind kind;
    if(!arithKind(op, integer.isSigned, kind)) return Constant();

    // Once at each width the operand type may have - see `integerAnswer`, which is where the two
    // are compared and where the reason they have to agree is written down.
    U64 narrow, wide;
    if(!evaluateBinary(kind, patternAt(widths.narrow, lhs.asInt()),
                       patternAt(widths.narrow, rhs.asInt()), widths.narrow, narrow)) {
        return Constant();
    }

    if(!evaluateBinary(kind, patternAt(widths.wide, lhs.asInt()),
                       patternAt(widths.wide, rhs.asInt()), widths.wide, wide)) {
        return Constant();
    }

    // The result's own type rather than the operands', which are the same for every operation here
    // and would stop being so the day one of them narrows.
    return integerAnswer(*(IntType*)base[result], result, narrow, wide);
}

Constant foldConstantUnary(Program& program, UnaryOp op, const Constant& value,
                           DeclPtr<Type> result) {
    auto base = *program.decl;
    if(!value.isSet() || !result) return Constant();

    if(base[value.type]->kind == Type::Float) {
        if(value.kind != Constant::Float || op != UnaryOp::Neg) return Constant();
        return floatConstant(result, -value.asFloat());
    }

    if(base[value.type]->kind != Type::Int || value.kind != Constant::Int) return Constant();

    auto& integer = *(IntType*)base[value.type];
    auto widths = foldWidths(integer);
    auto bits = widths.narrow;
    if(!holdsAtWidth(integer, value.asInt())) return Constant();

    auto pattern = patternAt(widths.narrow, value.asInt());
    auto widePattern = patternAt(widths.wide, value.asInt());

    switch(op) {
        /*
         * Negation and complement are the two the operator classes name, and both are stated as the
         * arithmetic rather than as C's operators on a 64-bit register: a narrow type's `not` is
         * the complement of *its* bits and nothing above them, which is what the mask is for.
         *
         * Both are computed at both widths, and both are the operations that made that necessary:
         * the complement of a `USize` zero and the negation of one are the two whose answer *is*
         * the width. See `integerAnswer`.
         */
        case UnaryOp::Neg: {
            U64 narrow, wide;
            if(!evaluateBinary(LowerInst::Sub, 0, pattern, widths.narrow, narrow)) return Constant();
            if(!evaluateBinary(LowerInst::Sub, 0, widePattern, widths.wide, wide)) return Constant();
            return integerAnswer(*(IntType*)base[result], result, narrow, wide);
        }
        case UnaryOp::Not:
            return integerAnswer(*(IntType*)base[result], result,
                                 ~pattern & maskOf(widths.narrow),
                                 ~widePattern & maskOf(widths.wide));

        /*
         * The bit counts, which are questions about the *declared* width - so a target-answered one
         * has no answer here at all: `leadingZeros(0 :: USize)` is 32 on one target and 64 on
         * another, and that is exactly what `wordBits` exists to say instead.
         */
        case UnaryOp::CountBits:
        case UnaryOp::LeadingZeros:
        case UnaryOp::TrailingZeros: {
            if(integer.isTargetWidth()) return Constant();

            U32 count = 0;
            if(op == UnaryOp::CountBits) {
                for(U32 i = 0; i < bits; i++) count += U32((pattern >> i) & 1);
            } else if(op == UnaryOp::LeadingZeros) {
                while(count < bits && !((pattern >> (bits - 1 - count)) & 1)) count++;
            } else {
                while(count < bits && !((pattern >> count) & 1)) count++;
            }

            return integerAnswer(*(IntType*)base[result], result, U64(count));
        }

        // `bitcast` keeps the bits and changes the type, which at a constant is exactly that. A
        // `convert` is `foldLiteral`'s and reaches this function through no caller.
        case UnaryOp::Bitcast:
            if(base[result]->kind != Type::Int) return Constant();
            return integerAnswer(*(IntType*)base[result], result, value.asInt());

        default:
            return Constant();
    }
}

} // namespace mir
