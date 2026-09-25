#pragma once

#include "lower_inst.h"

/*
 * Two's-complement arithmetic at a width - the one thing the two constant folders in this compiler
 * genuinely share, and the whole of what they share.
 *
 * `lower/lower_fold.cpp` folds the lower IR and `resolve_ssa/opt/fold.cpp` folds ownership SSA. What
 * an operation *means* is the same question in both: `add` of two patterns at `bits` is one answer
 * on every machine there is, and a signed division whose quotient the width cannot hold wraps back
 * to the minimum in both tiers because the language says so rather than because a target does.
 * Everything else about the two folders differs - one has logical types, refinements and
 * conventions and the other has registers and lanes - so this is the header and there is not going
 * to be a second one.
 *
 * ## Why the operation is a `LowerInst::Kind` and not something neutral
 *
 * Because a neutral enum would be a third roster to keep in step with the two that exist. The lower
 * IR's kinds already say the one thing an evaluator has to be told that `mir::BinaryOp` does not -
 * **the signedness** - since `Div`/`IDiv`, `Mul`/`IMul` and `MulHi`/`IMulHi` are separate rows there
 * and one row each above, where the operand *type* answers it. So the SSA folder maps its operation
 * and its operand's signedness onto a kind here, which is a table of fifteen rows, and gets one
 * implementation of the arithmetic rather than a second copy of it.
 *
 * ## What a caller owes, and what it gets back
 *
 * Both operands are read as the low `bits` of their patterns - `evaluateBinary` masks nothing on the
 * way in - so a caller whose values are held sign-extended at a narrower width must mask first. What
 * comes back is masked to `bits` and **not** sign-extended, because the lower tier holds a value in
 * its register's own width and has nothing narrower to extend to; the SSA tier's normal form is the
 * declared width sign-extended, and `narrowToWidth` there is what puts the answer into it.
 *
 * `false` means the answer is not one this may state - which, since division by zero and the
 * rotations are ruled on by the language, is only a shift by a distance at or above the width. Every
 * target masks such a count its own way and none of them is the language's answer.
 */

namespace foldArith {

// The low `bits` bits. The one spelling of it in the lower tier and the new stage's lowering since
// J163 - lower_store.cpp's `maskOfBits`, resolve_ssa/lower/repr.h's `lowMask`, three local `maskOf`s
// in lower_thread.cpp, lower_divide.cpp and lower_strength.cpp, and a merge in lower_promote.cpp
// written out inline were all this. The shipping stage's `compiler/resolve` and `compiler/opt` keep
// their own, since they go when that stage does.
inline U64 maskOf(U32 bits) {
    return bits >= 64 ? maxLimit<U64> : (U64(1) << bits) - 1;
}

// Whether exactly one bit is set - lower_split.cpp and resolve_ssa/lower/convention.cpp each had one.
inline bool isPowerOfTwo(U64 value) {
    return value && (value & (value - 1)) == 0;
}

// The same pattern read as a signed number of that width, which is what every operation that has a
// signed and an unsigned form needs and what a widening cast of a signed source produces.
inline I64 signedValue(U64 value, U32 bits) {
    if(bits >= 64) return I64(value);

    auto spare = 64 - bits;
    return I64(value << spare) >> spare;
}

/*
 * The top 64 bits of a 64x64 product, out of 32-bit pieces.
 *
 * By hand rather than through a wider integer type because there is no portable one: `__int128` is
 * a GNU extension and this builds under MSVC too. Four partial products, and the only subtle line
 * is the carry - the two cross terms and the top half of the low product all land in the same
 * 32-bit column, and their sum can reach into the next.
 */
inline U64 highProduct(U64 a, U64 b) {
    auto aLow = a & 0xffffffffu, aHigh = a >> 32;
    auto bLow = b & 0xffffffffu, bHigh = b >> 32;

    auto low = aLow * bLow;
    auto crossA = aHigh * bLow;
    auto crossB = aLow * bHigh;

    auto carry = ((low >> 32) + (crossA & 0xffffffffu) + (crossB & 0xffffffffu)) >> 32;
    return aHigh * bHigh + (crossA >> 32) + (crossB >> 32) + carry;
}

// The same half read as a signed number. The unsigned product treats each sign bit as magnitude
// worth 2^64, so a negative operand has contributed the other operand's whole value one place too
// high, and subtracting it once per negative operand is the correction.
inline I64 signedHighProduct(I64 a, I64 b) {
    auto high = I64(highProduct(U64(a), U64(b)));
    if(a < 0) high -= b;
    if(b < 0) high -= a;
    return high;
}

// Both operands known: the operation itself, at `bits`. False where the answer is not one this may
// state, which since the division rule below is only a shift by a distance the machine would have
// masked.
inline bool evaluateBinary(LowerInst::Kind kind, U64 a, U64 b, U32 bits, U64& into) {
    auto sa = signedValue(a, bits);
    auto sb = signedValue(b, bits);
    auto lowest = signedValue(U64(1) << (bits - 1), bits);

    switch(kind) {
        case LowerInst::Add: into = a + b; break;
        case LowerInst::Sub: into = a - b; break;

        // Signed and unsigned multiplication answer the same low bits, which is why the IR keeps the
        // two apart only because the machine does - see the LLVM backend, which merges them too.
        case LowerInst::Mul:
        case LowerInst::IMul: into = a * b; break;

        // `x / 0` is 0 and `x % 0` is `x`, which the language answers rather than the machine - see
        // the ruling beside `Div` in resolve/inst.def. There is no divisor these may not state.
        case LowerInst::Div: into = b ? a / b : 0; break;
        case LowerInst::Rem: into = b ? a % b : a; break;

        // The signed pair has one more: the quotient the width cannot hold, which wraps back to the
        // minimum as every other signed overflow does, and whose remainder is 0. Both are written
        // out rather than computed, `INT64_MIN / -1` being undefined in the C++ this runs in too.
        case LowerInst::IDiv:
            if(!sb) into = 0;
            else if(sa == lowest && sb == -1) into = U64(lowest);
            else into = U64(sa / sb);
            break;
        case LowerInst::IRem:
            if(!sb) into = a;
            else if(sa == lowest && sb == -1) into = 0;
            else into = U64(sa % sb);
            break;

        // The half of the product that does not fit, which is the one thing here that has to be
        // computed at *twice* the operands' width. A 32-bit pair fits a U64; a 64-bit pair is the
        // schoolbook split, because there is no wider integer to hold it.
        case LowerInst::MulHi:
            into = bits <= 32 ? (a * b) >> bits : highProduct(a, b);
            break;
        case LowerInst::IMulHi:
            into = bits <= 32 ? U64(sa * sb) >> bits : U64(signedHighProduct(sa, sb));
            break;

        case LowerInst::Shl: if(b >= bits) return false; into = a << b; break;
        case LowerInst::Shr: if(b >= bits) return false; into = a >> b; break;
        case LowerInst::Sar: if(b >= bits) return false; into = U64(sa >> b); break;

        /*
         * The rotations, which unlike the three above have an answer for *every* distance: the count
         * is defined modulo the width (see the kinds), so there is no out-of-range case to decline.
         *
         * `a` arrives masked to the width already - `foldBinaryValue`'s callers narrow it - so the
         * halves are the value's own bits and nothing above them travels. The `% bits` before the
         * shifts is the modulus and the `(bits - n) % bits` is what keeps a zero count from shifting
         * by the whole width, which is the one distance C++ leaves undefined.
         */
        case LowerInst::Rol: {
            auto n = b % bits;
            into = (a << n) | (a >> ((bits - n) % bits));
            break;
        }
        case LowerInst::Ror: {
            auto n = b % bits;
            into = (a >> n) | (a << ((bits - n) % bits));
            break;
        }

        case LowerInst::And: into = a & b; break;
        case LowerInst::Or:  into = a | b; break;
        case LowerInst::Xor: into = a ^ b; break;

        /*
         * The three BMI2 operations, which unlike the shifts have an answer for every operand pair -
         * see the kinds. The count saturates rather than being refused, and a permutation has no
         * count at all.
         */
        case LowerInst::BitsUpTo: into = b >= bits ? a : a & maskOf(U32(b)); break;

        case LowerInst::GatherBits: {
            U64 out = 0;
            U64 target = 1;

            for(auto mask = b & maskOf(bits); mask != 0; mask &= mask - 1) {
                if(a & (mask & (0 - mask))) out |= target;
                target <<= 1;
            }

            into = out;
            break;
        }

        case LowerInst::ScatterBits: {
            U64 out = 0;
            U64 source = 1;

            for(auto mask = b & maskOf(bits); mask != 0; mask &= mask - 1) {
                auto low = mask & (0 - mask);
                if(a & source) out |= low;
                source <<= 1;
            }

            into = out;
            break;
        }

        default: return false;
    }

    into &= maskOf(bits);
    return true;
}

} // namespace foldArith
