#include "lower_strength.h"
#include "lower_builder.h"

namespace {

// The width the arithmetic below works at, which for a vector is one lane's: every rewrite here is
// lane-wise, and a reciprocal is computed for the lane and not for the register.
U32 widthOf(LowerType type) {
    return laneBytes(type.lane) * 8;
}

/*
 * Whether the general route - a reciprocal and the top half of its product - may be taken at this
 * type.
 *
 * Always, for a scalar: every machine that has a multiply produces the high half beside the low one.
 * For a **vector** it is a real question, and the answer is the machine's rather than this pass's: a
 * packed widening product exists at a 16-bit lane on both native targets, and at a 32- and a 64-bit
 * lane it is built out of the even-lane multiply x86 has and ARM has a widening pair for. **A byte
 * lane is the one that is left out**, and not for want of an instruction - `pmullw` would serve -
 * but because a reciprocal at eight bits is a multiply, two shifts and a correction against a
 * division whose divisor fits in a byte, and nothing in the corpus asks.
 *
 * Below the widths named here a division by a constant stays a division, which the backend then
 * answers however it answers one - a target with more than this loses an optimization rather than
 * correctness, which is the right way round for a claim about machines in general made in a file
 * that names none of them.
 *
 * The quadword is the one that was measured rather than argued: the sequence is eight instructions
 * of addition around four widening products, against the two `idiv r64` it replaces, and it comes
 * out 2.5x ahead signed and 4.0x unsigned at 128 bits with nothing hoisted - see §60.7 of
 * test/bench/findings.md.
 *
 * The power-of-two cases are not gated: they are shifts, and every lane width has those.
 */
bool hasWideningProduct(LowerType type) {
    if(!isVectorLike(type)) return true;

    auto bits = laneBytes(type.lane) * 8;
    return bits == 16 || bits == 32 || bits == 64;
}

U64 maskOf(U32 bits) {
    return bits >= 64 ? maxLimit<U64> : (U64(1) << bits) - 1;
}

I64 signedValue(U64 value, U32 bits) {
    if(bits >= 64) return I64(value);

    auto spare = 64 - bits;
    return I64(value << spare) >> spare;
}

// The position of the only set bit, or nothing where there is not exactly one. Zero is not a power
// of two here and neither is 1: a division or a multiplication by one has already been forwarded.
Maybe<U32> powerOfTwo(U64 value) {
    if(value < 2 || (value & (value - 1))) return Nothing();

    U32 bit = 0;
    while(!(value & (U64(1) << bit))) bit++;

    return Just(bit);
}

/*
 * The reciprocal of an unsigned divisor: a multiplier, a shift, and whether the multiplier needed
 * one bit more than the type has.
 *
 * Hacker's Delight figure 10-3, generalized from 32 bits to `bits` and stated in U64 with every
 * intermediate masked back down, so the same routine answers for both widths. What it computes is
 * the smallest `M` and `s` for which `(x * M) >> (bits + s)` is `x / d` for *every* x the type can
 * hold - which is why it is a search rather than a formula: the error of a rounded reciprocal has
 * to be bounded across the whole range, and `q1`/`r1` are what tracks that bound.
 *
 * `add` is the overflow case. Where the multiplier does not fit in `bits`, the top bit is implicit
 * and the sequence has to put it back, which costs three instructions instead of one - see
 * `reduceUnsignedDivide`. It is still far cheaper than a divider.
 */
struct Reciprocal {
    U64 multiplier;
    U32 shift;
    bool add;
};

Reciprocal unsignedReciprocal(U64 d, U32 bits) {
    auto mask = maskOf(bits);
    auto high = U64(1) << (bits - 1);          // 2^(bits-1)
    auto ones = high - 1;                      // 2^(bits-1) - 1

    // The largest dividend for which the quotient must still be exact, less its remainder: the
    // bound the search below has to stay inside.
    auto nc = (mask - ((mask - d + 1) & mask) % d) & mask;

    auto p = bits - 1;
    auto q1 = high / nc, r1 = high - q1 * nc;
    auto q2 = ones / d,  r2 = ones - q2 * d;
    auto add = false;
    U64 delta;

    do {
        p++;

        if(r1 >= nc - r1) {
            q1 = (2 * q1 + 1) & mask;
            r1 = 2 * r1 - nc;
        } else {
            q1 = (2 * q1) & mask;
            r1 = 2 * r1;
        }

        if(r2 + 1 >= d - r2) {
            if(q2 >= ones) add = true;
            q2 = (2 * q2 + 1) & mask;
            r2 = 2 * r2 + 1 - d;
        } else {
            if(q2 >= high) add = true;
            q2 = (2 * q2) & mask;
            r2 = 2 * r2 + 1;
        }

        delta = d - 1 - r2;
    } while(p < 2 * bits && (q1 < delta || (q1 == delta && r1 == 0)));

    return Reciprocal { (q2 + 1) & mask, p - bits, add };
}

/*
 * The same for a signed divisor, from figure 10-1, over the absolute values with the sign carried
 * into the multiplier at the end. `add` has no counterpart here - the correction a signed sequence
 * needs is not an overflow but the rounding: a quotient computed this way is one too low for a
 * negative dividend, which `reduceSignedDivide` fixes by adding back the sign bit.
 *
 * Never asked about -1, 0 or 1: all three are answered before this is reached.
 */
Reciprocal signedReciprocal(I64 d, U32 bits) {
    auto mask = maskOf(bits);
    auto high = U64(1) << (bits - 1);

    // Negated as an unsigned pattern rather than as a number: the type's lowest value has no
    // positive counterpart, and `-d` on one is undefined rather than merely wrong.
    auto negative = d < 0;
    auto ad = negative ? U64(0) - U64(d) : U64(d);

    // 2^(bits-1) for a positive divisor and one more for a negative one: the dividend furthest from
    // zero in the direction the quotient rounds.
    auto t = high + (negative ? 1 : 0);
    auto anc = t - 1 - (t % ad);

    auto p = bits - 1;
    auto q1 = high / anc, r1 = high - q1 * anc;
    auto q2 = high / ad,  r2 = high - q2 * ad;
    U64 delta;

    do {
        p++;

        q1 = (2 * q1) & mask;
        r1 = 2 * r1;
        if(r1 >= anc) { q1 = (q1 + 1) & mask; r1 -= anc; }

        q2 = (2 * q2) & mask;
        r2 = 2 * r2;
        if(r2 >= ad) { q2 = (q2 + 1) & mask; r2 -= ad; }

        delta = ad - r2;
    } while(q1 < delta || (q1 == delta && r1 == 0));

    auto multiplier = (q2 + 1) & mask;
    if(negative) multiplier = (U64(0) - multiplier) & mask;

    return Reciprocal { multiplier, p - bits, false };
}

/*
 * Instructions built into the middle of a block that is being rewritten.
 *
 * `LowerBlock::addInst` appends, and the list is being rebuilt from the front, so an instruction
 * that has to stand exactly where the one it replaces stood cannot go through it. This does the two
 * things that matter and skips the rest: it claims the block, and it registers each operand's use,
 * which is the half a rewrite most easily forgets - `lower_validate.cpp` checks both directions.
 */
struct Reducer {
    LowerBase base;
    LowerModule& module;
    LowerPtr<LowerBlock> block;
    LocationId source;
    SmallArray<LowerPtr<LowerInst>, 32>& into;

    LowerValue* place(LowerInst* inst) {
        inst->block = block;
        inst->source = source;

        for(auto use: inst->used()) base[use]->uses.push(module.arena, inst - base);

        into.push(inst - base);
        return &((LowerInstSingle*)inst)->result;
    }

    LowerValue* imm(LowerType type, U64 value, StringId name = StringId {}) {
        // The same number at whatever shape the operation works at: one immediate for a scalar, and
        // a splat of one for a vector, which every backend and every pooling pass reads as the
        // constant it is. A bare `LowerImm` of a vector type is not a thing this IR has.
        if(isVectorLike(type)) {
            auto scalar = place(new (module.arena) LowerImm(StringId {}, scalarFormOf(type), value));
            return place(new (module.arena) LowerInstVecSplat(name, type, scalar - base));
        }

        return place(new (module.arena) LowerImm(name, type, value));
    }

    LowerValue* op(LowerInst::Kind kind, LowerValue* lhs, LowerValue* rhs, StringId name = StringId {}) {
        return place(new (module.arena) LowerInstBinary(name, lhs->type, lhs - base, rhs - base, kind));
    }

    // The right-hand side of everything below is a literal, so this is what almost every step is.
    LowerValue* opImm(LowerInst::Kind kind, LowerValue* lhs, U64 value, StringId name = StringId {}) {
        return op(kind, lhs, imm(lhs->type, value), name);
    }

    LowerValue* neg(LowerValue* from, StringId name = StringId {}) {
        return place(new (module.arena) LowerInstUnary(LowerInst::Neg, name, from->type, from - base));
    }
};

/*
 * `x / d` for an unsigned `d`, which is a shift where `d` is a power of two and the top half of a
 * reciprocal product otherwise.
 *
 * The `add` sequence is the one worth reading twice. Where the multiplier needs `bits + 1` bits, the
 * product this computes is short by `x * 2^bits`, which is `x` at this half's scale; adding it back
 * would itself overflow, so half the difference is added instead and the shift is one shorter.
 */
LowerValue* reduceUnsignedDivide(Reducer& r, LowerValue* x, U64 d, U32 bits, StringId name) {
    if(auto shift = powerOfTwo(d)) {
        return r.opImm(LowerInst::Shr, x, shift.unwrap(), name);
    }

    auto reciprocal = unsignedReciprocal(d, bits);
    auto high = r.op(LowerInst::MulHi, x, r.imm(x->type, reciprocal.multiplier));

    if(!reciprocal.add) {
        if(!reciprocal.shift) return high;
        return r.opImm(LowerInst::Shr, high, reciprocal.shift, name);
    }

    auto difference = r.op(LowerInst::Sub, x, high);
    auto halved = r.opImm(LowerInst::Shr, difference, 1);
    auto sum = r.op(LowerInst::Add, halved, high);
    return r.opImm(LowerInst::Shr, sum, reciprocal.shift - 1, name);
}

/*
 * `x / d` for a signed `d`.
 *
 * Both forms end the same way, and it is the same correction in both: an arithmetic shift rounds
 * toward negative infinity and a division rounds toward zero, so a negative dividend that did not
 * divide exactly comes out one too low. Adding the result's own sign bit back is what fixes it, and
 * it is why the power-of-two case is four instructions rather than one.
 *
 * For the reciprocal form there is a second correction, and it is about the multiplier rather than
 * the dividend: where `M` does not fit in a signed word its top bit was dropped, and the operand it
 * was multiplied by has to be added back at the product's high half - which is `x` itself.
 */
LowerValue* reduceSignedDivide(Reducer& r, LowerValue* x, I64 d, U32 bits, StringId name) {
    auto negative = d < 0;
    auto magnitude = negative ? U64(0) - U64(d) : U64(d);

    auto signBit = [&](LowerValue* value) {
        return r.opImm(LowerInst::Shr, value, bits - 1);
    };

    if(auto power = powerOfTwo(magnitude)) {
        auto shift = power.unwrap();

        // The bias: `2^shift - 1` where the dividend is negative and zero where it is not, reached
        // without a branch by broadcasting the sign bit and then dropping all but `shift` of it.
        auto ones = r.opImm(LowerInst::Sar, x, bits - 1);
        auto bias = r.opImm(LowerInst::Shr, ones, bits - shift);
        auto biased = r.op(LowerInst::Add, x, bias);
        auto quotient = r.opImm(LowerInst::Sar, biased, shift, negative ? StringId {} : name);

        return negative ? r.neg(quotient, name) : quotient;
    }

    auto reciprocal = signedReciprocal(d, bits);
    auto product = r.op(LowerInst::IMulHi, x, r.imm(x->type, reciprocal.multiplier));
    auto multiplier = signedValue(reciprocal.multiplier, bits);

    // The two sides of the dropped top bit, and only one of them can apply: a positive divisor whose
    // multiplier came out negative lost a bit off the top, and a negative divisor whose multiplier
    // came out positive lost one the same way with the sign the other way round.
    if(!negative && multiplier < 0) product = r.op(LowerInst::Add, product, x);
    else if(negative && multiplier > 0) product = r.op(LowerInst::Sub, product, x);

    auto shifted = reciprocal.shift ? r.opImm(LowerInst::Sar, product, reciprocal.shift) : product;
    return r.op(LowerInst::Add, shifted, signBit(shifted), name);
}

/*
 * What one instruction becomes, or null where it stays as it is.
 *
 * A remainder is the division and the multiplication back, everywhere except at a power of two -
 * where the unsigned case is a mask and the signed case is the same biased value the division uses,
 * with its low bits cleared instead of shifted away.
 */
LowerValue* reduce(Reducer& r, LowerInstBinary* inst) {
    auto type = inst->result.type;
    auto x = r.base[inst->lhs];

    /*
     * Integers, scalar or packed. Every rewrite below is lane-wise arithmetic over shifts, adds and
     * products, so a vector needs no shape of its own - what it needs is that its constants are
     * splats and that its reciprocal is computed for one *lane*, both of which are above.
     */
    if(!isInt(type) && !isIntVector(type)) return nullptr;
    if(x->type != type) return nullptr;

    // Read through the splat the language's spelling wraps a divisor in: `Integral(a)` types both
    // operands as the same `a`, so `v / 3` over a vector arrives as a division by `vsplat(3)`.
    auto divisor = r.base[inst->rhs];
    auto literal = divisor->inst();
    if(literal->kind == LowerInst::VecSplat) literal = r.base[((LowerInstVecSplat*)literal)->from]->inst();

    if(literal->kind != LowerInst::Imm || divisor->type != type) return nullptr;

    auto bits = widthOf(type);
    auto name = inst->result.name;
    auto d = ((LowerImm*)literal)->i & maskOf(bits);
    auto sd = signedValue(d, bits);

    switch(inst->kind) {
        /*
         * A shift where the multiplier is a power of two, and the same shift negated where it is a
         * negative one - of which -1 is the case where the shift is nothing at all and only the
         * negation is left.
         *
         * The sign is read out of the two's-complement pattern rather than off the instruction: the
         * low half of a product is the same bits whichever way the operands are read, so `mul` by
         * `2^bits - 2^k` and `imul` by `-2^k` are one case and not two. Nothing here overflows into
         * a different answer than the multiply gave - a negation wraps exactly as the product does,
         * including on the type's lowest value.
         */
        case LowerInst::Mul:
        case LowerInst::IMul: {
            if(auto shift = powerOfTwo(d)) return r.opImm(LowerInst::Shl, x, shift.unwrap(), name);

            auto magnitude = (U64(0) - d) & maskOf(bits);
            if(magnitude == 1) return r.neg(x, name);
            if(auto shift = powerOfTwo(magnitude)) return r.neg(r.opImm(LowerInst::Shl, x, shift.unwrap()), name);

            return nullptr;
        }

        case LowerInst::Div:
            if(d < 2) return nullptr;
            if(!powerOfTwo(d) && !hasWideningProduct(type)) return nullptr;
            return reduceUnsignedDivide(r, x, d, bits, name);

        case LowerInst::Rem: {
            if(d < 2) return nullptr;
            if(powerOfTwo(d)) return r.opImm(LowerInst::And, x, d - 1, name);
            if(!hasWideningProduct(type)) return nullptr;

            auto quotient = reduceUnsignedDivide(r, x, d, bits, StringId {});
            return r.op(LowerInst::Sub, x, r.opImm(LowerInst::Mul, quotient, d), name);
        }

        // -1 is rewritten now that the language says what its quotient is - `neg` on the type's
        // lowest value wraps back to it, which is the answer rather than an accident. 0 is still
        // left alone, and deliberately: `makeDivisionTotal` guards it and the fold behind this pass
        // collapses the guard to the constant, which is a shorter route than a rule here would be.
        case LowerInst::IDiv: {
            if(sd == 0 || sd == 1) return nullptr;
            if(sd == -1) return r.neg(x, name);

            auto magnitude = sd < 0 ? U64(0) - U64(sd) : U64(sd);
            if(!powerOfTwo(magnitude) && !hasWideningProduct(type)) return nullptr;

            return reduceSignedDivide(r, x, sd, bits, name);
        }

        case LowerInst::IRem: {
            if(sd == 0 || sd == 1) return nullptr;
            if(sd == -1) return r.imm(x->type, 0, name);

            auto magnitude = sd < 0 ? U64(0) - U64(sd) : U64(sd);
            if(auto power = powerOfTwo(magnitude)) {
                auto shift = power.unwrap();
                auto ones = r.opImm(LowerInst::Sar, x, bits - 1);
                auto bias = r.opImm(LowerInst::Shr, ones, bits - shift);
                auto biased = r.op(LowerInst::Add, x, bias);

                // The quotient scaled back up, which is the biased value with its low `shift` bits
                // cleared - the sign of the divisor cannot reach this, since a remainder takes its
                // sign from the dividend.
                auto truncated = r.opImm(LowerInst::And, biased, ~(magnitude - 1) & maskOf(bits));
                return r.op(LowerInst::Sub, x, truncated, name);
            }

            if(!hasWideningProduct(type)) return nullptr;

            auto quotient = reduceSignedDivide(r, x, sd, bits, StringId {});
            return r.op(LowerInst::Sub, x, r.opImm(LowerInst::IMul, quotient, d), name);
        }

        default:
            return nullptr;
    }
}

} // namespace

void strengthReduceFunction(LowerBase base, LowerModule& module, LowerFunction& fun) {
    for(auto blockPtr: fun.blocks.contents(base)) {
        auto block = base[blockPtr];

        // Inline: one of these per block, holding the instructions of that block while the list it
        // came from is rewritten - the same shape as foldFunctionConstants, and for the same reason.
        SmallArray<LowerPtr<LowerInst>, 32> kept;
        auto rewrote = false;

        for(auto instPtr: block->instructions.contents(base)) {
            auto inst = base[instPtr];

            if(!isBinary(inst) || inst->kind == LowerInst::Cmp) {
                kept.push(instPtr);
                continue;
            }

            Reducer reducer { base, module, blockPtr, inst->source, kept };
            auto replacement = reduce(reducer, (LowerInstBinary*)inst);

            if(!replacement) {
                kept.push(instPtr);
                continue;
            }

            detach(base, inst);
            replaceUses(base, module.arena, ((LowerInstSingle*)inst)->created().ptr - base, replacement - base);
            rewrote = true;
        }

        if(!rewrote) continue;

        block->instructions.clear();
        for(auto instPtr: kept) block->instructions.push(module.arena, instPtr);
    }
}
