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

        default:
            break;
    }

    return false;
}

/*
 * The width a constant of this type is folded at, and whether folding it is allowed at all.
 *
 * A fixed width answers itself. A **target-answered** one answers the width the language
 * guarantees - see the header - and the caller then has to hold the answer to that width as well,
 * which `holdsAtWidth` below is.
 */
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
 * A pattern into the constant its type holds - `reducedToWidth`, plus the guarantee above.
 *
 * `pattern` is the low `foldWidth` bits of the answer, which is what `evaluateBinary` hands back;
 * putting it into the type's normal form is this function's whole job, and the **type's own**
 * signedness is what decides whether that means sign-extending it. A target-answered width has no
 * `reducedToWidth` to do it - the number is left alone there, because normalizing at the widest
 * width it might turn out to be would be normalizing at a width no target necessarily has - so this
 * extends it itself and then asks whether the number is one every target holds.
 */
static Constant integerAnswer(const IntType& type, DeclPtr<Type> pointer, U64 pattern) {
    auto bits = foldWidth(type);
    auto value = type.isSigned ? U64(foldArith::signedValue(pattern, bits))
                               : (pattern & maskOf(bits));

    if(!type.isTargetWidth()) return intConstant(pointer, reducedToWidth(type, value));
    if(!holdsAtWidth(type, value)) return Constant();

    return intConstant(pointer, value);
}

// The operand as the number the arithmetic reads - the low `foldWidth` bits, which is what
// `evaluateBinary` documents it takes.
static U64 patternOf(const IntType& type, U64 value) { return value & maskOf(foldWidth(type)); }

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
    auto bits = foldWidth(integer);

    // Both operands have to be values of the type on every target before anything is computed with
    // them, which for a fixed width is always true and for `Size` is the whole of the rule.
    if(!holdsAtWidth(integer, lhs.asInt()) || !holdsAtWidth(integer, rhs.asInt())) return Constant();

    if(base[result]->kind != Type::Int) {
        return truthConstant(program, compareIntegers(op, lhs.asInt(), rhs.asInt(),
                                                      integer.isSigned));
    }

    LowerInst::Kind kind;
    if(!arithKind(op, integer.isSigned, kind)) return Constant();

    U64 answer;
    if(!evaluateBinary(kind, patternOf(integer, lhs.asInt()), patternOf(integer, rhs.asInt()), bits,
                       answer)) {
        return Constant();
    }

    // The result's own type rather than the operands', which are the same for every operation here
    // and would stop being so the day one of them narrows.
    return integerAnswer(*(IntType*)base[result], result, answer);
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
    auto bits = foldWidth(integer);
    if(!holdsAtWidth(integer, value.asInt())) return Constant();

    auto pattern = patternOf(integer, value.asInt());

    switch(op) {
        /*
         * Negation and complement are the two the operator classes name, and both are stated as the
         * arithmetic rather than as C's operators on a 64-bit register: a narrow type's `not` is
         * the complement of *its* bits and nothing above them, which is what the mask is for.
         */
        case UnaryOp::Neg: {
            U64 answer;
            if(!evaluateBinary(LowerInst::Sub, 0, pattern, bits, answer)) return Constant();
            return integerAnswer(*(IntType*)base[result], result, answer);
        }
        case UnaryOp::Not:
            return integerAnswer(*(IntType*)base[result], result, ~pattern & maskOf(bits));

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
