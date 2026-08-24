#include "transform_internal.h"

/*
 * Operations the machine has no instruction for, written out as IR.
 *
 * Expanding into ordinary lower IR rather than into a machine pseudo is the rule here, and the
 * unsigned bank conversion below argues for it at length: every instruction one of these produces
 * is then allocated, folded and costed by machinery that already exists, where a pseudo would need
 * scratch registers nothing reserves and an encoder of its own.
 *
 * What decides that an operation belongs here is `unsupportedVectorReason` in machine_vector.cpp refusing
 * it, or a lane width having no column in the form table - so the two files are read together.
 */

/*
 * Unsigned conversions.
 *
 * AMD64 converts between the two register banks in one instruction only where the integer side is
 * *signed*: `cvtsi2sd` reads an i64 and `cvttsd2si` writes one, and neither has an unsigned form
 * before AVX-512. So an unsigned conversion is a sequence rather than an instruction, which is
 * exactly why it is expanded here instead of being given a machine form - a form describes one
 * encoding with its operands, and there is no encoding to describe.
 *
 * Expanding into ordinary IR rather than into a pseudo is what keeps it cheap. Every instruction
 * below is one the backend already allocates, folds and encodes: each comparison is folded into the
 * select that reads it, each constant is embedded or materialized by the same peephole as any other,
 * and the register pressure is priced by the same costing. A pseudo would have needed scratch
 * registers nothing reserves, clobbers of its own, and an encoder that reproduced half of this file.
 *
 * The two 32-bit cases are exact rather than approximate, and for the same reason in both
 * directions: every u32 fits in an i64, so widening the *other* side and converting signed is
 * correct with nothing to correct afterwards.
 */

// An unsigned integer widened into a float.
static LowerValue* expandUnsignedToFloat(Expansion& e, LowerValue* value, LowerType to, StringId name) {
    if(value->type == LowerType::Int32) {
        auto wide = e.convert(LowerType::Int64, value, false, false);
        return e.convert(to, wide, true, false, name);
    }

    // Only the top bit makes the signed conversion wrong, so a value that has it set is halved until
    // it does not and the result doubled back. Halving with `(x >> 1) | (x & 1)` rather than with a
    // plain shift keeps the bit that would have been shifted out where the rounding can still see
    // it: dropping it would round a value that should have gone up to even instead.
    //
    // Both halves are computed and one is selected rather than branched over. Neither can trap - a
    // conversion of a negative i64 is simply a negative float, which this path then discards - so
    // the arm that is wrong on a given input costs an instruction rather than correctness.
    auto one = e.integer(LowerType::Int64, 1);
    auto half = e.binary(LowerInst::Shr, LowerType::Int64, value, one);
    auto odd = e.binary(LowerInst::And, LowerType::Int64, value, one);
    auto rounded = e.binary(LowerInst::Or, LowerType::Int64, half, odd);
    auto halved = e.convert(to, rounded, true, false);
    auto doubled = e.binary(LowerInst::Add, to, halved, halved);
    auto direct = e.convert(to, value, true, false);

    // Signed-less-than-zero is exactly "the top bit is set", which is the one case the direct
    // conversion gets wrong. The comparison is emitted immediately in front of the select so that
    // the folding leaves it in the flags rather than materializing it.
    auto zero = e.integer(LowerType::Int64, 0);
    auto negative = e.compare(LowerCmp::ilt, value, zero);
    return e.select(to, negative, doubled, direct, name);
}

/*
 * A float truncated into a *signed* integer, saturating - see `saturationRange` for the ruling.
 *
 * Two comparisons and two selects, and the reason it is only two of each is that `cvttsd2si` has
 * already answered one of the three cases. Its result for a NaN, for +infinity and for anything
 * outside the range is the integer indefinite value - which *is* the type's minimum, and therefore
 * *is* the saturated answer for everything that overflows downwards. So what is left to fix is the
 * top end and NaN, and each is one comparison the hardware reads directly:
 *
 *  - `x >= 2^(n-1)` is an ordered comparison, so a NaN answers false and cannot be caught here. The
 *    bound is the power of two rather than the type's maximum because the maximum is not a double at
 *    sixty-four bits, and a comparison against something near it is a comparison against the wrong
 *    number.
 *  - `cmp_uno` is the NaN test, and it exists because no pair of ordered comparisons can be one: a
 *    NaN and a value below the range both answer false to `x >= lo`, and the two want different
 *    results. On this machine it is the parity flag alone, which is why it is a comparison of its
 *    own rather than the `x != x` it replaced - that needed ZF as well and the two `setcc`s and a
 *    combine `genFloatFlagsToReg` emits for a float equality.
 */
static LowerValue* expandFloatToSigned(Expansion& e, LowerValue* value, LowerType to, StringId name) {
    auto bits = to == LowerType::Int32 ? 32 : 64;
    auto limit = bits == 32 ? 2147483648.0 : 9223372036854775808.0;
    auto highest = bits == 32 ? U64(0x7FFFFFFF) : U64(0x7FFFFFFFFFFFFFFF);

    auto direct = e.convert(to, value, false, true);

    auto zero = e.integer(to, 0);
    auto isNaN = e.compare(LowerCmp::uno, value, value);
    auto ordered = e.select(to, isNaN, zero, direct);

    auto bound = e.floating(value->type, limit);
    auto maximum = e.integer(to, highest);
    auto isBig = e.compare(LowerCmp::ge, value, bound);

    return e.select(to, isBig, maximum, ordered, name);
}

/*
 * And into an unsigned integer, which saturates on the same terms and gets no help from the
 * hardware at either end: `cvttsd2si` is a signed conversion, so its answer for a negative input is
 * a negative number rather than the zero this has to produce.
 *
 * Both ends are therefore explicit. `x >= 0` is ordered, so it is false for a NaN as well as for a
 * negative - and both of those want zero, which is why one comparison covers the two cases that
 * needed two for the signed form.
 */
static LowerValue* expandFloatToUnsigned(Expansion& e, LowerValue* value, LowerType to, StringId name) {
    auto zeroFloat = e.floating(value->type, 0.0);
    auto atLeastZero = e.compare(LowerCmp::ge, value, zeroFloat);

    if(to == LowerType::Int32) {
        // Every value of a `U32` converts through a signed 64-bit conversion exactly, so the
        // in-range arm is what it always was and only the two ends are new.
        auto wide = e.convert(LowerType::Int64, value, false, true);
        auto narrowed = e.convert(to, wide, false, false);

        auto zero = e.integer(to, 0);
        auto low = e.select(to, atLeastZero, narrowed, zero);

        auto bound = e.floating(value->type, 4294967296.0);
        auto maximum = e.integer(to, 0xFFFFFFFF);
        auto isBig = e.compare(LowerCmp::ge, value, bound);

        return e.select(to, isBig, maximum, low, name);
    }

    // Everything below 2^63 converts signed exactly. Everything above it is brought into range by
    // subtracting 2^63 - which is exact, both operands being of the same magnitude - and the bit
    // that removes is put back into the integer afterwards.
    //
    // As above, both arms are computed and one selected. The comparison has the select as its only
    // reader and sits directly in front of it, so it stays in the flags.
    auto limit = e.floating(value->type, 9223372036854775808.0);
    auto reduced = e.binary(LowerInst::Sub, value->type, value, limit);
    auto big = e.convert(LowerType::Int64, reduced, false, true);
    auto sign = e.integer(LowerType::Int64, 0x8000000000000000);
    auto flipped = e.binary(LowerInst::Xor, LowerType::Int64, big, sign);
    auto small = e.convert(LowerType::Int64, value, false, true);
    auto isBig = e.compare(LowerCmp::ge, value, limit);
    auto inRange = e.select(LowerType::Int64, isBig, flipped, small);

    auto zero = e.integer(LowerType::Int64, 0);
    auto low = e.select(LowerType::Int64, atLeastZero, inRange, zero);

    auto ceiling = e.floating(value->type, 18446744073709551616.0);
    auto maximum = e.integer(LowerType::Int64, 0xFFFFFFFFFFFFFFFF);
    auto isHuge = e.compare(LowerCmp::ge, value, ceiling);

    return e.select(LowerType::Int64, isHuge, maximum, low, name);
}

void expandBankConversions(Context&, LowerBase base, LowerFunction& fun) {
    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        // Indexed rather than buffered, and advanced by hand rather than by the loop, because both
        // things this does to an instruction move the ones after it: an expansion inserts in front
        // of the conversion and every removal closes the gap it leaves.
        for(Size i = 0; i < block->instructions.size(); ) {
            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::Cast) { i++; continue; }

            auto cast = (LowerInstCast*)inst;
            auto value = base[cast->from];
            auto to = cast->result.type;

            // A conversion is unsigned when the *integer* side is, which is whichever of the two
            // flags the cast carries depending on the direction it goes in. A cast that crosses no
            // bank boundary is not a conversion at all.
            auto toFloat = isFloat(to) && !isFloat(value->type);
            auto fromFloat = isFloat(value->type) && !isFloat(to);

            // **Every** float-to-integer conversion is expanded, not only the unsigned ones, because
            // saturating is what one means and `cvttsd2si` alone does not saturate - see
            // `saturationRange`. The other direction still only needs the unsigned case, which is
            // the one the machine has no instruction for.
            auto expandable = fromFloat || (toFloat && !cast->isSignedSource());

            if(!expandable) { i++; continue; }

            // A conversion nothing reads is removed rather than expanded. It is dead either way -
            // every instruction the expansion produces is side-effect free - but expanding it first
            // would make ten dead instructions out of one, and no later pass would take them out.
            // Whatever followed has moved into this position, so the walk stays where it is.
            if(cast->result.uses.isEmpty()) {
                removeInst(base, cast);
                continue;
            }

            Expansion e { base, fun, block, i };
            auto replacement = toFloat
                ? expandUnsignedToFloat(e, value, to, cast->result.name)
                : cast->isSignedResult() ? expandFloatToSigned(e, value, to, cast->result.name)
                                         : expandFloatToUnsigned(e, value, to, cast->result.name);

            replaceAllUses(base, &cast->result, replacement);
            removeInst(base, cast);

            // Past the whole expansion. The insertions left it occupying the positions the
            // conversion's own used to begin at, and removing the conversion from the end of it
            // closed the gap - so `at` is where the walk carries on. Nothing in what was produced is
            // an unsigned conversion, so there is nothing there to come back for.
            i = e.at;
        }
    }
}

/*
 * A packed shift whose count is a splat, written as the scalar count the machine's form takes.
 *
 * `class (Num(a)) Integral(a)` declares `shl(lhs: a, rhs: a)`, so over a vector *both* operands are
 * vectors and `v `shr` 7` reaches this backend as a shift by `vsplat(7)`. The form table has only
 * the immediate rows - `pslld xmm, imm8` and its siblings - and the selection asks whether the right
 * operand is a scalar `Imm`, so every shift a program could actually write was refused for want of
 * an instruction that was standing right there.
 *
 * **The splat is the whole of the difference, and unwrapping it is the whole of the fix.** Every
 * lane of the count holds the same scalar by construction, which is exactly what one shared count
 * means, so the rewrite is exact rather than a narrowing of what was asked for.
 *
 * **Every splat is unwrapped, not only a constant one**, and the argument is the same for both: a
 * splat's lanes all hold the scalar, and both machine forms want that scalar. ~~A splat of a runtime
 * value would want the machine's other form, which this backend does not have~~ - it has it now
 * (`FormVShlReg` and its two siblings), and what that form takes is a scalar in a general register,
 * so this pass is what puts it in reach.
 *
 * Handing either form the splat *unchanged* would be wrong rather than slow, which is worth keeping
 * written down: `pslld` reads the whole low **quadword** as one count, so a 32-bit lane splat of 7
 * arrives as 0x0000000700000007 and shifts every lane out. The unwrapping is what makes the count a
 * number rather than a bit pattern; the `movd` in the register form's expansion is what keeps it
 * one.
 *
 * **Above `poolVectorConstants`**, which is the whole of where this may sit: a constant splat is a
 * `.rodata` load by the time that pass has run, and a load is not a count this can read. It is the
 * same ordering constraint every pass reading a constant chain has, and for the same reason.
 */
void unwrapVectorShiftCounts(Context&, LowerBase base, LowerFunction& fun) {
    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];

            if(inst->kind != LowerInst::Shl && inst->kind != LowerInst::Shr && inst->kind != LowerInst::Sar) {
                continue;
            }

            auto shift = (LowerInstBinary*)inst;
            if(!isVectorLike(shift->result.type)) continue;

            // A count that is already a scalar is the spelling the machine wants and the one the
            // `.lower` fixtures are written in - there is nothing here for it.
            auto count = base[shift->rhs];
            if(count->inst()->kind != LowerInst::VecSplat) continue;

            auto source = base[((LowerInstVecSplat*)count->inst())->from];
            setOperand(base, fun.arena, inst, shift->rhs, source);

            /*
             * **The orphaned splat has to go, and not only because it is dead code.** While it
             * stands it is a second reader of the constant, and `canEmbedImm` will not embed a
             * constant something else needs in a register - so the count would be materialized with
             * a `mov` and the shift would encode a zero where its immediate should be. Measured:
             * `mov $0x7,%eax ; movd ; pshufd ; psrld $0x0`.
             *
             * The splat sits above the shift, so removing it moves the walk back one - and its own
             * source may be dead in turn, which is what the second removal is for. Nothing deeper
             * than that: a constant is the end of the chain, and a runtime count is a value some
             * other instruction computed and this pass has no business following.
             */
            // The cursor moves back only for a removal from *this* block: either of the two may have
            // been built in another one - a constant hoisted to the entry block is the ordinary case
            // - and there the walk is unaffected.
            auto removeAndTrack = [&](LowerInst* dead) {
                if(base[dead->block] == block) i--;
                removeInst(base, dead);
            };

            if(!count->uses.isEmpty()) continue;

            removeAndTrack(count->inst());

            // The scalar goes only if it is a constant with nothing left reading it. A runtime count
            // is somebody else's instruction and removing it here would be a dead-code pass, which
            // this is not - and it still has a reader anyway, the shift this just gave it to.
            if(source->uses.isEmpty() && source->inst()->kind == LowerInst::Imm) {
                removeAndTrack(source->inst());
            }
        }
    }
}

/*
 * The arithmetic shift of a quadword lane, which no x86 below AVX-512 has and every x86 can do.
 *
 * `psraw` and `psrad` exist and `psraq` does not, which is the machine's one asymmetry in the shift
 * family - the logical shift and the left shift are complete at all three widths. What fills it is
 * the standard bias identity: shifting logically brings in zeros where the arithmetic shift wanted
 * the sign bit, and the difference between the two is a constant that depends on the count alone.
 *
 *     x >>a n  ==  ((x >>l n) ^ K) - K        where K = 1 << (63 - n)
 *
 * Read it as a two's-complement offset rather than as a trick. `(t ^ K) - K` with `K` a single bit
 * is sign extension *from that bit*: it leaves `t` alone when the bit is clear and subtracts `2K`
 * when it is set, which is exactly what turning bit `63-n` into a sign bit means. And bit `63-n` is
 * where the operand's own sign bit landed after shifting right by `n`.
 *
 * Three instructions where the count is a constant, `K` being one pooled load hoisted to the top of
 * the function and shared by every shift by that count. That is against about eleven for a
 * scalarized pair of lanes - two `pextrq`, two `sar`, two `pinsrq` and the bank crossings - so it
 * clears Design-Vector property 5 at every lane count.
 *
 * ## A count that is not a constant
 *
 * The same identity, with `K` computed rather than pooled: `1 << (63 - n)` is the sign bit shifted
 * right by `n`, which is the machine's own `psrlq` over a splat of `0x8000...0`. So the runtime case
 * is the constant one with one more shift in it, and the splat it shifts is the hoistable part.
 *
 * ## The out-of-range count, which is unspecified and was already unspecified three ways
 *
 * A count of 64 or more has no answer in this language: `lower_fold.cpp` refuses to fold one rather
 * than deciding it, and the two machines already disagree about it. A *scalar* `sar` on x86 masks
 * the count to six bits, so `x >>a 64` is `x`; a packed `psrad` saturates, so `x >>a 32` at a 32-bit
 * lane is the sign bit in every position.
 *
 * A written-down count is clamped to 63 here, which makes it agree with the narrower packed widths -
 * it costs nothing, the clamp happening at compile time. A *runtime* count out of range comes out as
 * zero instead, `psrlq` answering zero for both the value and the bias, and buying the saturating
 * answer would mean a `min` on the count in the loop. It is not bought, and this paragraph is the
 * reason: there is no answer to be faithful to.
 */
void expandQuadwordSar(Context&, LowerBase base, LowerFunction& fun) {
    auto home = constantHome(base, fun);
    if(!home) return;

    // The splat of the sign bit, which the runtime count shifts into its bias, and one bias per
    // written-down count. Interned here rather than left to CSE, which does not run below this point;
    // the array is indexed by the count itself, which is why it is 64 wide and needs no search.
    LowerValue* signBits = nullptr;
    LowerValue* biases[64] = {};

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::Sar) continue;

            auto shift = (LowerInstBinary*)inst;
            auto type = shift->result.type;
            if(!isIntVector(type) || laneBytes(type.lane) != 8) continue;

            auto value = base[shift->lhs];
            auto count = base[shift->rhs];

            /*
             * A count that is one per lane, which reaches here only where AVX2 is present -
             * `scalarizeVectorLanes` above takes away every other one. The identity below is
             * lane-wise in the count as well as in the value, so it needs no change at all for it:
             * `K = 1 << (63 - n)` is the sign bit shifted right by `n`, and `vpsrlvq` shifts each
             * lane by its own.
             */
            auto perLane = isVectorLike(count->type);
            if(perLane && !(targetFeatures() & kFeatureAvx2)) continue;

            // The count is a scalar by now - `unwrapVectorShiftCounts` runs above this and is what
            // takes the splat the language's spelling wraps it in off. A vector one reaching here is
            // that pass not having run, and `packedShiftSharedCount` has already refused the shape
            // it could not unwrap.
            auto written = !perLane && count->inst()->kind == LowerInst::Imm
                         ? (LowerImm*)count->inst() : nullptr;

            /*
             * The two constants, and where each of them goes.
             *
             * A written-down count decides the bias outright, so both it and the shift's own count
             * are compile-time values and the bias is hoisted whole. A runtime one leaves the bias a
             * shift of the hoisted sign-bit splat, which has to stand beside its reader.
             */
            LowerValue* bias = nullptr;
            LowerValue* amount = count;

            if(written) {
                auto n = written->i > 63 ? U64(63) : written->i;

                if(!biases[n]) {
                    Expansion at { base, fun, home, 0 };
                    auto scalar = at.integer(scalarFormOf(type), U64(1) << (63 - n));
                    biases[n] = at.splat(type, scalar);

                    if(block == home) i += 2;
                }

                bias = biases[n];

                // A clamped count is a different number from the one that was written, so it needs
                // an immediate of its own; an in-range one reaches the machine as it stands.
                if(n != written->i) {
                    Expansion at { base, fun, block, i };
                    amount = at.integer(scalarFormOf(type), n);
                    i = at.at;
                }
            } else if(!signBits) {
                Expansion at { base, fun, home, 0 };
                auto scalar = at.integer(scalarFormOf(type), U64(1) << 63);
                signBits = at.splat(type, scalar);

                if(block == home) i += 2;
            }

            Expansion e { base, fun, block, i };
            if(!bias) bias = e.binary(LowerInst::Shr, type, signBits, amount);

            auto logical = e.binary(LowerInst::Shr, type, value, amount);
            auto flipped = e.binary(LowerInst::Xor, type, logical, bias);
            auto result = e.binary(LowerInst::Sub, type, flipped, bias, shift->result.name);

            replaceAllUses(base, &shift->result, result);
            removeInst(base, shift);

            // The subtraction stands where the shift did, so the walk carries on past it.
            i = e.at - 1;
        }
    }
}

/*
 * A shift at a byte lane, which the machine has none of in any direction.
 *
 * `psllw` is the narrowest packed shift there is, so a byte lane has to borrow the word one - and
 * what it borrows is *almost* right. Shifting sixteen bits at a time gets every byte's own bits into
 * the right places; what it also does is carry bits across the boundary between the two bytes of
 * each word, and the whole of the correction is masking those away. The mask is one byte repeated
 * and depends on the count alone:
 *
 *     x <<b n  ==  (x <<w n) & (0xff << n)        per byte
 *     x >>b n  ==  (x >>w n) & (0xff >> n)        per byte
 *
 * Two instructions and a constant, for sixteen lanes at once. The scalarized alternative is sixteen
 * `pextrb`, sixteen shifts and sixteen `pinsrb`, so this is not a close comparison at any lane count.
 *
 * ## The arithmetic one, on top of the logical one
 *
 * There is no byte arithmetic shift to borrow either - `psraw` and `psrad` are the family's whole
 * extent - so it is the logical shift above plus the same bias `expandQuadwordSar` uses one lane
 * width up: `(t ^ K) - K` with `K = 1 << (7 - n)` is sign extension from bit `7-n`, which is where
 * the byte's own sign bit landed. Four instructions and two constants.
 *
 * ## Where the mask comes from when the count does not
 *
 * `0xff << n` is a *scalar* expression, and the count is already a scalar in a general register by
 * the time this runs - so a count that is not written down costs two general-register instructions
 * and a broadcast, once, rather than anything per lane. A written-down count folds all of that at
 * compile time and leaves one pooled splat hoisted to the top of the function.
 *
 * The runtime case builds its mask beside the shift rather than in the entry block, which is the one
 * thing here that is left on the table: a loop-invariant count recomputes its mask every iteration,
 * there being no code motion below this point. It is three instructions and a rare shape - the
 * language spells a byte shift `v `shl` n` with `n` a `Vec(I8)`, so a runtime one is a count that
 * varies - and hoisting it would mean asking where the count is defined, which is a dominance
 * question this pass has no answer to.
 *
 * ## The out-of-range count
 *
 * Unspecified, as it is at every other lane width - `lower_fold.cpp` refuses to fold one rather than
 * deciding it. A written-down count of 8 or more comes out as zero for the two logical directions,
 * the mask being zero, and is clamped to 7 for the arithmetic one, which is the saturating answer
 * `psrad` gives at its own width. A runtime one out of range is whatever the general-register shift
 * that built the mask did with it.
 */
void expandByteShifts(Context&, LowerBase base, LowerFunction& fun) {
    auto home = constantHome(base, fun);
    if(!home) return;

    // Interned per count: the byte mask for a left shift, the one for a right shift - which the
    // arithmetic shift borrows, its logical half being exactly that - and the arithmetic one's bias.
    LowerValue* leftMasks[8] = {};
    LowerValue* rightMasks[8] = {};
    LowerValue* biases[8] = {};

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];

            if(inst->kind != LowerInst::Shl && inst->kind != LowerInst::Shr
               && inst->kind != LowerInst::Sar) {
                continue;
            }

            auto shift = (LowerInstBinary*)inst;
            auto type = shift->result.type;
            if(!isIntVector(type) || laneBytes(type.lane) != 1) continue;

            auto value = base[shift->lhs];
            auto count = base[shift->rhs];
            if(isVectorLike(count->type)) continue; // one count per lane - see expandQuadwordSar

            auto written = count->inst()->kind == LowerInst::Imm ? (LowerImm*)count->inst() : nullptr;

            auto arithmetic = inst->kind == LowerInst::Sar;
            auto left = inst->kind == LowerInst::Shl;

            // The word view the borrowed shift works at, which is the same register read another way
            // and emits nothing wherever the allocator lands both ends in one place.
            auto words = vectorType(LowerLane::Int16, type.lanes() / 2);

            // A constant is built once at the top of the function and shared; a computed one stands
            // beside its reader, for the reason the note above gives.
            auto splatConstant = [&](LowerValue*& slot, U64 bits) {
                if(slot) return slot;

                Expansion at { base, fun, home, 0 };
                auto scalar = at.integer(scalarFormOf(type), bits);
                slot = at.splat(type, scalar);

                if(block == home) i += 2;
                return slot;
            };

            LowerValue* mask = nullptr;
            LowerValue* bias = nullptr;

            if(written) {
                auto n = written->i;

                // The logical directions answer zero past the lane's width, which is what a mask of
                // zero produces; the arithmetic one saturates, which is what clamping produces.
                auto clamped = arithmetic ? (n > 7 ? U64(7) : n) : n;
                auto byte = n > 7 ? U64(0) : (left ? (0xffu << n) & 0xffu : 0xffu >> n);

                mask = splatConstant(left ? leftMasks[clamped] : rightMasks[clamped], byte);
                if(arithmetic) bias = splatConstant(biases[clamped], U64(1) << (7 - clamped));
            }

            Expansion e { base, fun, block, i };

            if(!written) {
                /*
                 * The mask, computed rather than pooled. `0xff << n` needs the surviving byte taken
                 * back out of it because a general-register shift is 32 bits wide; `0xff >> n` does
                 * not, the value being eight bits to begin with.
                 */
                auto scalar = scalarFormOf(type);
                auto all = e.integer(scalar, 0xff);
                auto shifted = e.binary(left ? LowerInst::Shl : LowerInst::Shr, scalar, all, count);

                if(left) {
                    auto byte = e.integer(scalar, 0xff);
                    shifted = e.binary(LowerInst::And, scalar, shifted, byte);
                }

                mask = e.splat(type, shifted);

                if(arithmetic) {
                    auto top = e.integer(scalar, 7);
                    auto from = e.binary(LowerInst::Sub, scalar, top, count);
                    auto one = e.integer(scalar, 1);
                    bias = e.splat(type, e.binary(LowerInst::Shl, scalar, one, from));
                }
            }

            auto wide = e.reinterpret(words, value);
            auto borrowed = e.binary(left ? LowerInst::Shl : LowerInst::Shr, words, wide, count);
            auto lanes = e.reinterpret(type, borrowed);
            auto result = e.binary(LowerInst::And, type, lanes, mask,
                                   arithmetic ? StringId() : shift->result.name);

            if(arithmetic) {
                auto flipped = e.binary(LowerInst::Xor, type, result, bias);
                result = e.binary(LowerInst::Sub, type, flipped, bias, shift->result.name);
            }

            replaceAllUses(base, &shift->result, result);
            removeInst(base, shift);

            // What came out last stands where the shift did, so the walk carries on past it.
            i = e.at - 1;
        }
    }
}

/*
 * The two integer multiplies the machine does not have: a byte lane and a quadword one.
 *
 * `pmullw` is SSE2, `pmulld` is SSE4.1 and therefore the floor, and that is the whole list - there
 * is no packed multiply of a byte or a quadword at any feature level, `vpmullq` being AVX-512DQ.
 * Both are built here out of the products that do exist, and both are the sequences a compiler is
 * expected to produce for them; the reason they are worth building rather than refusing is
 * Design-Vector property 5, which compares whole loops. A multiply inside vector code has vector
 * operands already, so its alternative is `pextr`, the scalar multiply and `pinsr` **per lane** plus
 * the bank crossing each way - sixteen of those at a byte lane, against the six instructions below.
 *
 * ## The byte lane, which is the word product masked
 *
 * The low eight bits of a product depend on the low eight bits of its operands and nothing else, so
 * a 16-bit multiply computes the byte product of the *even* bytes correctly in its low half. The odd
 * bytes are the same multiply with the operands lined up differently:
 *
 *     even = (a * b) & 0x00ff                    -- `a * b` mod 256 is `alo * blo` mod 256
 *     odd  = (a & 0xff00) * (b >> 8)             -- `(ahi << 8) * bhi`, whose low byte is zero
 *     r    = even | odd
 *
 * Six instructions and two pooled masks, and no shift to put the odd half back where it belongs -
 * shifting one operand up instead of the product is what saves that.
 */
void expandByteMul(Context&, LowerBase base, LowerFunction& fun) {
    auto home = constantHome(base, fun);
    if(!home) return;

    LowerValue* lowBytes = nullptr;
    LowerValue* highBytes = nullptr;

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::Mul && inst->kind != LowerInst::IMul) continue;

            auto product = (LowerInstBinary*)inst;
            auto type = product->result.type;
            if(!isIntVector(type) || laneBytes(type.lane) != 1) continue;

            // The word view both multiplies work at, which is the same register read another way.
            auto words = vectorType(LowerLane::Int16, type.lanes() / 2);

            // One mask each per function, at the *word* type: neither is a byte pattern repeated, so
            // neither is a splat of the lane this instruction is written at.
            auto splatConstant = [&](LowerValue*& slot, U64 bits) {
                if(slot) return slot;

                Expansion at { base, fun, home, 0 };
                auto scalar = at.integer(scalarFormOf(words), bits);
                slot = at.splat(words, scalar);

                if(block == home) i += 2;
                return slot;
            };

            auto low = splatConstant(lowBytes, 0x00ff);
            auto high = splatConstant(highBytes, 0xff00);

            Expansion e { base, fun, block, i };

            auto a = e.reinterpret(words, base[product->lhs]);
            auto b = e.reinterpret(words, base[product->rhs]);

            auto even = e.binary(LowerInst::And, words, e.binary(LowerInst::Mul, words, a, b), low);

            // The odd bytes with `a`'s shifted up rather than the product shifted back: the low byte
            // of `(ahi << 8) * bhi` is zero by construction, so the two halves combine with an `or`
            // and no third instruction between them.
            auto eight = e.integer(scalarFormOf(words), 8);
            auto odd = e.binary(LowerInst::Mul, words, e.binary(LowerInst::And, words, a, high),
                                e.binary(LowerInst::Shr, words, b, eight));

            auto joined = e.binary(LowerInst::Or, words, even, odd);
            auto result = e.reinterpret(type, joined, product->result.name);

            replaceAllUses(base, &product->result, result);
            removeInst(base, product);

            i = e.at - 1;
        }
    }
}

/*
 * The quadword lane, which is long multiplication over two 32-bit limbs.
 *
 * `pmuludq` is the machine's only 32x32 -> 64 packed product - see LowerInst::X86MulWide - and three
 * of them are what a 64-bit product costs, the fourth partial product being entirely above the bits
 * that survive:
 *
 *     a * b  ==  alo*blo + (ahi*blo + alo*bhi) << 32          (mod 2^64)
 *
 * Eight instructions: two shifts to bring each operand's high limb down, three widening products,
 * the cross sum, its shift and the final add. **Every partial product is unsigned** whichever way
 * the multiplication was written - the low 64 bits of a product are the same bits in two's
 * complement either way, which is the same reason `Mul` and `IMul` share one row at every lane
 * width the machine does have.
 *
 * A slight win against the scalarized form at two lanes and a clear one at four, which is where the
 * cost of the transfers stops being amortized against the same eight instructions.
 */
void expandQuadwordMul(Context&, LowerBase base, LowerFunction& fun) {
    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::Mul && inst->kind != LowerInst::IMul) continue;

            auto product = (LowerInstBinary*)inst;
            auto type = product->result.type;
            if(!isIntVector(type) || laneBytes(type.lane) != 8) continue;

            auto a = base[product->lhs];
            auto b = base[product->rhs];

            Expansion e { base, fun, block, i };

            auto widen = [&](LowerValue* lhs, LowerValue* rhs) {
                return e.emit(new (fun.arena) LowerInstX86MulWide(StringId(), type, lhs - base,
                                                                  rhs - base, false));
            };

            // `pmuludq` reads the low 32 bits of every quadword, so bringing a high limb into
            // reach is an ordinary logical shift and needs no repacking.
            auto thirtyTwo = e.integer(scalarFormOf(type), 32);
            auto aHigh = e.binary(LowerInst::Shr, type, a, thirtyTwo);
            auto bHigh = e.binary(LowerInst::Shr, type, b, thirtyTwo);

            auto cross = e.binary(LowerInst::Add, type, widen(aHigh, b), widen(a, bHigh));
            auto shifted = e.binary(LowerInst::Shl, type, cross, thirtyTwo);

            // The shift is what makes the cross sum's own overflow harmless: only its low 32 bits
            // reach the answer, and every bit the addition could have lost is above them.
            auto result = e.binary(LowerInst::Add, type, widen(a, b), shifted, product->result.name);

            replaceAllUses(base, &product->result, result);
            removeInst(base, product);

            i = e.at - 1;
        }
    }
}

/*
 * The *high* half of a product at a 32- and a 64-bit lane, which is what a division by a constant
 * needs. `strengthReduceFunction` is where the division becomes one, a whole tier above this.
 *
 * `pmulld` keeps the low half and `pmullq` is AVX-512DQ, so at neither width does the machine hand
 * over the top of a product. What it has instead is one widening even-lane multiply - `pmuludq` and
 * `pmuldq`, see LowerInst::X86MulWide - and both sequences below are that instruction run more than
 * once with shuffles around it.
 *
 * **Every shuffle here is in-lane**, which is what makes each 256-bit twin the same sequence with
 * `L` set: `vpmuludq` on a ymm reads dwords 0 and 2 of each 128-bit half and lands their products in
 * that half's quadwords, so nothing this builds crosses the middle.
 *
 * ## The 32-bit lane: two products, gathered
 *
 *     t0 = mulwide(a, b)                       -- 64-bit products of lanes 0 and 2
 *     t1 = mulwide(odd(a), odd(b))             -- and of lanes 1 and 3, brought down by a shuffle
 *     r  = the four high dwords, put back in lane order
 *
 * Six instructions for four lanes, against the four `pextrd`, four `idiv` and four `pinsrd` a
 * scalarized division costs.
 *
 * ## The 64-bit lane: four products, and long multiplication over them
 *
 * The high half of a 64x64 product needs every partial product, unlike the low half - which is why
 * `expandQuadwordMul` uses three of these and this uses four:
 *
 *     a * b  ==  alo*blo + (ahi*blo + alo*bhi) << 32 + ahi*bhi << 64
 *
 * and what survives above bit 63 is `ahi*bhi`, the top halves of the two cross terms, and the carry
 * out of their bottom halves. That last is the whole reason this is eight instructions of addition
 * rather than three: the carry has to be *computed*, since a packed add sets no flags and there is
 * nothing to read one out of.
 *
 * The **signed** form is the unsigned one with a correction rather than `pmuldq` throughout, and the
 * reason is the same one that makes `expandQuadwordMul` use the unsigned row for every partial
 * product: a widening multiply of the *limbs* is unsigned whatever the whole values are, the limbs
 * being 32 bits of a number rather than a number. What the signedness costs is at the end -
 * `x <s 0 ? y : 0` subtracted for each operand, which is the standard correction from an unsigned
 * high half to a signed one, and is where the two `psrad` come from.
 *
 * Measured against the `idiv` pair it replaces: **2.5x signed and 4.0x unsigned** at 128 bits on a
 * Raptor Cove P-core, 5.0x signed at 256, and more on an E-core, whose 64-bit divider is slower
 * while this sequence is not. Those are the numbers with *nothing* hoisted, which is what this
 * emits - the shuffles of the multiplier are loop-invariant and there is no code motion below this
 * pass to notice. Hoisting them would be worth a further 13% signed and 3% unsigned.
 */
void expandVectorMulHi(Context&, LowerBase base, LowerFunction& fun) {
    // The low half of each quadword, for the 64-bit sequence's carry - one per function, and pooled
    // by the pass below this one.
    auto home = constantHome(base, fun);
    LowerValue* lowHalves = nullptr;

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::MulHi && inst->kind != LowerInst::IMulHi) continue;

            auto product = (LowerInstBinary*)inst;
            auto type = product->result.type;

            // A 16-bit lane is `pmulhw`/`pmulhuw` and never comes here; a byte has no widening
            // product to build one out of and `unsupportedVectorReason` has refused it before this
            // runs.
            if(!isIntVector(type)) continue;
            if(laneBytes(type.lane) != 4 && laneBytes(type.lane) != 8) continue;

            auto isSigned = inst->kind == LowerInst::IMulHi;

            /*
             * Work derived from a *constant* operand, built beside that constant instead of here -
             * and null where the operand is not one, which leaves the caller to build it inline.
             *
             * **The placement is the whole point.** A constant chain is where the last code motion
             * there was decided it belongs, which for a loop-invariant one is outside the loop; this
             * pass runs below every such pass, so anything it builds beside the *reader* stays in
             * the loop for ever. A strength-reduced division's multiplier is exactly that case - its
             * high limb and its sign are as invariant as it is, and `poolVectorConstants` then reads
             * through the whole derivation and puts one `.rodata` entry where the chain stood.
             *
             * Beside the constant rather than in the entry block, which would be the other obvious
             * answer and is wrong: the constant may be in a preheader that the entry block does not
             * reach a copy of, and a value cannot be built above the thing it reads.
             */
            auto deriveBeside = [&](LowerValue* value, auto&& make) -> LowerValue* {
                if(!isPooledVectorConstant(base, value)) return nullptr;

                auto definition = value->inst();
                auto where = definition ? base[definition->block] : nullptr;
                auto at = positionOf(base, where, definition);
                if(!at) return nullptr;

                Expansion beside { base, fun, where, at.unwrap() + 1 };
                return make(beside, value);
            };

            /*
             * The 64-bit lane, which is long multiplication kept at its top rather than its bottom.
             *
             * The operands are already quadword-typed and the widening multiply reads the low dword
             * of each quadword whatever the type says, so bringing a high limb into reach is a
             * `pshufd` that swaps the two dwords of each quadword and nothing else - no masking, and
             * no shift.
             */
            if(laneBytes(type.lane) == 8) {
                if(!home) continue;

                auto dwords = LowerType(LowerLane::Int32, U8(type.laneShift + 1), false);
                auto x = base[product->lhs];
                auto y = base[product->rhs];

                // Built before the expansion below is positioned, since placing it in this block
                // moves everything in it - including the instruction being rewritten - down by two.
                if(!lowHalves) {
                    Expansion at { base, fun, home, 0 };
                    auto scalar = at.integer(scalarFormOf(type), 0xffffffff);
                    lowHalves = at.splat(type, scalar);

                    if(block == home) i += 2;
                }

                // The two dwords of each quadword exchanged, which puts the high limb where the
                // widening multiply looks. `pshufd` with the pattern `1, 0, 3, 2`.
                auto swapped = [&](Expansion& at, LowerValue* v) {
                    auto lanes = at.reinterpret(dwords, v);
                    auto shuffled = at.shuffle(dwords, lanes, lanes, [](Size j) { return U8(j ^ 1); });
                    return at.reinterpret(type, shuffled);
                };

                // The sign of each quadword repeated across both of its dwords, which is a *32-bit*
                // arithmetic shift of the high dword duplicated - `psraq` not existing is no
                // obstacle, since what is wanted is the sign repeated rather than the value shifted.
                auto signMask = [&](Expansion& at, LowerValue* v) {
                    auto lanes = at.reinterpret(dwords, v);
                    auto top = at.shuffle(dwords, lanes, lanes, [](Size j) { return U8(j | 1); });
                    auto sign = at.binary(LowerInst::Sar, dwords, top,
                                          at.integer(scalarFormOf(dwords), 31));

                    return at.reinterpret(type, sign);
                };

                /*
                 * Everything derived from a *constant* operand, built beside that constant rather
                 * than here - see `deriveBeside`. A strength-reduced division's multiplier is one,
                 * and its high limb and its sign are as loop-invariant as it is.
                 */
                /*
                 * And the correction an operand does not need. `x <s 0 ? y : 0` is zero in every
                 * lane when `x` cannot be negative, and a strength-reduced division by a *positive*
                 * divisor has exactly that multiplier - so the common case loses four instructions
                 * rather than folding them to a mask of zeros nothing below here would remove.
                 */
                auto knownNonNegative = [&](LowerValue* v) {
                    auto width = Size(type.byteWidth());
                    if(width > kMaxVectorBytes) return false;

                    U8 constant[kMaxVectorBytes] = {};
                    InstChain read;
                    if(!constantVectorBytes(base, v, constant, width, read)) return false;

                    // The sign bit of a lane is the top bit of its last byte, these being written
                    // little-endian per lane.
                    for(Size at = laneBytes(type.lane); at <= width; at += laneBytes(type.lane)) {
                        if(constant[at - 1] & 0x80) return false;
                    }

                    return true;
                };

                auto correctX = isSigned && !knownNonNegative(x);
                auto correctY = isSigned && !knownNonNegative(y);

                auto xBeside = deriveBeside(x, swapped);
                auto yBeside = deriveBeside(y, swapped);
                auto xSign = correctX ? deriveBeside(x, signMask) : nullptr;
                auto ySign = correctY ? deriveBeside(y, signMask) : nullptr;

                // Whatever went into another block may have moved this one's contents; whatever went
                // into this one certainly did.
                auto here = positionOf(base, block, inst);
                if(!here) continue;

                i = here.unwrap();
                Expansion e { base, fun, block, i };

                // Unsigned for every partial product, whichever way the whole values are read: a
                // limb is 32 bits of a number rather than a number, and its widening product has no
                // sign to carry. The signed reading is corrected at the end instead.
                auto widen = [&](LowerValue* lhs, LowerValue* rhs) {
                    return e.emit(new (fun.arena) LowerInstX86MulWide(StringId(), type, lhs - base,
                                                                      rhs - base, false));
                };

                auto shift = e.integer(scalarFormOf(type), 32);
                auto xHigh = xBeside ? xBeside : swapped(e, x);
                auto yHigh = yBeside ? yBeside : swapped(e, y);

                auto lowLow = widen(x, y);
                auto lowHigh = widen(x, yHigh);
                auto highLow = widen(xHigh, y);
                auto highHigh = widen(xHigh, yHigh);

                /*
                 * The carry out of bit 63, computed rather than read: a packed add sets no flags, so
                 * the two cross terms are summed with the top of `alo*blo` and what crosses into the
                 * next limb is taken by shifting rather than by asking.
                 */
                auto first = e.binary(LowerInst::Add, type, lowHigh,
                                      e.binary(LowerInst::Shr, type, lowLow, shift));

                auto second = e.binary(LowerInst::Add, type, highLow,
                                       e.binary(LowerInst::And, type, first, lowHalves));

                auto high = e.binary(LowerInst::Add, type,
                                     e.binary(LowerInst::Add, type, highHigh,
                                              e.binary(LowerInst::Shr, type, first, shift)),
                                     e.binary(LowerInst::Shr, type, second, shift));

                /*
                 * And the two corrections a signed reading needs, which is what turns an unsigned
                 * high half into a signed one: `x <s 0 ? y : 0` subtracted for each operand. The
                 * test is a *32-bit* arithmetic shift of the duplicated high dword, which fills both
                 * dwords of the quadword with its sign - `psraq` not existing is no obstacle here,
                 * since what is wanted is the sign repeated rather than the value shifted.
                 */
                if(correctX) {
                    high = e.binary(LowerInst::Sub, type, high,
                                    e.binary(LowerInst::And, type,
                                             xSign ? xSign : signMask(e, x), y));
                }

                if(correctY) {
                    high = e.binary(LowerInst::Sub, type, high,
                                    e.binary(LowerInst::And, type,
                                             ySign ? ySign : signMask(e, y), x));
                }

                high->name = product->result.name;

                replaceAllUses(base, &product->result, high);
                removeInst(base, product);

                i = e.at - 1;
                continue;
            }

            auto quads = LowerType(LowerLane::Int64, U8(type.laneShift - 1), false);

            // Each 128-bit group's odd dwords brought down onto the even positions the widening
            // multiply reads: `pshufd` with the pattern `1,1,3,3`, which is in-lane at both tiers.
            auto odd = [&](Expansion& at, LowerValue* v) {
                // `1, 1, 3, 3` per group and not `1, 3, 1, 3`: the widening multiply reads dwords 0
                // and 2, so what has to land there is lane 1 and lane 3, and each may be written
                // into either of the two positions beside it.
                return at.shuffle(type, v, v, [&](Size j) { return U8((j & ~Size(3)) + (j & 2 ? 3 : 1)); });
            };

            auto lhsValue = base[product->lhs];
            auto rhsValue = base[product->rhs];

            auto lhsOdd = deriveBeside(lhsValue, odd);
            auto rhsOdd = deriveBeside(rhsValue, odd);

            auto here = positionOf(base, block, inst);
            if(!here) continue;

            i = here.unwrap();
            Expansion e { base, fun, block, i };

            auto widen = [&](LowerValue* lhs, LowerValue* rhs) {
                return e.emit(new (fun.arena) LowerInstX86MulWide(StringId(), quads, lhs - base,
                                                                  rhs - base, isSigned));
            };

            if(!lhsOdd) lhsOdd = odd(e, lhsValue);
            if(!rhsOdd) rhsOdd = odd(e, rhsValue);

            auto low = e.reinterpret(type, widen(lhsValue, rhsValue));
            auto high = e.reinterpret(type, widen(lhsOdd, rhsOdd));

            // The four high dwords of each group, two from each source - which is exactly the pair
            // of runs `shufps` takes.
            auto lanes = Size(type.lanes());
            auto gathered = e.shuffle(type, low, high, [&](Size j) {
                auto group = j & ~Size(3);
                auto within = j & 3;
                auto lane = group + 2 * (within & 1) + 1;

                return U8(within < 2 ? lane : lanes + lane);
            });

            // And back into lane order, which the interleaving above left as `0, 2, 1, 3` per group.
            auto result = e.shuffle(type, gathered, gathered, [&](Size j) {
                static const U8 kOrder[4] = { 0, 2, 1, 3 };
                return U8((j & ~Size(3)) + kOrder[j & 3]);
            }, product->result.name);

            replaceAllUses(base, &product->result, result);
            removeInst(base, product);

            i = e.at - 1;
        }
    }
}

/*
 * The two packed operations that have no instruction on any machine, done one lane at a time: an
 * integer division, and a shift whose count is *per lane* rather than shared.
 *
 * **These are the operations that are not a win on their own, and they are built anyway.** No x86
 * has a packed integer divide at any width or feature level, and neither has AArch64's ASIMD; the
 * only general lowering is the one below, and its transfers cost 10-20% against the same loop
 * written over scalars. Vectorizing a division buys no parallelism either - out-of-order execution
 * already overlaps a scalar loop's independent divisions, and both forms queue for the one
 * non-pipelined divider.
 *
 * What it buys is that `Vec(a)` supports every operation `a` does. A division inside vector code is
 * almost never alone: it sits among adds, multiplies and compares that are all sixteen-lanes-at-once
 * here, and refusing it makes the whole loop scalar rather than making one instruction faster. Only
 * a loop that is *nothing but* division comes out behind, and that is a trade worth stating rather
 * than a gap worth keeping.
 *
 * ## The per-lane shift, which was refused on a claim that was not true
 *
 * `unsupportedVectorReason` refused a shift whose count is not shared, on the grounds that the
 * language cannot write one - `Integral(a)` types both operands as the same `a`, so a count arrives
 * wrapped in a splat. It types them as the same `a`, which is exactly why `v `shl` w` over two
 * vectors typechecks: the count is a whole vector, one number per lane, and nothing wraps it.
 *
 * AVX2 has the family (`vpsllvd` and siblings) and v2 has nothing at all, so the general answer is
 * this one. A count that *is* shared never arrives here: `unwrapVectorShiftCounts` runs above and
 * leaves it a scalar, which is the machine's own register form.
 *
 * ## What it may assume about its operands
 *
 * That neither refused divisor reaches a division: `makeDivisionTotal` in lower/lower_divide.cpp
 * guards every integer one, packed ones included, so a zero and the overflowing signed pair have
 * both been answered in the vector domain before this runs. That is what lets the lane divisions
 * below be bare `idiv` - the language's `x / 0 == 0` is a lane-wise rule, and the select that
 * implements it stays lane-wise.
 *
 * ## The narrow lanes
 *
 * A lane read arrives **zero-extended** (`FormVExtract8`, and `outOfLane` in the LLVM backend states
 * the same contract), so an operation that reads a byte or word lane as *signed* has to put the sign
 * back first. Which operand needs it is per operation and not per instruction: a signed division
 * needs both, an arithmetic shift needs the value and not the count, and everything else needs
 * neither - a logical shift and an unsigned division are correct on the zero-extended value exactly
 * as it stands.
 *
 * A division's quotient then fits the lane without further work, and not by luck: the guard above
 * has already removed the divisor of -1, so `|q| <= |x| / 2` and the one signed pair whose quotient
 * does not fit the lane cannot arrive here. A shift's answer fits because the insert takes the low
 * bits, which is what a shift at that width means.
 */
void scalarizeVectorLanes(Context&, LowerBase base, LowerFunction& fun) {
    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];

            auto divides = inst->kind == LowerInst::Div || inst->kind == LowerInst::IDiv
                        || inst->kind == LowerInst::Rem || inst->kind == LowerInst::IRem;

            auto shifts = inst->kind == LowerInst::Shl || inst->kind == LowerInst::Shr
                       || inst->kind == LowerInst::Sar;

            if(!divides && !shifts) continue;

            auto operation = (LowerInstBinary*)inst;
            auto type = operation->result.type;
            if(!isIntVector(type)) continue;

            auto x = base[operation->lhs];
            auto b = base[operation->rhs];

            /*
             * A shift is only this pass's business where its count is one per lane *and* the machine
             * has no row for it. The shared spelling is the machine's own register form and reaches
             * here as a scalar, which is what `unwrapVectorShiftCounts` above leaves; the per-lane
             * one is AVX2's `vpsllv` family at a 32- or 64-bit lane, and nothing at all below AVX2
             * or at a narrower lane.
             *
             * The quadword `sar` is in the covered set even though `vpsravq` is AVX-512: it is what
             * `expandQuadwordSar` builds out of `vpsrlvq` and a bias, which is four instructions
             * against two lanes' worth of `pextrq`, `sar` and `pinsrq`.
             */
            if(shifts) {
                if(!isVectorLike(b->type)) continue;
                if((targetFeatures() & kFeatureAvx2) && laneBytes(type.lane) >= 4) continue;
            }

            auto scalar = scalarFormOf(type);
            auto bits = laneBytes(type.lane) * 8;

            // Which operand is read as signed, which is per operation. A shift count never is: it
            // is a distance, and a lane holding one that does not fit the lane is out of range in
            // any reading.
            auto signedLhs = inst->kind == LowerInst::IDiv || inst->kind == LowerInst::IRem
                          || inst->kind == LowerInst::Sar;
            auto signedRhs = inst->kind == LowerInst::IDiv || inst->kind == LowerInst::IRem;

            Expansion e { base, fun, block, i };

            // The sign put on a lane that arrived without one, where anything asks for it.
            LowerValue* distance = (signedLhs || signedRhs) && bits < 32
                                 ? e.integer(scalar, 32 - bits) : nullptr;

            auto extend = [&](LowerValue* value, bool wanted) {
                if(!distance || !wanted) return value;
                return e.binary(LowerInst::Sar, scalar,
                                e.binary(LowerInst::Shl, scalar, value, distance), distance);
            };

            /*
             * The left operand is the seed, which costs nothing: every lane of it is written before
             * the chain ends, so what it held is immaterial and a zero vector would be a constant to
             * materialize for no reason.
             */
            auto built = x;

            for(U32 index = 0; index < type.lanes(); index++) {
                auto lhs = extend(e.lane(x, U8(index)), signedLhs);
                auto rhs = extend(e.lane(b, U8(index)), signedRhs);
                auto answer = e.binary(inst->kind, scalar, lhs, rhs);

                built = e.withLane(type, built, U8(index), answer,
                                   index + 1 == type.lanes() ? operation->result.name : StringId());
            }

            replaceAllUses(base, &operation->result, built);
            removeInst(base, operation);

            // The last insert stands where the operation did, so the walk carries on past it.
            i = e.at - 1;
        }
    }
}

/*
 * The fused multiply-add, where the target has no instruction that fuses.
 *
 * `a * b + c` at two roundings rather than one, which is not an approximation of what was asked for
 * but the other thing the language permits: Design-Vector §3.3 makes `fma` a *permission* to fuse
 * rather than a promise, precisely so that a target without FMA3 can spend it as the two operations
 * it always meant. A program that must not fuse writes `a * b + c` itself and gets two roundings
 * everywhere; a program that writes `fma` is saying it does not care.
 *
 * Expanded into IR rather than into a pseudo, on `expandUnsignedConversions`' argument: the multiply
 * and the add are two instructions this backend already allocates, folds and costs.
 */
void expandFusedMultiplyAdd(Context&, LowerBase base, LowerFunction& fun) {
    if(targetFeatures() & kFeatureFma3) return;

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(Size i = 0; i < block->instructions.size(); ) {
            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::Fma) { i++; continue; }

            auto fma = (LowerInstFma*)inst;
            auto type = fma->result.type;

            Expansion e { base, fun, block, i };
            auto product = e.binary(LowerInst::Mul, type, base[fma->a], base[fma->b]);
            auto sum = e.binary(LowerInst::Add, type, product, base[fma->c], fma->result.name);

            replaceAllUses(base, &fma->result, sum);
            removeInst(base, fma);

            i = e.at;
        }
    }
}

/*
 * Unsigned packed comparisons, which no x86 has and every x86 can do.
 *
 * `pcmpgt` reads its lanes as signed and there is no unsigned twin at any feature level. What there
 * is instead is the identity that makes one: `a <u b` exactly when `(a ^ 0x80000000) <s (b ^
 * 0x80000000)`, because flipping the top bit maps the unsigned order onto the signed one. So an
 * unsigned relation is the signed one over both operands biased, which is two exclusive-ors and a
 * broadcast the folder hoists out of any loop it is invariant in.
 *
 * ~~A 32-bit lane only.~~ Every lane width the signed relations have: the bias is a constant splat,
 * which is pooled before it is anything, and a narrow one has a broadcast of its own now. A 64-bit
 * lane still has no `pcmpgtq` to bias *into* before SSE4.2, and `unsupportedVectorReason` states
 * that bound from the other side.
 *
 * This is what `firstSet` reaches, and reaching it is not obvious: the lane indices it compares are
 * small non-negative numbers whose signed and unsigned orders agree, and the *type* is what decides
 * which comparison the IR asks for. So the sequence below is exact where it is also unnecessary,
 * which is the ordinary case for it.
 *
 * ## The two non-strict relations take a shorter route
 *
 * `a <=u b` is `minu(a, b) == a` and `a >=u b` is `maxu(a, b) == a` - two instructions, no constant
 * and no mask inverted. What they replace is the worst case of the bias: `ile` is one of the three
 * relations the machine has only the *complement* of (`packedCompareIsInverted`), so an unsigned
 * `le` was two exclusive-ors and then `pcmpgt ; pcmpeqd ; pxor` through a scratch register - five
 * instructions where this is two.
 *
 * The two strict ones keep the bias, and that is a measurement rather than an omission: `a <u b`
 * through a minimum is `maxu(a, b) == a` complemented, which is the same inversion pseudo again and
 * comes to four, where the bias is two exclusive-ors and a `pcmpgt` whose constant is hoisted out of
 * any loop it stands in.
 */
void biasUnsignedPackedCompares(Context&, LowerBase base, LowerFunction& fun) {
    auto signedForm = [](LowerCmp cmp) {
        switch(cmp) {
            case LowerCmp::lt: return LowerCmp::ilt;
            case LowerCmp::le: return LowerCmp::ile;
            case LowerCmp::gt: return LowerCmp::igt;
            case LowerCmp::ge: return LowerCmp::ige;
            default:           return cmp;
        }
    };

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(Size i = 0; i < block->instructions.size(); ) {
            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::Cmp) { i++; continue; }

            auto cmp = (LowerInstCmp*)inst;
            auto type = base[cmp->lhs]->type;
            auto relation = signedForm(cmp->getCmp());

            if(relation == cmp->getCmp() || !isIntVector(type)) { i++; continue; }

            /*
             * The two non-strict relations, as a minimum or a maximum against one of the operands -
             * see the note above on why the strict pair is not done this way.
             *
             * The comparison is rewritten in place rather than replaced: it keeps its result, its
             * readers and its position, and what changes is which values it reads and which relation
             * it states. `a` stands in both the minimum and the equality, which is what makes this
             * two instructions rather than two and a copy.
             */
            auto written = cmp->getCmp();

            if((written == LowerCmp::le || written == LowerCmp::ge) && packedMinMaxSupported(type)) {
                Expansion pick { base, fun, block, i };
                auto lhs = base[cmp->lhs];
                auto rhs = base[cmp->rhs];

                auto minMax = pick.emit(new (fun.arena) LowerInstX86MinMax(
                    StringId(), type, lhs - base, rhs - base,
                    written == LowerCmp::le ? LowerMinMax::Min : LowerMinMax::Max
                ));

                setOperand(base, fun.arena, inst, cmp->lhs, minMax);
                setOperand(base, fun.arena, inst, cmp->rhs, lhs);
                cmp->setCmp(LowerCmp::eq);

                i = pick.at + 1;
                continue;
            }

            Expansion e { base, fun, block, i };

            // The top bit of every lane. Built once here and left to CSE and LICM, which is the
            // whole argument for expanding into IR: two comparisons in one block share this, and one
            // inside a loop leaves it outside.
            auto bit = e.integer(scalarFormOf(type), U64(1) << (laneBytes(type.lane) * 8 - 1));
            auto bias = e.splat(type, bit);
            auto lhs = e.binary(LowerInst::Xor, type, base[cmp->lhs], bias);
            auto rhs = e.binary(LowerInst::Xor, type, base[cmp->rhs], bias);

            setOperand(base, fun.arena, inst, cmp->lhs, lhs);
            setOperand(base, fun.arena, inst, cmp->rhs, rhs);
            cmp->setCmp(relation);

            // Past the four instructions the expansion added and past the comparison itself, which
            // is now a signed one and has nothing here to come back for.
            i = e.at + 1;
        }
    }
}

/*
 * `round`, which is the one of the four roundings SSE4.1 cannot encode.
 *
 * `roundsd`'s mode field names the four IEEE directions and ties-to-even; it has no ties-**away**,
 * and ties-away is what this language's `round` is (resolve/inst.def rules on it, and
 * lib/Math.yana has documented it since before there was an instruction). So this is the one
 * rounding the backend builds out of the others, and it is built here rather than in the library so
 * that LLVM and JS - both of which name it directly - do not pay for x64's gap.
 *
 * ## The identity
 *
 *     t = trunc(x)                  the integer part, toward zero
 *     c = trunc((x - t) + (x - t))  the carry: ±1 exactly when |fraction| >= 1/2, else ±0
 *     r = t + c
 *
 * `x - t` is exact - a difference of two values within a factor of two of each other is - and the
 * doubling is exact because the fraction is below one, so nothing here rounds and the comparison
 * against a half is made by the truncation rather than by a constant. Which is what makes it right
 * at `0.49999999999999994`, the input the library's old `trunc(x + copysign(0.5, x))` answered 1
 * for: doubling the largest double below a half gives the largest double below one, and truncating
 * that is zero. Adding a half to it rounds *up* to exactly 1 before any truncation can look.
 *
 * ## The guard, which is about infinity and not about size
 *
 * `x - t` is `Inf - Inf` for an infinite argument, which is a NaN, and `Inf + NaN` is a NaN - so an
 * infinity would come back as one. The test is `t == x`, which is true for every value that is
 * already integral (both infinities and both zeros among them) and false for a NaN, so a NaN takes
 * the arithmetic arm and stays a NaN. Testing a *magnitude* against 2^52 would answer the same
 * thing and would need a different constant per lane width; this needs none.
 *
 * Both arms are computed and one is selected. The discarded arm's NaN is a value, not a trap: none
 * of these operations signals.
 *
 * Scalar as well as packed, so this pass is not `vectorsOnly` - `Real(Double).round` is a scalar
 * `Round` and reaches selection by exactly this path.
 */
/*
 * `vzeroupper` on a machine that has no VEX-encoded instruction to have dirtied anything.
 *
 * `X86.vzeroupper()` is what a program writes at the entry of a legacy-encoded region so that no
 * upper half is non-zero when its first unprefixed vector instruction runs - see `Value::VZeroUpper`
 * in resolve/inst.def. Below AVX there is no encoding that could have left one non-zero and no
 * instruction to write, so the call is removed rather than standing anything in for it.
 *
 * It is not conditional in *source*, and that is the point of doing it here: the library says the
 * instruction it wants at the place it wants it, and which builds have something to emit for it is a
 * question about the target that only the backend can answer. `hasShaExtension` and the AVX level
 * are independent - Goldmont shipped SHA-NI with no AVX at all - so a build where this fires is a
 * real configuration rather than a hypothetical one.
 *
 * No operands and no result, so removal is the list edit and nothing else: there is no use list to
 * repair and nothing that named it.
 */
void dropUnsupportedVectorResets(Context&, LowerBase base, LowerFunction& fun) {
    if(targetFeatures() & kFeatureAvx) return;

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(Size i = 0; i < block->instructions.size();) {
            if(base[block->instructions.get(base, i)]->kind == LowerInst::VZeroUpper) {
                block->instructions.remove(base, i);
            } else {
                i++;
            }
        }
    }
}

void expandRoundAway(Context&, LowerBase base, LowerFunction& fun) {
    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::Round) continue;

            auto unary = (LowerInstUnary*)inst;
            auto type = unary->result.type;
            auto value = base[unary->from];

            Expansion at { base, fun, block, i };

            auto whole = at.emit(new (fun.arena) LowerInstUnary(LowerInst::Trunc, StringId(), type,
                                                               value - base));
            auto fraction = at.binary(LowerInst::Sub, type, value, whole);
            auto doubled = at.binary(LowerInst::Add, type, fraction, fraction);
            auto carry = at.emit(new (fun.arena) LowerInstUnary(LowerInst::Trunc, StringId(), type,
                                                                doubled - base));
            auto sum = at.binary(LowerInst::Add, type, whole, carry);

            /*
             * The comparison answers a *mask* of the operand's shape where a scalar one answers a
             * Bool, and `Expansion::compare` defaults to the scalar form - so a packed round built
             * through it handed the select a condition in the wrong register bank, which the
             * allocator then had to move between banks with a `movaps` and could not.
             */
            auto integral = isVectorLike(type)
                ? at.emit(new (fun.arena) LowerInstCmp(StringId(), whole - base, value - base,
                                                       LowerCmp::eq, maskType(type.lane, type.lanes())))
                : at.compare(LowerCmp::eq, whole, value);

            auto result = at.select(type, integral, value, sum);

            // The select stands where the round did, so the walk carries on past it - the same
            // bookkeeping expandVectorAbs does, and for the same reason.
            replaceAllUses(base, &unary->result, result);
            removeInst(base, inst);
            i = at.at - 1;
        }
    }
}

/*
 * The absolute value of a vector, which is one bit per lane for a float and a comparison for the one
 * integer lane width the machine has no `pabs` at.
 *
 * `LowerInst::Abs` is the magnitude and says nothing about how - see the `Abs` row in
 * resolve/inst.def, which is where the language rules that the sign of a NaN is unspecified. That
 * ruling is what lets this be one instruction: `v & 0x7fffffff` per lane leaves the exponent and the
 * mantissa exactly where they are, so every finite value, both infinities and both zeros come out
 * with the magnitude they had, and `-0.0` becomes `+0.0`.
 *
 * ## The integer quadword, which used to be a refusal
 *
 * `pabsb`/`pabsw`/`pabsd` are ordinary forms and never come here. The quadword has no `pabsq`
 * outside AVX-512, and `unsupportedVectorReason` refused it on the grounds that the comparison it
 * would fall back on was missing at that width too - which stopped being true when §38 named v2 the
 * floor, `pcmpgtq` being SSE4.2. So it is expanded rather than refused, into the shape the machine
 * has had all along:
 *
 *     %zeros    = vsplat 0                 -- hoisted, and a `pxor` rather than a pooled load
 *     %negative = cmp_igt %zeros, %x       -- pcmpgtq
 *     %negated  = neg %x                   -- pxor; psubq
 *     %r        = select %negative, %negated, %x
 *
 * Three instructions in a loop and a `pxor` outside it, against the eleven-odd a scalarized quadword
 * magnitude costs at two lanes - `pextrq`, the branchless negation, `pinsrq`, twice, and the bank
 * crossing each way. That is the bar Design-Vector property 5 sets and it clears it at every lane
 * count.
 *
 * It is written as a comparison and a select rather than as the shorter `(x ^ m) - m`, which is what
 * this would be if a mask could be read as a vector. It cannot: `validateBitcast` refuses a bitcast
 * between a mask and a vector outright, a mask's lanes being truth values rather than bits, and the
 * one instruction the trick saves is not worth an exception to that.
 *
 * ## The mask is an integer constant read as a float one
 *
 * `0x7fffffff` is a NaN when read as a float, and a float lane's immediate is held as a double in
 * this IR and narrowed where the bytes are taken - which is exact for every value in the language
 * and not for a NaN's payload. So the constant is built as an *integer* splat and bitcast to the
 * float vector the `and` works at, which `constantVectorBytes` reads through: the pool gets one
 * entry of the right bytes, and `andps` reads it in its own domain.
 *
 * ## The mask goes in the entry block, and that is the whole of what it is worth
 *
 * Built beside the `and`, the mask is the load *immediately above* its reader - and `tryFoldLoad`
 * takes the load immediately above, so the mask won the addressing mode and the value being
 * measured had to be loaded into a register first:
 *
 *     vmovups (%rdx),%ymm3 ; vandps 0x9b6(%rip),%ymm3,%ymm3
 *
 * Two instructions and, less obviously, **two loads**: the pooled mask is re-read from `.rodata`
 * every iteration. There is one r/m field, so only one operand can be the memory one, and the right
 * one to spend it on is the operand that changes. Built in the entry block, the mask leaves the loop
 * (once per function, and interned per lane width), the value's own load becomes the one above the
 * `and`, and the loop body is `vandps (%rdx),%ymmMask,%ymm3` - one instruction and one load.
 *
 * What that costs is a register held across the function, and it is the cheapest kind: a load of a
 * global nothing writes is rematerializable (`recipeFor` in place.cpp), so a function under pressure
 * spills it by forgetting it and re-loading where it is next read.
 */
void expandVectorAbs(Context&, LowerBase base, LowerFunction& fun) {
    // The block the mask is built in - see `constantHome`, which is where the argument for it is.
    auto home = constantHome(base, fun);
    if(!home) return;

    // One mask per lane width, built on demand and shared by every absolute value in the function -
    // interning it here rather than leaving it to CSE, which does not run below this point. The
    // third slot is the integer quadword's zero vector, which is interned for the same reason and
    // needs no width of its own: one lane shape reaches it.
    LowerValue* masks[2] = { nullptr, nullptr };
    LowerValue* zeros = nullptr;

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::Abs) continue;

            auto type = ((LowerInstUnary*)inst)->result.type;

            /*
             * The integer quadword, which is the comparison and the select above rather than a mask
             * and an `and`. Every narrower integer lane has `pabs` and never arrives here.
             */
            if(isIntVector(type) && laneBytes(type.lane) == 8) {
                if(!zeros) {
                    Expansion at { base, fun, home, 0 };
                    auto scalar = at.integer(scalarFormOf(type), 0);
                    zeros = at.splat(type, scalar);

                    if(block == home) i += 2;
                }

                auto value = base[((LowerInstUnary*)inst)->from];

                Expansion e { base, fun, block, i };

                // `zeros > x`, which is the operand order `pcmpgtq` has and the one
                // `canonicalizeOperands` would produce anyway from the other spelling.
                auto negative = e.emit(new (fun.arena) LowerInstCmp(
                    StringId(), zeros - base, value - base, LowerCmp::igt,
                    maskType(type.lane, type.lanes())));

                // `0 - x` rather than `neg x`, which is the same instruction with the zero already
                // in hand: the negation's form is a *pseudo* that materializes a zero of its own
                // into the scratch register and copies the difference back out, so writing it as the
                // subtraction it is saves an instruction and the scratch traffic with it.
                auto negated = e.binary(LowerInst::Sub, type, zeros, value);

                auto picked = e.select(type, negative, negated, value,
                                       ((LowerInstUnary*)inst)->result.name);

                replaceAllUses(base, &((LowerInstUnary*)inst)->result, picked);
                removeInst(base, inst);

                // The select stands where the magnitude did, so the walk carries on past it.
                i = e.at - 1;
                continue;
            }

            if(!isFloatVector(type)) continue;

            auto lane = laneBytes(type.lane);
            auto& mask = masks[lane == 4 ? 0 : 1];

            if(!mask) {
                /*
                 * At the *top* of that block, which is where a value with no operands may go and
                 * where it dominates every use by construction. The three instructions are an
                 * immediate, the splat over it and the reinterpretation - `poolVectorConstants`
                 * folds all three into one load of the whole pattern.
                 */
                auto integerLane = lane == 4 ? LowerLane::Int32 : LowerLane::Int64;
                auto integers = LowerType(integerLane, type.laneShift, false);
                auto bits = lane == 4 ? U64(0x7fffffff) : (~U64(0) >> 1);

                Expansion at { base, fun, home, 0 };
                auto scalar = at.integer(scalarFormOf(integers), bits);
                auto splat = at.splat(integers, scalar);
                mask = at.reinterpret(type, splat);

                // The walk is inside that block whenever this fires there, and three instructions
                // have just been put in front of it.
                if(block == home) i += 3;
            }

            Expansion e { base, fun, block, i };
            auto cleared = e.binary(LowerInst::And, type, base[((LowerInstUnary*)inst)->from], mask,
                                    ((LowerInstUnary*)inst)->result.name);

            replaceAllUses(base, &((LowerInstUnary*)inst)->result, cleared);
            removeInst(base, inst);

            // The `and` stands where the absolute value did, so the walk carries on past it.
            i = e.at - 1;
        }
    }
}

/*
 * The two bit scans whose zero case the machine may not have an instruction for.
 *
 * `CttzWidth` is `tzcnt` and `ClzWidth` is `lzcnt`, and both are v3. Below that level the machine
 * still has the *scans* - `bsf` and `bsr` are baseline, and have been since the 386 - and what it
 * has no encoding for is the answer to a zero operand: a scan leaves its destination unwritten
 * there, where the language's `trailingZeros` and `leadingZeros` are defined to answer the operand's
 * width (resolve/inst.def rules on it).
 *
 * So this is `expandRoundAway`'s case at an integer operation: the language committed to an answer,
 * one level of the target has the instruction for it and the floor does not, and the gap is spent
 * here rather than in the library so that neither LLVM nor JS pays for x64's feature ladder.
 *
 * ## What is emitted
 *
 *     t = bsf x            or   b = bsr x;  t = (width - 1) - b
 *     z = x == 0
 *     r = select(z, width, t)
 *
 * Two or four instructions, and the last of them is a `cmov` rather than a branch - the select is an
 * ordinary one from here down, and `expandRoundAway` leaves its own the same way. Nothing about the
 * scan's undefined answer escapes: the select discards it in exactly the case it is undefined in.
 *
 * The subtraction is written `(width - 1) - bsr` rather than as an exclusive-or with `width - 1`,
 * which is the same value for every index a scan can answer and one byte shorter. It is not written
 * that way because the two disagree on the undefined path - `bsr` of zero may leave any bits at all
 * in the register, and an exclusive-or would carry them into the arm the select is about to drop,
 * where a subtraction of a garbage index is equally garbage but the *flags* it writes are not read.
 * Neither is wrong; the subtraction is the one that reads as what it means.
 *
 * Not `vectorsOnly`: a bit count is an integer operation and reaches a function with no packed value
 * in it at all - which is most of them.
 */
void expandBitScans(Context&, LowerBase base, LowerFunction& fun) {
    auto features = targetFeatures();
    auto haveTzcnt = (features & kFeatureBmi1) != 0;
    auto haveLzcnt = (features & kFeatureLzcnt) != 0;

    if(haveTzcnt && haveLzcnt) return;

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::Intrinsic) continue;

            auto intrinsic = (LowerInstIntrinsic*)inst;
            auto which = intrinsic->getIntrinsic();
            auto leading = which == LowerIntrinsic::ClzWidth;

            if(which != LowerIntrinsic::CttzWidth && !leading) continue;
            if(leading ? haveLzcnt : haveTzcnt) continue;

            auto result = intrinsic->created().ptr;
            auto type = result->type;
            auto value = base[intrinsic->used().ptr[0]];
            auto width = is64Bit(type) ? 64 : 32;

            Expansion e { base, fun, block, i };

            auto scanned = e.intrinsic(leading ? LowerIntrinsic::Bsr : LowerIntrinsic::Cttz,
                                       type, value);
            auto counted = leading
                ? e.binary(LowerInst::Sub, type, e.integer(type, U64(width - 1)), scanned)
                : scanned;

            auto empty = e.compare(LowerCmp::eq, value, e.integer(type, 0));
            auto answered = e.select(type, empty, e.integer(type, U64(width)), counted);

            replaceAllUses(base, result, answered);
            removeInst(base, inst);

            // Past what was added, and back one for the instruction that is no longer there - the
            // bookkeeping `expandRoundAway` does, and for its reason.
            i = e.at - 1;
        }
    }
}

/*
 * A rotation over a vector, which x86 has only at AVX-512.
 *
 * `vprold`/`vprolq` (and the per-lane `vprolvd`) are EVEX, and nothing below that level has a packed
 * rotation at any lane width. So this is `expandFusedMultiplyAdd`'s case at an integer operation:
 * the IR states the operation once because a target may have it, and the target that does not
 * expands it into what it always meant.
 *
 *     rol(v, c) = (v << (c & (w - 1))) | (v >> ((w - c) & (w - 1)))
 *
 * with `w` the *lane* width, not the register's. Both masks are needed and neither is the machine's
 * own: a packed shift does not mask its count the way a general-register shift does - it *saturates*,
 * answering all-zero lanes for a count at or past the lane width - so a count of `w` would give zero
 * where the operation is defined to give the value back. Masking the second one is what turns the
 * zero count's `w` into a 0, and masking the first is the modulus the kind promises.
 *
 * ## The count is reduced where it *lives*, and that is the whole of the pass's difficulty
 *
 * A shared count reaches here as a splat, because `Integral(a)` types both operands as the same `a`
 * and `unwrapVectorShiftCounts` has not run yet. The arithmetic above must not be done to the splat:
 * a shift whose count is an `and` of a splat is no longer a shift whose count *is* a splat, so that
 * pass would not unwrap it, and a packed shift with a per-lane count is `vpsllv` - which v2 does not
 * have at all, so the whole thing would be scalarized into four `vlane`/`shl`/`vwithlane` triples
 * per direction. Measured before this read through the splat: forty-two instructions for one
 * rotation by a constant.
 *
 * So the reduction happens on the *scalar* behind the splat, and the shifts are handed that scalar
 * directly - which is exactly the shape `unwrapVectorShiftCounts` would have left, so everything
 * below this pass sees an ordinary shared-count shift. The orphaned splat is removed here for the
 * reason it is removed there: while it stands it is a second reader of the count, and a constant
 * something else needs in a register is a constant `canEmbedImm` will not embed.
 *
 * A count that is genuinely per-lane - two vectors, which the language can write - keeps the vector
 * arithmetic and is scalarized below, which is the honest cost of an operation the machine has no
 * form for at this level.
 *
 * A *constant* count is reduced here rather than left to a fold, because no fold runs between this
 * pass and selection: both distances are computed and emitted as immediates, leaving the two shifts
 * and the `or` that a hand-written rotation would be.
 */
void expandVectorRotate(Context&, LowerBase base, LowerFunction& fun) {
    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::Rol && inst->kind != LowerInst::Ror) continue;

            auto rotate = (LowerInstBinary*)inst;
            auto type = rotate->result.type;
            if(!isVectorLike(type)) continue;

            auto value = base[rotate->lhs];
            auto count = base[rotate->rhs];
            auto left = inst->kind == LowerInst::Rol;
            auto width = U64(laneBytes(type.lane)) * 8;

            // The scalar behind a shared count, or nothing where the count is one per lane.
            auto splat = count->inst()->kind == LowerInst::VecSplat ? count : nullptr;
            auto shared = splat ? base[((LowerInstVecSplat*)splat->inst())->from] : nullptr;
            auto written = shared && shared->inst()->kind == LowerInst::Imm
                         ? (LowerImm*)shared->inst() : nullptr;

            Expansion e { base, fun, block, i };

            LowerValue* forward;
            LowerValue* backward;

            if(written) {
                auto n = written->i & (width - 1);
                forward = e.integer(shared->type, n);
                backward = e.integer(shared->type, (width - n) & (width - 1));
            } else {
                // The count's own shape: the scalar where one is shared, the vector where each lane
                // has its own. The two differ only in where the constants have to be splatted.
                auto countType = shared ? shared->type : count->type;
                auto source = shared ? shared : count;

                auto constant = [&](U64 n) {
                    auto scalar = e.integer(scalarFormOf(countType), n);
                    return isVectorLike(countType) ? e.splat(countType, scalar) : scalar;
                };

                auto mask = constant(width - 1);

                forward = e.binary(LowerInst::And, countType, source, mask);
                backward = e.binary(LowerInst::And, countType,
                                    e.binary(LowerInst::Sub, countType, constant(width), source), mask);
            }

            auto up = e.binary(LowerInst::Shl, type, value, left ? forward : backward);
            auto down = e.binary(LowerInst::Shr, type, value, left ? backward : forward);
            auto joined = e.binary(LowerInst::Or, type, up, down);

            replaceAllUses(base, &rotate->result, joined);
            removeInst(base, inst);

            // Past what was added and back one for the instruction that is gone - `expandRoundAway`'s
            // bookkeeping, and for its reason.
            i = e.at - 1;

            // And the splat this read through, on `unwrapVectorShiftCounts`' terms exactly: it goes
            // only if nothing else reads it, and the constant under it goes only if it is one.
            auto removeAndTrack = [&](LowerInst* dead) {
                if(base[dead->block] == block) i--;
                removeInst(base, dead);
            };

            if(!splat || !splat->uses.isEmpty()) continue;

            removeAndTrack(splat->inst());

            if(shared->uses.isEmpty() && shared->inst()->kind == LowerInst::Imm) {
                removeAndTrack(shared->inst());
            }
        }
    }
}

/*
 * The three bit operations of `Core.Bits.bitsUpTo` and `Core.BitPermute`, which are one instruction
 * each at v3 and a sequence below it.
 *
 * `expandBitScans`' case at three more operations, and the same argument: the language committed to
 * an answer, one level of this target has an instruction for it, the floor does not, and the gap is
 * spent here so that neither LLVM nor JS pays for x64's feature ladder. What differs is how wide the
 * gap is, and that is worth reading before the code.
 *
 * ## `bitsUpTo`, which is cheap either way
 *
 * `bzhi` is the instruction and it *almost* answers the operation. The one place the two part
 * company is a count at or above the width: `bzhi` reads its count from the low byte of a register,
 * so 256 is a count of zero there, where `LowerInst::BitsUpTo` says every count at or above the
 * width answers the value unchanged. The difference is one unsigned saturation:
 *
 *     index  = count <u width ? count : width
 *     result = bzhi(value, index)
 *
 * Three instructions, and a *constant* count leaves none of them - `lower_fold.cpp` has folded the
 * whole operation long before this, into an `and` against a literal mask or into the value itself.
 * So the saturation is paid exactly where the count is a value, which is the tail-mask case it
 * exists for.
 *
 * Below BMI2 the same guard stands in front of the arithmetic the instruction replaces:
 *
 *     small  = count <u width
 *     masked = count & (width - 1)
 *     low    = value & ((1 << masked) - 1)
 *     result = small ? low : value
 *
 * The mask on the shift count is not the guard repeated. The guard chooses which arm survives; the
 * mask is what keeps the arm that does *not* survive from being a shift by an out-of-range count,
 * which x86 answers by masking and other machines answer differently. Both arms are computed either
 * way - a select is a `cmov` from here down - so the dead one has to be a defined value rather than
 * merely an unread one.
 *
 * ## The permutations, which are not cheap without the instruction
 *
 * `pext` and `pdep` are one instruction at v3. Below it there is no instruction, no short sequence
 * and no shape a `cmov` can close: what the machine is being asked for is an arbitrary permutation
 * of bits, and the standard answer is the parallel-suffix network of Hacker's Delight figures 7-6
 * and 7-11 - five rounds at 32 bits and six at 64, each round about twenty operations.
 *
 * **That is ninety to a hundred and thirty instructions, and the number is the honest cost of the
 * operation on a machine that does not have it.** Two things make it the right answer anyway:
 *
 *  - It is straight-line and constant-time. A loop over the set bits of the mask is a dozen
 *    instructions of *code* and a data-dependent number of iterations, and it would need this pass
 *    to build a loop inside the backend - which is CFG surgery below the point where the block order,
 *    the dominators and every phi have already been settled.
 *  - **It folds away against a constant mask.** Every value in the network above is derived from the
 *    mask alone, so a `gatherBits(x, 0x0f0f0f0f)` leaves the five rounds of mask arithmetic entirely
 *    to `lower_fold.cpp` and keeps only the four operations per round that touch the value - about
 *    twenty instructions, and fewer once the constant rounds that move nothing drop out. A constant
 *    mask is what a Morton code, a bitfield decoder and a magic-bitboard index all have.
 *
 * A runtime mask on a target below v3 is the expensive case, and it is the case the feature level
 * exists for. Nothing here tries to hide it.
 */

// The parallel suffix of `x` - every bit set where an odd number of bits at or below it is set, which
// is `x ^= x << 1; x ^= x << 2; ...` up to half the width. Both networks below open each round with
// one of these, over the mask alone.
static LowerValue* parallelSuffix(Expansion& e, LowerType type, LowerValue* x, U32 width) {
    for(U32 shift = 1; shift < width; shift *= 2) {
        auto up = e.binary(LowerInst::Shl, type, x, e.integer(type, shift));
        x = e.binary(LowerInst::Xor, type, x, up);
    }

    return x;
}

// `~x`, written as the kind rather than as a `xor` against all ones: the kind is what the BMI1
// peephole reads, so a complement built here can still become half of an `andn`.
static LowerValue* complement(Expansion& e, LowerType type, LowerValue* x) {
    return e.emit(new (e.fun.arena) LowerInstUnary(LowerInst::Not, StringId(), type, x - e.base));
}

/*
 * The mask arithmetic both networks share - Hacker's Delight figure 7-6's loop body, minus the two
 * lines that touch the value.
 *
 * Each round works out which bits of the mask move by one, two, four... positions, compresses the
 * mask by that much, and hands the round's `mv` back so that the caller can do the same to the
 * value it is permuting. `mk` counts the zeros to the right and is threaded through unchanged.
 */
struct PermuteRound {
    LowerValue* mv;
    LowerValue* mask;
    LowerValue* mk;
};

static PermuteRound permuteRound(Expansion& e, LowerType type, U32 width, U32 step,
                                 LowerValue* mask, LowerValue* mk)
{
    auto mp = parallelSuffix(e, type, mk, width);
    auto mv = e.binary(LowerInst::And, type, mp, mask);

    auto moved = e.binary(LowerInst::Shr, type, mv, e.integer(type, step));
    auto kept = e.binary(LowerInst::Xor, type, mask, mv);

    return PermuteRound {
        mv,
        e.binary(LowerInst::Or, type, kept, moved),
        e.binary(LowerInst::And, type, mk, complement(e, type, mp)),
    };
}

// The bits of `value` at the set positions of `mask`, packed down - Hacker's Delight figure 7-6.
static LowerValue* expandGather(Expansion& e, LowerType type, U32 width, LowerValue* value, LowerValue* mask) {
    auto x = e.binary(LowerInst::And, type, value, mask);
    auto mk = e.binary(LowerInst::Shl, type, complement(e, type, mask), e.integer(type, 1));

    for(U32 step = 1; step < width; step *= 2) {
        auto round = permuteRound(e, type, width, step, mask, mk);
        mask = round.mask;
        mk = round.mk;

        auto t = e.binary(LowerInst::And, type, x, round.mv);
        auto moved = e.binary(LowerInst::Shr, type, t, e.integer(type, step));
        x = e.binary(LowerInst::Or, type, e.binary(LowerInst::Xor, type, x, t), moved);
    }

    return x;
}

/*
 * The inverse - Hacker's Delight figure 7-11.
 *
 * The rounds are the same and are run in the same direction, because compressing the mask is what
 * discovers how far each group of bits has to travel; what changes is that the *value* is moved in a
 * second pass afterwards, in the opposite order, since a deposit spreads bits out where an extract
 * packed them in. Each round's `mv` is therefore kept rather than spent as it is produced.
 *
 * The trailing `and` against the original mask is not tidiness. The value being deposited may have
 * bits above the ones the mask has room for, and the network leaves them where the last shift put
 * them; `pdep` answers zero for every position the mask does not name, so this does too.
 */
static LowerValue* expandScatter(Expansion& e, LowerType type, U32 width, LowerValue* value, LowerValue* mask) {
    auto original = mask;
    auto mk = e.binary(LowerInst::Shl, type, complement(e, type, mask), e.integer(type, 1));

    SmallArray<LowerValue*, 6> moves;
    SmallArray<U32, 6> steps;

    for(U32 step = 1; step < width; step *= 2) {
        auto round = permuteRound(e, type, width, step, mask, mk);
        mask = round.mask;
        mk = round.mk;

        moves.push(round.mv);
        steps.push(step);
    }

    auto x = value;

    for(auto i = moves.size(); i-- > 0;) {
        auto mv = moves[i];
        auto up = e.binary(LowerInst::Shl, type, x, e.integer(type, steps[i]));

        auto stay = e.binary(LowerInst::And, type, x, complement(e, type, mv));
        auto move = e.binary(LowerInst::And, type, up, mv);
        x = e.binary(LowerInst::Or, type, stay, move);
    }

    return e.binary(LowerInst::And, type, x, original);
}

void expandBitOperations(Context&, LowerBase base, LowerFunction& fun) {
    auto haveBmi2 = (targetFeatures() & kFeatureBmi2) != 0;

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];

            auto kind = inst->kind;
            if(kind != LowerInst::BitsUpTo && kind != LowerInst::GatherBits
                && kind != LowerInst::ScatterBits) continue;

            auto binary = (LowerInstBinary*)inst;
            auto type = binary->result.type;
            auto width = U32(is64Bit(type) ? 64 : 32);
            auto lhs = base[binary->lhs];
            auto rhs = base[binary->rhs];

            Expansion e { base, fun, block, i };
            LowerValue* answered = nullptr;

            if(kind == LowerInst::BitsUpTo) {
                auto limit = e.integer(type, width);
                auto small = e.compare(LowerCmp::lt, rhs, limit);

                if(haveBmi2) {
                    auto index = e.select(type, small, rhs, limit);
                    answered = e.intrinsic2(LowerIntrinsic::Bzhi, type, lhs, index);
                } else {
                    auto one = e.integer(type, 1);
                    auto masked = e.binary(LowerInst::And, type, rhs, e.integer(type, width - 1));
                    auto bit = e.binary(LowerInst::Shl, type, one, masked);
                    auto mask = e.binary(LowerInst::Sub, type, bit, e.integer(type, 1));
                    auto low = e.binary(LowerInst::And, type, lhs, mask);

                    answered = e.select(type, small, low, lhs);
                }
            } else if(haveBmi2) {
                answered = e.intrinsic2(kind == LowerInst::GatherBits ? LowerIntrinsic::Pext
                                                                     : LowerIntrinsic::Pdep,
                                        type, lhs, rhs);
            } else {
                answered = kind == LowerInst::GatherBits
                    ? expandGather(e, type, width, lhs, rhs)
                    : expandScatter(e, type, width, lhs, rhs);
            }

            replaceAllUses(base, &binary->result, answered);
            removeInst(base, inst);

            // Past what was added, and back one for the instruction that is no longer there - the
            // bookkeeping `expandBitScans` does, and for its reason.
            i = e.at - 1;
        }
    }
}
