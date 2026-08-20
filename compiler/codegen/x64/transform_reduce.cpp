#include "transform_internal.h"

/*
 * A vector answered as a scalar.
 *
 * Three things, and they are one subject because they share the movemask: the butterfly that folds
 * every lane of a vector into one, the `pmovmskb` that every consumer of a mask reads its answer
 * out of, and the scan-and-guard the branch above it fuses into a single instruction.
 */

/*
 * Every lane of a vector combined into one scalar.
 *
 * A tree of shuffles and pairwise operations, log2(lanes) deep, expanded into IR - which is what
 * §5.3 of Implementation-Vector.md asks for and for the reason it gives: every instruction this
 * produces is then allocated, folded and costed by machinery that already exists, where a pseudo
 * would have needed a scratch vector register for each level of the tree.
 *
 * **The tree is a butterfly and not a compaction**, which is the one thing here that is a decision
 * rather than a transcription. The order a floating-point reduction combines in is a stated language
 * property (Design-Vector §4.5) - `(a0+a1) + (a2+a3)` for four lanes - so the shape has to be the
 * adjacent-pair tree the other two backends build and not the "fold the upper half down" idiom,
 * which for four lanes gives `(a0+a2) + (a1+a3)` and a different answer. Written as a compaction
 * that would have been two shuffles per level (the even lanes, then the odd ones); written as a
 * butterfly - lane `j` paired with lane `j ^ s`, doubling `s` - it is *one*, because every lane
 * holds the combination of its own group at every level and lane zero holds the one that is wanted.
 *
 * What comes out of the bottom is a lane extract, which is the same instruction the tree is built on
 * top of and needs nothing of its own.
 */

// One level of the tree: the two vectors combined the way this reduction says. `min` and `max` are a
// comparison and a select rather than an instruction, which is the same shape `emitMinMax` gives
// them in the library - the machine has `minps` and this backend has no form for it yet.
static LowerValue* reduceStep(Expansion& e, LowerReduce reduce, LowerType type,
                              LowerValue* lhs, LowerValue* rhs)
{
    auto compareAndSelect = [&](LowerCmp cmp) {
        // The comparison answers a mask of the operands' shape, which is the one thing about a
        // vector comparison that is not the scalar instruction unchanged.
        auto mask = e.emit(new (e.fun.arena) LowerInstCmp(
            StringId(), lhs - e.base, rhs - e.base, cmp, maskType(type.lane, type.lanes())));

        return e.select(type, mask, lhs, rhs);
    };

    switch(reduce) {
        case LowerReduce::Add: return e.binary(LowerInst::Add, type, lhs, rhs);
        case LowerReduce::Mul: return e.binary(LowerInst::Mul, type, lhs, rhs);
        case LowerReduce::And: return e.binary(LowerInst::And, type, lhs, rhs);
        case LowerReduce::Or:  return e.binary(LowerInst::Or, type, lhs, rhs);

        // `min` keeps the left operand where it compares less, and `max` where it compares greater -
        // so a NaN in either position follows the comparison, which answers false and yields the
        // right-hand side. That is what `minps` does with its operands in this order, and what the
        // library's `min` already promises.
        case LowerReduce::Min:  return compareAndSelect(LowerCmp::lt);
        case LowerReduce::IMin: return compareAndSelect(LowerCmp::ilt);
        case LowerReduce::Max:  return compareAndSelect(LowerCmp::gt);
        default:                return compareAndSelect(LowerCmp::igt);
    }
}

/*
 * A reduction of a **mask**, which is four questions rather than one arithmetic operation.
 *
 * `and` and `or` combine the lanes as they stand and answer a truth value: a mask lane is all-ones
 * or all-zeros here, so lane zero of the combined mask is `-1` or `0`, and `& 1` is the whole of
 * turning that into the `Bool` the result type asks for.
 *
 * `add` is the odd one and is `count`: how many lanes are set. Summing the mask itself would answer
 * the negative of it - every set lane is `-1` - and, worse, would be an `add` over two masks, which
 * is a thing the lower IR does not admit at all (a mask holds truth values). So the mask is turned
 * into a vector of ones and zeros with a select first, and what is summed is that.
 *
 * `first` is `firstSet` and has no tree at all: it is a lane *index*, and the only reason it is a
 * reduction kind rather than the `select(mask, iota, splat(lanes))` chain it used to be written as
 * is that every machine answers it in one step and no two answer it alike.
 *
 * `bits` is the movemask the three below reach it through, and the three things a caller may hand in
 * are about it: one already placed for a mask that several of them read, whether what comes back is
 * already a 0/1 rather than the -1/0 a mask lane holds, and whether those bits are the complement of
 * the mask this reduction was written about (§45.3).
 */
static LowerValue* expandMaskBitsReduce(Expansion& e, LowerReduce reduce, LowerValue* source,
                                        LowerValue* bits, bool& truth, bool complemented);

/*
 * A reduction of an 8- or 16-bit lane, which is the butterfly above finished in a general register.
 *
 * The tree needs a shuffle that pairs lane `i` with lane `i ^ stride`, and this machine's only
 * integer shuffle moves 32-bit lanes - `pshufd`. So the levels split in two by where the partner is:
 *
 * - **A partner a whole 32-bit lane away or further** is that same `pshufd`, applied to the register
 *   read as `i32` and read back as itself. Both bitcasts are the register and emit nothing; the
 *   *combining* step stays at the narrow lane, which is where the lane width has to be honoured.
 * - **A partner inside one 32-bit lane** has no shuffle at all, so the last one or two levels are
 *   done after the value has crossed to a general register - which it has to do anyway, since what
 *   a reduction answers is a scalar.
 *
 * The order is free here in a way it is not for a float: Design-Vector §4.5 fixes the *pairing* order
 * because floating-point addition is not associative, and there is no floating-point lane narrower
 * than four bytes. Every operation this reaches is associative and commutative over the integers, so
 * doing the wide levels first and the narrow ones last answers the same number.
 *
 * The scalar finish is two shapes rather than one. `add`, `and`, `or` and `mul` combine the whole
 * word against itself shifted down - the low lane of the result is exact whatever the lanes above it
 * hold, because a carry only ever travels upward - and the answer is the low lane truncated. `min`
 * and `max` cannot borrow that: they need each sub-lane as a value of its own, so the word is cut
 * into its two or four pieces and the pieces are compared at the lane's own width and signedness.
 */
static LowerValue* reduceScalarStep(Expansion& e, LowerReduce reduce, LowerType type,
                                    LowerValue* lhs, LowerValue* rhs) {
    auto compareAndSelect = [&](LowerCmp cmp) {
        auto flag = e.compare(cmp, lhs, rhs);
        return e.select(type, flag, lhs, rhs);
    };

    switch(reduce) {
        case LowerReduce::Add: return e.binary(LowerInst::Add, type, lhs, rhs);
        case LowerReduce::Mul: return e.binary(LowerInst::Mul, type, lhs, rhs);
        case LowerReduce::And: return e.binary(LowerInst::And, type, lhs, rhs);
        case LowerReduce::Or:  return e.binary(LowerInst::Or, type, lhs, rhs);
        case LowerReduce::Min:  return compareAndSelect(LowerCmp::lt);
        case LowerReduce::IMin: return compareAndSelect(LowerCmp::ilt);
        case LowerReduce::Max:  return compareAndSelect(LowerCmp::gt);
        default:                return compareAndSelect(LowerCmp::igt);
    }
}

static LowerValue* expandNarrowReduce(Expansion& e, LowerReduce reduce, LowerValue* value) {
    auto type = value->type;
    auto width = laneBytes(type.lane);
    auto bits = width * 8;
    auto perWord = 4 / width;
    auto lanes = type.lanes();

    // The same register read as 32-bit lanes, which is what the shuffle below is expressed in. Both
    // bitcasts are the register itself and emit nothing.
    auto wide = LowerType(LowerLane::Int32, U8(type.laneShift - (perWord == 4 ? 2 : 1)), false);

    for(U32 stride = perWord; stride < lanes; stride *= 2) {
        auto step = stride / perWord;
        auto asWide = e.reinterpret(wide, value);
        auto partnerWide = e.shuffle(wide, asWide, asWide, [&](Size j) { return U8(j ^ step); });
        auto partner = e.reinterpret(type, partnerWide);

        value = reduceStep(e, reduce, type, value, partner);
    }

    // Lane zero of the 32-bit view, which now holds every sub-lane's own answer - `movd`, and the
    // one lane extract this backend has at every feature level.
    auto word = e.lane(e.reinterpret(wide, value), 0);
    auto scalar = LowerType::Int32;

    /*
     * Every sub-lane cut out of the word as a value of its own, and then combined.
     *
     * ~~`add`, `and`, `or` and `mul` combine the whole word against itself shifted down - the low
     * lane of the result is exact whatever the lanes above it hold, because a carry only ever
     * travels upward.~~ **That was wrong, and quietly.** The argument holds for one step and fails
     * for the next: the second step reads a *higher* sub-lane, and the first step's carry landed
     * there. Sixteen byte lanes of 32 summed to 1 rather than to 0, because 0x80 + 0x80 carried into
     * the byte the second step then read. There is no cheap repair - masking between the steps costs
     * exactly what cutting the pieces costs - so both shapes take the same route now, which is the
     * one `min` and `max` always had to take.
     *
     * An unsigned piece is masked and a signed one is shifted up and back down, which is the sign
     * extension the lane's own width asks for. Only the two orderings ask: `add` and the bitwise
     * pair agree about the low `bits` whichever way the pieces were read, so they take the cheaper
     * mask and the answer is put back inside the lane at the end.
     */
    auto signedLane = reduce == LowerReduce::IMin || reduce == LowerReduce::IMax;

    auto piece = [&](U32 index) {
        if(signedLane) {
            /*
             * The top sub-lane's own sign bit is already the register's, so there is nothing to
             * raise it by and the shift is `x << 0` - which is the same guard the unsigned half
             * below has always had at index zero, on the other end of the word.
             *
             * Written here rather than left to a fold, because nothing folds it: `lower_fold.cpp`
             * takes a shift by zero to its operand, and that runs a whole tier above this file. What
             * an expansion builds below it is emitted as written.
             */
            auto distance = 32 - bits - index * bits;
            auto up = distance == 0
                ? word
                : e.binary(LowerInst::Shl, scalar, word, e.integer(scalar, distance));

            return e.binary(LowerInst::Sar, scalar, up, e.integer(scalar, 32 - bits));
        }

        auto down = index == 0 ? word
                               : e.binary(LowerInst::Shr, scalar, word, e.integer(scalar, index * bits));

        return e.binary(LowerInst::And, scalar, down, e.integer(scalar, (U64(1) << bits) - 1));
    };

    auto best = piece(0);
    for(U32 i = 1; i < perWord; i++) best = reduceScalarStep(e, reduce, scalar, best, piece(i));

    /*
     * And the bits above the lane, cleared, which is the contract rather than tidiness: a narrow
     * lane leaves this backend **zero-extended**, exactly as a lane extract does and exactly as the
     * LLVM backend's `outOfLane` writes it. `count(mask)` reads the answer as a whole `Int` and
     * would see a sum's carry bits; a signed lane gets its sign back one tier up, in `Value::VecReduce`
     * in resolve/lower_calc.cpp, which is the last place that knows the lane had a signedness.
     */
    return e.binary(LowerInst::And, scalar, best, e.integer(scalar, (U64(1) << bits) - 1));
}

/*
 * The tree over one vector, answering the scalar it reduces to.
 *
 * A scalar rather than the vector whose lane zero holds it, because the narrow route below has no
 * such vector: its last levels happen after the value has crossed to a general register. The wide
 * route ends in the lane extract it always did, which is one instruction either way.
 */
static LowerValue* reduceTree(Expansion& e, LowerReduce reduce, LowerValue* value, LowerType type) {
    if(laneBytes(type.lane) < 4) return expandNarrowReduce(e, reduce, value);

    auto lanes = type.lanes();

    // The butterfly. At stride `s` every lane is paired with the lane `s` above or below it, which
    // after `log2(lanes)` doublings leaves lane zero holding the whole tree - and holding it in the
    // adjacent-pair order, since a lane's partner at each level is the other half of its own group.
    for(U32 stride = 1; stride < lanes; stride *= 2) {
        auto partner = e.shuffle(type, value, value, [&](Size i) { return U8(i ^ stride); });
        value = reduceStep(e, reduce, type, value, partner);
    }

    return e.lane(value, 0);
}

static LowerValue* expandReduce(Expansion& e, LowerReduce reduce, LowerValue* source,
                                LowerValue* bits, bool& truth, bool complemented = false) {
    auto type = source->type;

    if(type.isMask()) return expandMaskBitsReduce(e, reduce, source, bits, truth, complemented);
    return reduceTree(e, reduce, source, type);
}

/*
 * A mask read through `pmovmskb` - §34.2 of test/bench/findings.md.
 *
 * One instruction turns the whole mask into an integer with a bit per *byte*, and all four mask
 * consumers are then ordinary scalar arithmetic on it. What it replaces is a reduction tree: `any`
 * was three `pshufd`/`por` levels, a lane extract and six general-register instructions, `count`
 * was that plus a select against two splats it had to build first, and `firstSet` was a blend per
 * level over a vector of lane indices - about forty instructions where a bit scan is one.
 *
 * **A bit per byte, not per lane.** A mask lane is all-ones or all-zeros by construction, so a
 * four-byte lane contributes four identical bits: `any` and `all` do not care, `count` divides and
 * `firstSet` shifts. The full pattern is `1 << bytes` minus one - sixteen bits at 128 and thirty-two
 * at 256 - and it is computed from the type rather than written down, because the two tiers share
 * this code.
 *
 * `count` needs a population count, which is an instruction only where the target claims it. Without
 * it the tree below is still the shorter of the two - a SWAR population count is a dozen general
 * -register instructions - so the fallback is the code this replaced rather than a second expansion.
 */

/*
 * How many bits of the movemask one lane contributes.
 *
 * One, wherever the machine has an instruction that says so. `movmskps` and `movmskpd` read the sign
 * bit of each 32- or 64-bit element - which for a mask is the lane, a mask lane being all-ones or
 * all-zeros - and hand back exactly the bitmap every consumer below wants. `pmovmskb` reads a bit
 * per *byte*, so a 16-bit lane contributes two and every consumer of one pays a shift to divide them
 * back out; there is no `movmskw` and that is why the shift survives at one lane width.
 *
 * An 8-bit lane takes `pmovmskb` as well and has no shift either way, a bit per byte already being a
 * bit per lane there.
 *
 * **This and the form selection in machine_vector.cpp are one decision made twice**, which is the hazard
 * worth naming: choosing `movmskps` there and leaving the shift here would divide a bitmap that was
 * never multiplied. See selectPackedForm's `VecReduce` arm.
 */
static U64 maskBitsPerLane(LowerType type) {
    auto width = laneBytes(type.lane);
    return width == 4 || width == 8 ? 1 : width;
}

// The same as a shift: a bit index is a lane index shifted left by this, which is what `count`
// divides out and `firstSet` undoes. Zero at three of the four lane widths.
static U64 maskBitShift(LowerType type) {
    U64 shift = 0;
    for(auto bits = maskBitsPerLane(type); bits > 1; bits /= 2) shift++;

    return shift;
}

// And how many bits of the movemask are the mask's at all - which is what "nothing is set above the
// lanes" means, and what the sentinel `firstSet` uses sits at.
static Size maskBitCount(LowerType type) {
    return Size(type.lanes() * maskBitsPerLane(type));
}

// The movemask itself. One instruction, and the one instruction every consumer below starts from -
// which is why `lowerVectorReductions` may place it once for a mask several of them read.
static LowerValue* emitMaskBits(Expansion& e, LowerValue* source) {
    return e.emit(new (e.fun.arena) LowerInstVecReduce(StringId(), LowerType::Int32, source - e.base,
                                                       LowerReduce::Bits));
}

/*
 * `firstSet` off the movemask: the lowest set bit of it, shifted back into a lane index.
 *
 * **Two sequences, and which one is chosen is decided by whether the movemask fills its word.**
 * Both answer the same three things - the lowest set lane, the lane count where none is set, and
 * nothing undefined in between - and they answer the third differently because the machine does.
 *
 * *Where the bits leave room above them*, which is a 128-bit register's sixteen, the sentinel does
 * two jobs and is one instruction: setting the bit one past the last lane byte makes "nothing is
 * set" answer the lane count - the scan finds the sentinel and the shift turns it into `lanes` - and
 * it is also what keeps the operand non-zero, which `Cttz` needs because `bsf` leaves its
 * destination undefined at zero.
 *
 * *Where they fill it*, which is a `ymm`'s thirty-two bytes, there is no bit to set: bit 32 is not a
 * bit an `i32` has. `tzcnt` answers the operand's width for a zero operand and the width is 32,
 * which is the byte count, which the shift turns into the lane count - so the sentinel is not
 * replaced by something wider, it is *not needed*, and the sequence is one instruction shorter than
 * the one that has it rather than longer.
 *
 * **The second is the one that generalizes and the first is the one that is portable**, which is why
 * both are here. A 512-bit mask is a `k` register with a bit per lane rather than per byte, so
 * sixty-four byte lanes fill sixty-four bits with nothing above them, and the sentinel has no width
 * to live at however wide the arithmetic is made; `tzcnt` at 64 answers 64, which is the lane count
 * again. The sentinel survives because it needs no feature, and BMI1 is claimed only from AVX2 -
 * which is exactly the level at which a movemask first fills its word.
 */
static LowerValue* expandMaskFirstSet(Expansion& e, LowerValue* bits, LowerType type) {
    auto scalar = LowerType::Int32;
    auto width = maskBitCount(type);
    auto shift = maskBitShift(type);

    /*
     * A movemask that fills its word comes from a register wider than 128 bits, and a vector that
     * wide needs AVX2 - `unsupportedVectorReason` refuses one without it - so the feature the scan
     * needs is implied by the shape that needs the scan. Asserted rather than branched on, because a
     * fallback here would be a path no target description can reach: see x64FeaturesFor, which is
     * where the two are tied together.
     *
     * One bit per lane is what narrowed this case rather than widening it: only a `pmovmskb` of a
     * `ymm` fills the word now, which is an 8- or a 16-bit lane at 256 bits. A 32-bit lane's eight
     * bits leave twenty-four above them and take the sentinel, which needs no feature at all.
     */
    assertTrue(width <= 32); // a wider register's mask is a `k` bit per lane, and is not this yet

    if(width == 32) {
        assertTrue(targetFeatures() & kFeatureBmi1);

        auto first = e.intrinsic(LowerIntrinsic::CttzWidth, scalar, bits);
        return shift ? e.binary(LowerInst::Shr, scalar, first, e.integer(scalar, shift)) : first;
    }

    auto marked = e.binary(LowerInst::Or, scalar, bits, e.integer(scalar, U64(1) << width));
    auto first = e.intrinsic(LowerIntrinsic::Cttz, scalar, marked);

    return shift ? e.binary(LowerInst::Shr, scalar, first, e.integer(scalar, shift)) : first;
}

/*
 * §45.3 A mask read complemented, and the three reductions that need no complement at all.
 *
 * `complemented` says that `bits` is the movemask of the *opposite* of the mask this reduction was
 * written about - see `foldComplementedCompare` below, which is what arranges that. The bitmap of a
 * complemented mask is the bitmap exclusive-ored with the pattern, so one instruction would answer
 * it; three of the four consumers do not need even that:
 *
 *     all(!m)   = none(m)        the bitmap against zero
 *     any(!m)   = !all(m)        the bitmap against the full pattern
 *     count(!m) = lanes - count(m)
 *
 * The first two are the *same* comparison this emits either way with the constant swapped, so they
 * cost nothing whatsoever. The population is written as the exclusive-or rather than as a
 * subtraction from the lane count, which is the same instruction count and one fewer live value -
 * and `firstSet` has no identity of its own and takes the exclusive-or as well.
 */
static LowerValue* expandMaskBitsReduce(Expansion& e, LowerReduce reduce, LowerValue* source,
                                        LowerValue* bits, bool& truth, bool complemented = false) {
    auto type = source->type;
    auto scalar = LowerType::Int32;
    auto width = maskBitCount(type);
    auto full = width < 32 ? (U64(1) << width) - 1 : ~U64(0);

    if(!bits) bits = emitMaskBits(e, source);

    // The two that read the bits themselves rather than a property of them, which is where the
    // complement has to become an instruction. Only the mask's own bits are flipped: nothing above
    // them is the mask's, and `firstSet` reads a set bit there as a lane.
    if(complemented && (reduce == LowerReduce::FirstSet || reduce == LowerReduce::Add)) {
        bits = e.binary(LowerInst::Xor, scalar, bits, e.integer(scalar, full));
    }

    if(reduce == LowerReduce::FirstSet) return expandMaskFirstSet(e, bits, type);

    if(reduce == LowerReduce::Add) {
        auto counted = e.intrinsic(LowerIntrinsic::Popcnt, scalar, bits);

        // Every lane contributed `maskBitsPerLane` equal bits, so the population is the lane count
        // times that - and it is a power of two, so the division is a shift. Three of the four lane
        // widths contribute one bit and need no shift at all; the 16-bit lane is the one that does.
        auto shift = maskBitShift(type);

        return shift ? e.binary(LowerInst::Shr, scalar, counted, e.integer(scalar, shift)) : counted;
    }

    /*
     * `any` is "not zero" and `all` is "every byte set", and what is answered is the *comparison* -
     * so a branch on it reads the flags the comparison left standing and spends nothing at all.
     *
     * It used to be a select of one and zero, which the caller then narrowed with an `& 1` because
     * the tree this replaced handed back the -1 a mask lane holds. Both are gone: a comparison
     * already answers 0 or 1, so the narrowing is a no-op, and materializing it is `genSetCc`'s job
     * on the paths that genuinely want a value. `if any(hits)` was `xor ; test ; mov ; cmove ; and ;
     * jne` and is now `test ; jne` - four instructions per iteration of every search loop.
     */
    /*
     * The pattern is signed at the width the comparison happens at, which is the difference between
     * a three-byte instruction and nine. A mask filling a whole 256-bit register wants every bit of
     * the `i32` set, and that written as `0xffffffff` is a constant no immediate carries - so it was
     * materialized into a register, and in a leaf function that register cost a callee-saved push
     * and pop as well. The same number as an `i32` is -1, which is an `imm8`.
     */
    /*
     * `any` is a comparison against zero and `all` one against the full pattern; complemented, each
     * keeps its relation and takes the *other's* constant. That is the identity above written out -
     * `any(!m)` is "not every bit set" and `all(!m)` is "no bit set" - and it is why these two cost
     * nothing at all: the instruction emitted is the same instruction with a different immediate.
     */
    auto isAny = reduce == LowerReduce::Or;
    auto wanted = isAny != complemented ? U64(0) : full;

    truth = true;
    return e.compare(isAny ? LowerCmp::neq : LowerCmp::eq, bits, e.integer(scalar, wanted));
}

/*
 * §37 One movemask for every consumer of a mask.
 *
 * `if any(hits) then return Just(at + firstSet(hits))` is two consumers of one mask, in two blocks,
 * and expanded one at a time it is two `pmovmskb`s of the same register - the second on the path
 * where the first has already answered. The instruction is the same instruction; what stops the
 * expansion from saying so is that it runs after the tier where common subexpressions are removed,
 * so nothing below it will notice.
 *
 * So the movemask is placed *once*, immediately below the instruction that defines the mask. That
 * position needs no dominance computation to justify: a definition dominates every use of what it
 * defines, and an instruction directly under it dominates exactly what it does - so a movemask there
 * is readable from every consumer of the mask, in this block and in every block below.
 *
 * **Only where there is more than one consumer**, which is the whole of what keeps it from being a
 * hoist. A single reader has its movemask expanded where it stands, as before; moving that one up to
 * the definition would lengthen a general-register live range across whatever lies between, and buy
 * nothing. A mask defined by a phi or by an argument has no position "below the definition" in an
 * instruction list at all, and takes the same path.
 */

// The mask a reduction reads, where that reduction is one that goes through the movemask - which
// is every mask reduction, and nothing else.
static LowerValue* maskBitsSource(LowerBase base, LowerInst* inst) {
    if(inst->kind != LowerInst::VecReduce) return nullptr;

    auto reduce = (LowerInstVecReduce*)inst;
    auto source = base[reduce->from];

    if(!source->type.isMask()) return nullptr;
    if(reduce->getReduce() == LowerReduce::Bits) return nullptr;   // the movemask itself

    return source;
}

/*
 * How many reductions read this mask through the movemask, and whether *every* reader of it is one
 * that a live-lane range can serve.
 *
 * The count is what decides whether a movemask is placed once or expanded where it stands. The
 * second answer is what §41.3's fusion needs and is stricter in two ways, because that rewrite
 * removes the `and` rather than reading it:
 *
 *   - a use that is not a mask reduction at all keeps the `and` alive, so the range would be a
 *     `bzhi` placed *beside* the vector comparison rather than instead of it, and
 *   - `all` cannot be served by ranged bits at any price. It asks whether every lane holds, which
 *     is the movemask against a full pattern - and the whole of what the range does is clear the
 *     bits above the live lanes, so the pattern would have to become the range.
 */
static Size maskBitsReaders(LowerBase base, LowerValue* source, bool& rangeable) {
    Size readers = 0;
    rangeable = true;

    for(auto use: source->uses.contents(base)) {
        auto inst = base[use];

        if(maskBitsSource(base, inst) != source) {
            rangeable = false;
            continue;
        }

        if(((LowerInstVecReduce*)inst)->getReduce() == LowerReduce::And) rangeable = false;
        readers++;
    }

    return readers;
}

/*
 * The point immediately below the instruction that defines this value - which dominates exactly what
 * the definition does, and is where anything shared between the definition's readers goes.
 *
 * A **phi** takes the top of its own block, which is the same statement one line up: a phi is not in
 * the instruction list and has no slot below it, but every value it defines is live from the head of
 * its block, so position zero dominates exactly what the phi does. That is the shape a search whose
 * mask comes from two arms has, and without it such a mask pays a movemask per consumer.
 *
 * An **argument** is declined, and that is a judgement rather than a limitation. The only position
 * that dominates every use of one is the top of the entry block, which may be an arbitrary distance
 * - a call, a loop - from the consumers that would read it, and a mask handed in as a parameter is
 * not a shape this language's own vector code produces. It keeps a movemask per consumer, which is
 * what every consumer had before any of this.
 */
static bool positionBelowDefinition(LowerBase base, LowerValue* value, LowerBlock*& block, Size& at) {
    auto definition = value->inst();

    block = base[definition->block];
    at = 0;

    if(isPhi(definition)) return true;

    auto list = block->instructions.contents(base);
    while(at < list.size() && base[list[at]] != definition) at++;
    if(at == list.size()) return false; // an argument: see above

    at++;
    return true;
}

// One movemask below the mask's definition, for a mask more than one consumer reads. Where that
// position is, and which definitions have one at all, is `positionBelowDefinition` above.
static LowerValue* placeSharedMaskBits(LowerBase base, LowerFunction& fun, LowerValue* source) {
    LowerBlock* block = nullptr;
    Size at = 0;

    if(!positionBelowDefinition(base, source, block, at)) return nullptr;

    Expansion e { base, fun, block, at };
    return emitMaskBits(e, source);
}

/*
 * §52 A mask two arms join, read as the bits its two arms already computed.
 *
 * `if any(hits) then return at + firstSet(hits)` written in a loop *and* in the tail after it is two
 * such tests branching into one block, and the merge of the two arms is a phi of the two masks:
 *
 *   b_loop: %h1 = cmp ... ; %b1 = movmsk %h1 ; test %b1 ; jne b_hit
 *   b_tail: %h2 = and ... ; %b2 = movmsk %h2 ; test %b2 ; jne b_hit
 *   b_hit:  %h  = phi [b_loop, %h1], [b_tail, %h2]
 *           %b  = movmsk %h                        <- a third movemask of a mask already measured
 *           bsf (%b | sentinel)
 *
 * §37 places one movemask per mask and that is exactly what it did here: three masks, three
 * movemasks. What it cannot see is that the third is a *join* of the other two - and a movemask
 * distributes over one, because it is a function of its operand alone. So the phi moves into the
 * scalar domain: `%b = phi [b_loop, %b1], [b_tail, %b2]`, and the vector phi dies with it.
 *
 * **Each alternative's bits are reused rather than computed**, which is what makes this free rather
 * than a trade. An arm that branches here on `any` has measured its own mask to do it, so the bits
 * this joins are the ones the guard already holds - `existingMaskBits` finds that movemask, and a
 * placement below the definition is the fallback for an arm whose own reduction has not been reached
 * yet. Either way no movemask is added; one is removed, and the phi that replaces it holds a general
 * register where the old one held a vector across the join.
 *
 * The bits of a *complemented* mask (§45.3) are the complement of the bits asked about, so the
 * alternatives have to agree about that: the flag is carried out to the caller, which records it for
 * the joined value exactly as it would for a shared one. Disagreement is a refusal rather than an
 * exclusive-or per arm, since nothing produces one - a complement comes from the shape of the
 * comparison, and the arms of a search are the same shape.
 */

// A movemask of this mask that every reader of the mask can see. Only one in the definition's own
// block is accepted: the definition dominates wherever the mask is live, so a movemask beside it
// does too, and one *elsewhere* would need the dominator tree to justify.
static LowerValue* existingMaskBits(LowerBase base, LowerValue* mask) {
    for(auto use: mask->uses.contents(base)) {
        auto inst = base[use];
        if(inst->kind != LowerInst::VecReduce) continue;
        if(((LowerInstVecReduce*)inst)->getReduce() != LowerReduce::Bits) continue;
        if(base[inst->block] != base[mask->inst()->block]) continue;

        return &((LowerInstVecReduce*)inst)->result;
    }

    return nullptr;
}

// Whether any reduction reads this mask through the movemask - which is what says that a movemask
// placed for it now is work an expansion below was going to do anyway.
static bool hasMaskBitsReader(LowerBase base, LowerValue* mask) {
    for(auto use: mask->uses.contents(base)) {
        if(maskBitsSource(base, base[use]) == mask) return true;
    }

    return false;
}

/*
 * The mask sources a function's reductions have settled, and how each was settled.
 *
 * A function has a handful of these at most - one per mask a search or a tally reads - so the lookup
 * is a walk rather than a map. `bits` is the movemask placed once where every consumer can see it,
 * where one was placed at all; `complemented` says the comparison under it was rewritten to the
 * relation the machine has (§45.3), so the bits every consumer reads are the opposite of the ones it
 * asked about. An entry may carry the second without the first.
 */
struct SharedMaskBits {
    LowerValue* source;
    LowerValue* bits;
    bool complemented;
};

using SharedMaskList = SmallArray<SharedMaskBits, 8>;

static LowerValue* placeJoinedMaskBits(LowerBase base, LowerFunction& fun, LowerValue* source,
                                       SharedMaskList& shared, bool& complemented)
{
    auto phi = (LowerInstPhi*)source->inst();
    auto block = base[phi->block];
    auto count = Size(phi->usedCount);

    // The bits each alternative contributes, null where one is still to be placed. Every alternative
    // is settled before *any* movemask is placed, because a refusal after the first would leave the
    // arm's own bits somewhere the arm did not choose.
    SmallArray<LowerValue*, 8> alternatives;
    auto complementedAll = false;

    for(Size i = 0; i < count; i++) {
        auto incoming = base[phi->used()[i]];
        if(!incoming->type.isMask()) return nullptr;

        LowerValue* bits = nullptr;
        auto flipped = false;

        for(auto& entry: shared) {
            if(entry.source != incoming) continue;

            bits = entry.bits;
            flipped = entry.complemented;
            break;
        }

        if(!bits) bits = existingMaskBits(base, incoming);

        if(!bits) {
            LowerBlock* at = nullptr;
            Size index = 0;

            // An alternative no reduction reads is one whose movemask would be new work, and the
            // join saves exactly one - so paying for it per arm is a trade rather than a saving. One
            // with nowhere to put a movemask is an argument, which `positionBelowDefinition` refuses
            // for reasons of its own.
            if(!hasMaskBitsReader(base, incoming)) return nullptr;
            if(!positionBelowDefinition(base, incoming, at, index)) return nullptr;
        }

        if(i && flipped != complementedAll) return nullptr;

        complementedAll = flipped;
        alternatives.push(bits);
    }

    for(Size i = 0; i < count; i++) {
        if(alternatives[i]) continue;

        auto incoming = base[phi->used()[i]];
        alternatives[i] = placeSharedMaskBits(base, fun, incoming);

        // Recorded, so that the arm's own reduction reads this movemask rather than emitting a
        // second one where it stands - which is what makes the placement free.
        shared.push(SharedMaskBits { incoming, alternatives[i], false });
    }

    auto joined = makePhi(fun.arena, LowerType::Int32, U32(count));
    auto used = joined->used();
    auto sources = joined->sources();

    for(Size i = 0; i < count; i++) {
        used[i] = alternatives[i] - base;
        sources[i] = phi->sources()[i];
    }

    block->addInst(base, joined);

    complemented = complementedAll;
    return &joined->result;
}

/*
 * `none`, which arrives as `any` and a negation and leaves as one comparison.
 *
 * The library writes `none(m)` as `any(m)` exclusive-ored with one - the negation of a `Bool` is
 * arithmetic, and above this tier there is nothing to negate but the value. What that costs once the
 * reduction has become a comparison is the whole materialization the comparison was meant to avoid:
 * `test ; setne ; xor $1 ; jne` where the answer wanted is `test ; je`.
 *
 * So the comparison is inverted instead, which is exact - it answers 0 or 1 by construction, and the
 * negation of a bit is the other relation. Asked here rather than as a peephole over every `xor`,
 * because here the comparison's uses are known to be the reduction's: it was created three lines ago
 * with the uses the reduction had and nothing else, which is what makes rewriting it rather than
 * copying it sound.
 */
static void foldNegatedTruth(LowerBase base, LowerValue* value) {
    if(value->uses.size() != 1) return;

    auto use = base[value->uses.get(base, 0)];
    if(use->kind != LowerInst::Xor) return;

    auto negation = (LowerInstBinary*)use;
    auto lhs = base[negation->lhs];
    auto other = lhs == value ? base[negation->rhs] : lhs;

    /*
     * The constant read as an instruction rather than through `isImm`, which asks a different
     * question: that one means "already embedded into its reader", and the embedding happens in
     * `selectMachineInstructions` several passes below this. Here every constant is still an
     * instruction of its own, and the one this looks for is the `1` a negation is written as.
     */
    if(other == value || other->inst()->kind != LowerInst::Imm) return;
    if(((LowerImm*)other->inst())->i != 1) return;

    // The two the mask expansion produces, and the two a truth value can be compared with. Written
    // out rather than negated generically because a signed relation has no business here at all.
    auto cmp = (LowerInstCmp*)value->inst();
    auto kind = cmp->getCmp();

    if(kind == LowerCmp::neq) cmp->setCmp(LowerCmp::eq);
    else if(kind == LowerCmp::eq) cmp->setCmp(LowerCmp::neq);
    else return;

    replaceAllUses(base, &negation->result, value);
    removeInst(base, negation);
}

/*
 * §41.3 The live-lane range of a masked tail, taken out of the vector bank.
 *
 * Every bulk operation in `resolve/core.cpp` is written so that the last chunk contributes only the
 * lanes that are really there:
 *
 *     count(m .& maskUpTo(live))
 *
 * and `maskUpTo(n)` is `iota .< splat(n)`. Written out, that is a general register moved into a
 * vector one, a broadcast, a comparison against `iota` - which is a 32-byte constant held in
 * `.rodata` and in a register for the whole function, plus the bias constant an *unsigned* lane
 * comparison needs and the two exclusive-ors that apply it - and then an `and` per consumer. Eight
 * instructions and three pooled constants to say "only the first `n` lanes count".
 *
 * Every one of those consumers goes through a movemask (§37), and a bit range of a general register
 * is one instruction: `bzhi dst, bits, n` keeps the low `n` bits and clears the rest. So the range
 * stops being a vector at all. What it takes away is not the `and` - it is `iota`, its bias, the two
 * registers holding them across the loop and the `.rodata` they sat in.
 *
 * ## The index, and the one thing the machine will not do
 *
 * `bzhi` reads its count from the *low byte* of its operand and clears nothing when that byte is at
 * or above the register width. That is the right answer for a count larger than the lane count -
 * every lane is live - and it is the wrong answer twice over otherwise: a *negative* count reads as
 * 255 and would answer "all lanes" where `iota <s n` answers none, and a count of 256 reads as zero
 * and would answer "no lanes" where the truth is all of them.
 *
 * So the count has to be known to be a small non-negative number before this is worth anything, and
 * `laneRangeIndex` below is where that is established rather than assumed. Two proofs, and between
 * them they cover what the library writes:
 *
 *   - **the high bits are known clear**, which `knownZeroBits` answers directly. A byte lane's count
 *     arrives as `n .& 255` because that is the lane's own width, so every string search and count
 *     is this case and pays nothing at all.
 *   - **the block is guarded by the comparison the count is a subtraction of**. `live = n - i` in a
 *     block entered only when `i <s n` cannot be negative, which is exactly the shape a chunked
 *     loop's tail has. The upper end is then one unsigned `min` against the lane count - three
 *     instructions in a block that runs once per call, against six per call in the vector bank.
 *
 * A count neither proof reaches is left as the vector comparison it was written as.
 *
 * ## §45.1 The range is one scalar, however many consumers read it
 *
 * `occurrencesVectors` has one consumer of the masked result and `indexOfVectors` has two - `any` in
 * the tail block and `firstSet` on the arm below it, both of `and(v .== sought, maskUpTo(live))`.
 * The second was refused outright while the fusion insisted on a single reader, and the whole vector
 * bank stayed for it: `iota` and its bias in `.rodata` and in two registers, a broadcast, a compare
 * and an `and`, so that two reductions of one mask could read the vector the range was written as.
 *
 * They do not read the vector. Both read the *movemask* of it, which §37 already places once below
 * the mask's definition - and a range over that placed movemask is one more instruction in the same
 * position, shared on exactly the same terms:
 *
 *     %bits = vpmovmskb %hits      one movemask of the data mask alone
 *     %live = bzhi %bits, %n       the range, applied once
 *
 * So the sharing is not a second mechanism. `placeFusedRangeBits` puts the pair where
 * `placeSharedMaskBits` puts the movemask - immediately below the `and`, which dominates every
 * consumer of it because the `and` does - and every consumer then reads `%live` as if it had been
 * the movemask, because for every consumer other than `all` that is what it is.
 *
 * What that leaves is a use count rather than a use: the `and` dies once the last reduction reading
 * it has been rewritten, which may be in a block below the one the range was placed in. That is why
 * the dead chain here is swept once for the whole function rather than once per block.
 */

struct LaneRange {
    LowerValue* mask = nullptr;    // the data mask the range is applied to
    LowerValue* count = nullptr;   // how many lanes are live, as a scalar
    LowerInst* combine = nullptr;  // the `and` of the two
    LowerInst* compare = nullptr;  // `iota REL splat(count)`
    LowerInst* splat = nullptr;    // the count moved into the vector bank, which is what stops
    bool ordered = false;          // the relation is signed, so a negative count means no lanes
    InstChain chain;       // the `iota` constant's own chain
};

// Whether these bytes are `0, 1, 2, ...` read at the lane width - `iota`, and the only constant this
// recognizes. Read little-endian per lane, which is what `constantVectorBytes` wrote.
static bool bytesAreIota(const U8* bytes, LowerType type) {
    auto width = laneBytes(type.lane);

    for(Size lane = 0; lane < type.lanes(); lane++) {
        U64 value = 0;
        copyMem(bytes + lane * width, &value, width);
        if(value != lane) return false;
    }

    return true;
}

/*
 * `and(m, iota REL splat(n))`, taken apart - or nothing.
 *
 * The relation is read rather than assumed: `iota .< splat(n)` is what the library writes, and the
 * lane type decides whether that is the signed or the unsigned comparison. Both are recognized and
 * which one it was is carried out, because it is what says whether a negative count is a question at
 * all - an unsigned lane has no negative counts to worry about.
 *
 * A **constant** count is declined and left to `foldConstantMasks`, which answers it exactly: the
 * full chunks of the same loop go through this identical line with `n` equal to the lane count, and
 * a mask that is all-ones should disappear rather than become a `bzhi` of a literal.
 *
 * *Who* reads the masked result is not asked here - `maskBitsReaders` is, and its `rangeable` is the
 * whole of the condition: every reader a reduction that goes through the movemask, and none of them
 * `all`. One reader and several are both served, and the difference is only where the `bzhi` goes.
 */
static bool matchLaneRangeMask(LowerBase base, LowerValue* source, LaneRange& into) {
    auto combine = source->inst();
    if(combine->kind != LowerInst::And || !source->type.isMask()) return false;

    auto binary = (LowerInstBinary*)combine;

    for(Size side = 0; side < 2; side++) {
        auto range = base[side ? binary->lhs : binary->rhs];
        auto mask = base[side ? binary->rhs : binary->lhs];
        auto compare = range->inst();

        if(compare->kind != LowerInst::Cmp || range->uses.size() != 1) continue;

        auto cmp = (LowerInstCmp*)compare;
        auto relation = cmp->getCmp();
        if(relation != LowerCmp::lt && relation != LowerCmp::ilt) continue;

        auto constant = base[cmp->lhs];
        auto splat = base[cmp->rhs];
        auto type = constant->type;

        if(!isIntVector(type) || splat->inst()->kind != LowerInst::VecSplat) continue;

        auto count = base[((LowerInstVecSplat*)splat->inst())->from];
        if(count->inst()->kind == LowerInst::Imm) continue; // see above: a fold, not a range

        auto size = Size(type.byteWidth());
        if(size > kMaxVectorBytes) continue;

        U8 bytes[kMaxVectorBytes] = {};

        // Collected straight into the result rather than into a list of its own and copied across.
        // Emptied on the way in because this loop has two sides and the failing one has to leave
        // nothing behind; what `into` holds is only read when this returns true.
        into.chain.clear();

        if(!constantVectorBytes(base, constant, bytes, size, into.chain)) continue;
        if(!bytesAreIota(bytes, type)) continue;

        into.mask = mask;
        into.count = count;
        into.combine = combine;
        into.compare = compare;
        into.splat = splat->inst();
        into.ordered = relation == LowerCmp::ilt;

        return true;
    }

    return false;
}

/*
 * Whether this value cannot be negative where the block below it runs.
 *
 * Two answers, and the second is the one that exists for this. `knownZeroBits` is the general one
 * and covers everything the front end masked on the way in; the guard is the shape a chunked tail
 * has, and nothing weaker reaches it - `live = n - i` is a subtraction of two values neither of
 * which is bounded on its own.
 *
 * The guard is read *locally*: the block has one predecessor, and that predecessor branches here on
 * exactly the comparison the subtraction is of. No dominator tree, and no reasoning about paths -
 * one predecessor is what makes "the branch was taken" true of every entry to this block.
 */
static bool isNonNegativeIn(LowerBase base, LowerBlock* block, LowerValue* value) {
    if(knownZeroBits(base, value) & (U64(1) << 31)) return true;

    auto inst = value->inst();
    if(inst->kind != LowerInst::Sub) return false;

    auto subtraction = (LowerInstBinary*)inst;
    if(block->incoming.size() != 1) return false;

    auto from = base[block->incoming.get(base, 0)];
    auto terminator = base[from->terminator];
    if(terminator->kind != LowerInst::Je) return false;

    auto branch = (LowerInstJe*)terminator;
    if(base[branch->then] != block) return false; // the arm where the comparison held

    auto condition = base[branch->cond]->inst();
    if(condition->kind != LowerInst::Cmp) return false;

    auto cmp = (LowerInstCmp*)condition;
    auto relation = cmp->getCmp();

    // `a - b` is not negative where `b < a` or `b <= a` held, at either signedness - the unsigned
    // pair as well, since a difference the unsigned relation makes non-negative is one the signed
    // reading of `Int` agrees about for every value below 2^31.
    auto ordered = relation == LowerCmp::ilt || relation == LowerCmp::ile;
    if(!ordered && relation != LowerCmp::lt && relation != LowerCmp::le) return false;

    return base[cmp->lhs] == base[subtraction->rhs] && base[cmp->rhs] == base[subtraction->lhs];
}

/*
 * The count as a `bzhi` index, or nothing where it cannot be made into one safely.
 *
 * Three things have to hold of what is handed to the instruction, and each is either proved or paid
 * for: it is not negative (proved, or the range is declined), it is not above 255 (an unsigned `min`
 * against the lane count, unless the bits above the byte are already known clear), and it is the
 * count *scaled* by whatever a lane is worth in the movemask - which after §41.5 is one bit at three
 * of the four lane widths and needs no scaling at all.
 */
static LowerValue* laneRangeIndex(Expansion& e, const LaneRange& range, LowerType type) {
    auto scalar = LowerType::Int32;
    auto count = range.count;
    auto shift = maskBitShift(type);
    auto lanes = U64(type.lanes());

    // An unsigned relation has no negative counts to rule out; a signed one has, and a count this
    // cannot place above zero would answer "every lane" where the comparison answers "none".
    if(range.ordered && !isNonNegativeIn(e.base, e.block, count)) return nullptr;

    /*
     * Whether the scaled count is the whole of its own low byte, which is what makes the machine's
     * saturation at the register width the right answer and the `min` below unnecessary.
     *
     * Asked as "every bit from here up is known zero", the position being what the scaling leaves
     * room for: a count that has to be shifted left by one may reach 127 rather than 255. A byte
     * lane's count arrives as `n .& 255` and clears the question outright.
     */
    auto known = knownZeroBits(e.base, count);
    auto highBits = U64(0xffffffff) & ~((U64(1) << (8 - shift)) - 1);

    if((known & highBits) != highBits) {
        // `min(count, lanes)` unsigned, which is a compare and a conditional move. Correct at both
        // ends: a count above the lane count means every lane is live and `lanes` says exactly that,
        // and the comparison is unsigned because by here the count is known not to be negative.
        auto limit = e.integer(scalar, lanes);
        auto within = e.compare(LowerCmp::lt, count, limit);
        count = e.select(scalar, within, count, limit);
    }

    return shift ? e.binary(LowerInst::Shl, scalar, count, e.integer(scalar, shift)) : count;
}

/*
 * The range-limited movemask, placed once below the `and` it replaces - §45.1 above.
 *
 * Two instructions and whatever the index costs, in a position that dominates every reader of the
 * `and` for the reason `placeSharedMaskBits` gives about the movemask alone. The count is available
 * there without asking: it is read by the splat, which is read by the comparison, which is read by
 * the `and` this sits under, so its definition dominates this point through three edges of the same
 * chain.
 *
 * A refusal costs nothing to back out of. `laneRangeIndex` declines before it emits - the proof it
 * cannot make is the first thing it asks - so a null here has left the block exactly as it was.
 */
static LowerValue* placeFusedRangeBits(LowerBase base, LowerFunction& fun, LowerValue* source,
                                       const LaneRange& range) {
    LowerBlock* block = nullptr;
    Size at = 0;

    if(!positionBelowDefinition(base, source, block, at)) return nullptr;

    Expansion e { base, fun, block, at };
    auto index = laneRangeIndex(e, range, source->type);
    if(!index) return nullptr;

    return e.intrinsic2(LowerIntrinsic::Bzhi, LowerType::Int32, emitMaskBits(e, range.mask), index);
}

// What a fused range leaves behind: the `and`, the comparison that built it, the splat under that
// and the `iota` chain under the comparison. Each goes only once its own use list has emptied, which
// is `removeDeadChain`'s rule and what keeps an `iota` two masked tails share exactly where it is.
/*
 * §45.3 A comparison the machine has only the complement of, complemented after the movemask.
 *
 * Three of the six relations a signed lane can be compared with are not instructions: `neq` is
 * `pcmpeq` inverted, and `ile` and `ige` are `pcmpgt` inverted. Inverting a *vector* is an all-ones
 * register and an exclusive-or against it, so each of the three costs two extra vector instructions
 * and a register held for the constant - `VecLanewise.comparisons` has three copies of
 *
 *     pcmpgtd  xmm2, xmm1
 *     pcmpeqd  xmm15, xmm15
 *     pxor     xmm2, xmm15
 *     movmskps eax, xmm2
 *
 * in one 182-byte function. But a mask whose every reader is a reduction is never *looked at* as a
 * vector: what each of them reads is the movemask, and the complement of a bitmap is a scalar
 * operation on a value that is already in a general register.
 *
 * So the comparison is rewritten to the relation the machine has, and its readers are told the bits
 * they are given are the opposite ones. `expandMaskBitsReduce` is where that is paid for, and for
 * three of the four consumers it is not paid at all - `all` and `any` become each other with the
 * constant swapped, and `count` is the lane count less the count. Only `firstSet` and the population
 * need the exclusive-or, and one scalar instruction against two vector ones is still the trade.
 *
 * **Every reader has to be a reduction**, which is the whole of the condition. A mask that is also
 * selected with, stored, or combined with another mask is one whose lanes are the value, and
 * rewriting the comparison under it would be answering a different question. `Bits` reductions are
 * not in the set either, which is what makes this safe to ask only at the first consumer: a movemask
 * placed for a mask is a reader of it, so a mask that already has one is a mask this declines.
 */
static bool foldComplementedCompare(LowerBase base, LowerValue* source) {
    auto inst = source->inst();
    if(inst->kind != LowerInst::Cmp) return false;

    auto cmp = (LowerInstCmp*)inst;
    auto type = base[cmp->lhs]->type;
    if(!isIntVector(type)) return false;

    // The machine's own question, asked of the relation as it will be selected - and asked twice,
    // because what makes this worth doing is that the *negation* is an instruction where this is
    // not. `neq`, `ile` and `ige` are the three that answer yes to the first and no to the second.
    auto relation = cmp->getCmp();
    auto negated = negatedCmp(relation);

    if(!packedCompareIsInverted(type, packedCompareRelation(relation))) return false;
    if(packedCompareIsInverted(type, packedCompareRelation(negated))) return false;

    /*
     * Nothing but reductions, each of which goes through the movemask - and at most one of them a
     * reduction the complement costs an instruction.
     *
     * The vector complement is paid *once* however many readers there are: the mask is inverted
     * where it is built. So a scalar one has to be paid once too, and `count` and `firstSet` each
     * pay their own - two of them is one instruction traded for two, which the vector register the
     * all-ones constant occupies does not make up for. `any` and `all` are free at any number, being
     * the same comparison against the other constant.
     */
    Size complements = 0;

    for(auto use: source->uses.contents(base)) {
        auto inst = base[use];
        if(maskBitsSource(base, inst) != source) return false;

        auto kind = ((LowerInstVecReduce*)inst)->getReduce();
        if(kind == LowerReduce::FirstSet || kind == LowerReduce::Add) complements++;
    }

    if(complements > 1) return false;

    cmp->setCmp(negated);
    return true;
}

static void pushFusedRange(InstChain& dead, const LaneRange& range) {
    dead.push(range.combine);
    dead.push(range.compare);
    dead.push(range.splat);
    for(auto link: range.chain) dead.push(link);
}

void lowerVectorReductions(Context&, LowerBase base, LowerFunction& fun) {
    // What this walk has settled, per mask - see SharedMaskBits, which the joined placement below
    // reads and writes as well.
    SharedMaskList shared;

    // The mask phis a joined placement emptied, swept once the walk is done: each loses its last
    // reader when the reduction that read it is expanded, and a phi nothing reads is still a live
    // range the allocator would carry a vector register through the join for.
    SmallArray<LowerInstPhi*, 4> joined;

    /*
     * What a fused lane range leaves behind: the `and`, the comparison that built the range, the
     * splat under it and the `iota` chain under that.
     *
     * Cleared once the *function* has been walked rather than once per block, which is §45.1's one
     * structural consequence. Each of these stands above the reduction being expanded, so removing
     * one during the walk would renumber what the loop indexes - and the `and` of a mask two blocks
     * read is still live when the first of them is done, so a per-block sweep would find it in use
     * and leave the whole vector bank standing.
     */
    InstChain dead;

    /*
     * The function is walked twice, and what the second pass is for is the *joins* - a reduction
     * whose mask is a phi, which §52's placement answers out of the bits its alternatives already
     * hold. Those bits are what an arm's own guard computed, and an arm below this one in the block
     * order has not been expanded when the join is reached: taken in one pass, the join would find
     * nothing to reuse in half of its predecessors and would place a movemask that the arm was about
     * to place for itself.
     *
     * Nothing else about the two passes differs, and there is no third: a *chain* of joins - a mask
     * phi whose own alternative is one - settles in whichever order the second pass reaches them,
     * and both orders are right. One that has already been joined is in `shared` and its bits are
     * reused; one that has not is measured where it stands, which is the movemask its own reduction
     * was going to place anyway.
     */
    for(Size pass = 0; pass < 2; pass++)
    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        // Indexed and advanced by hand, like the passes above: the expansion is inserted in front of
        // the reduction and moves everything after it.
        for(Size i = 0; i < block->instructions.size(); ) {
            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::VecReduce) { i++; continue; }

            auto reduce = (LowerInstVecReduce*)inst;
            auto source = base[reduce->from];

            // The movemask is this backend's own instruction and is what every expansion below is
            // written in terms of, so one standing here is finished rather than pending. It is here
            // because the walk placed it: a mask defined in this block by an instruction above this
            // one is where a shared movemask goes, and expanding it again would not terminate.
            if(reduce->getReduce() == LowerReduce::Bits) { i++; continue; }

            // Which pass this reduction belongs to - see above.
            auto join = source->type.isMask() && isPhi(source->inst());
            if(join != (pass == 1)) { i++; continue; }

            /*
             * The bits this reduction reads, and the three ways there are to one.
             *
             * A value already in `shared` is a movemask - ranged or plain - placed below this mask's
             * definition by an earlier consumer, and is the whole answer: what a consumer wants of
             * the bits is the same thing whichever of the two it is reading.
             *
             * Otherwise the live-lane range is asked first, because it decides what the movemask is
             * taken *of* - see LaneRange above. `all` is the one consumer it cannot serve, and it
             * disqualifies the mask rather than itself: the comparison against a full pattern is
             * exactly what the range clears the top of, so a mask any reader asks `all` of keeps the
             * vector form for all of them. `maskBitsReaders` is where both halves of that are
             * counted.
             *
             * The placement may go *into this block above this instruction*, so the position the
             * expansion starts at is re-read afterwards rather than carried across the call.
             *
             * The complement (§45.3) is the last of the three and is asked only where neither of the
             * others applied - a ranged mask is an `and` rather than a comparison, and a mask an
             * earlier consumer has settled is settled including which way round its bits are. It is
             * *recorded* even where no movemask was shared, since the rewrite it makes is to the
             * comparison itself: a later consumer looking at it would find the relation already
             * flipped and read the bits the wrong way round.
             */
            SharedMaskBits* settled = nullptr;
            for(auto& entry: shared) {
                if(entry.source == source) { settled = &entry; break; }
            }

            auto bits = settled ? settled->bits : nullptr;
            auto complemented = settled ? settled->complemented : false;

            LaneRange range;
            auto fused = false; // this reduction applies the range itself, where it stands

            /*
             * A mask a phi joins, answered out of its alternatives' own bits - §52. Taken before the
             * three below and instead of them: a phi is not a comparison, so neither the lane range
             * nor the complement has anything to read, and the movemask this places is a *phi* of
             * placements rather than one of its own.
             */
            if(!settled && join && maskBitsSource(base, inst)) {
                bits = placeJoinedMaskBits(base, fun, source, shared, complemented);

                if(bits) {
                    shared.push(SharedMaskBits { source, bits, complemented });

                    // Once per phi however many reductions read it: the second would be a sweep of
                    // an instruction the first had already taken out of its block.
                    if(!joined.containsValue((LowerInstPhi*)source->inst())) {
                        joined.push((LowerInstPhi*)source->inst());
                    }
                }
            }

            if(!bits && !settled && maskBitsSource(base, inst)) {
                auto rangeable = false;
                auto readers = maskBitsReaders(base, source, rangeable);

                auto ranged = rangeable && (targetFeatures() & kFeatureBmi2)
                    && matchLaneRangeMask(base, source, range);

                if(ranged && readers > 1) {
                    // One movemask and one `bzhi` for every consumer of the masked result, which is
                    // §45.1. A refusal falls through to the plain shared movemask below.
                    bits = placeFusedRangeBits(base, fun, source, range);
                    if(bits) {
                        shared.push(SharedMaskBits { source, bits, false });
                        pushFusedRange(dead, range);
                    }
                } else if(ranged) {
                    // The single reader, which is this one: no position dominates it more usefully
                    // than the one it occupies, and hoisting the movemask to the definition would
                    // lengthen a general-register live range across whatever lies between.
                    fused = true;
                }

                if(!bits && !fused) {
                    complemented = foldComplementedCompare(base, source);
                    if(readers > 1) bits = placeSharedMaskBits(base, fun, source);
                    if(bits || complemented) shared.push(SharedMaskBits { source, bits, complemented });
                }
            }

            auto list = block->instructions.contents(base);
            Size at = i;
            while(at < list.size() && base[list[at]] != inst) at++;
            assertTrue(at < list.size()); // it was at `i`, or one below it if a movemask was placed

            Expansion e { base, fun, block, at };
            auto truth = false;

            /*
             * The movemask of the data mask alone, and the range applied to its bits. The count is
             * asked for last because it is the half that can refuse - `laneRangeIndex` declines a
             * count it cannot place inside the byte the instruction reads - and a refusal here falls
             * back to expanding the `and` as it stands, which is what every consumer did before.
             */
            if(fused) {
                if(auto index = laneRangeIndex(e, range, source->type)) {
                    bits = e.intrinsic2(LowerIntrinsic::Bzhi, LowerType::Int32,
                                        emitMaskBits(e, range.mask), index);

                    pushFusedRange(dead, range);
                } else {
                    // Nothing was emitted for the range, so nothing has to be taken back: the two
                    // producers `laneRangeIndex` may leave standing are its own `min`, and it emits
                    // that only on the path that then answers.
                }
            }

            auto scalar = expandReduce(e, reduce->getReduce(), source, bits, truth, complemented);

            /*
             * A mask's `and` and `or` answer a `Bool`, and what the extract handed back is `-1` or
             * `0` - every bit of a set lane, which is what a mask lane holds. `& 1` is the narrowing
             * to a truth value, and it is exact rather than approximate precisely because the lane
             * has no other two values it could have held.
             *
             * `truth` is the expansion saying it has already answered one: the movemask route ends
             * in a comparison, which is a 0 or a 1 by construction and is worth far more than the
             * narrowing - a branch reads it out of the flags.
             */
            if(source->type.isMask() && !truth && reduce->getReduce() != LowerReduce::Add
               && reduce->getReduce() != LowerReduce::FirstSet) {
                scalar = e.binary(LowerInst::And, scalar->type, scalar, e.integer(scalar->type, 1));
            }

            replaceAllUses(base, &reduce->result, scalar);
            removeInst(base, reduce);

            // And `none`, which is this comparison with a negation on top of it - asked after the
            // uses have moved across, since what it reads is the comparison's users.
            if(truth) foldNegatedTruth(base, scalar);

            // Past the whole expansion. Removing the reduction from the end of it closed the gap the
            // insertions opened, and nothing in what was produced is a reduction.
            i = e.at;
        }
    }

    // Each is removed only once its own use list is empty, which is what keeps a constant two ranges
    // share exactly where it is - and `iota` in a function with two masked tails is exactly that
    // constant.
    removeDeadChain(base, dead);

    // And the vector phis the joins replaced, each of which is empty exactly when every reduction
    // that read it has been expanded - which is now. One that still has a reader stays: a mask a
    // select or a store also reads is a value the join shared rather than removed.
    for(auto phi: joined) {
        auto block = base[phi->block];
        dropUnusedPhi(base, block, phi);
    }
}

/*
 * §42 The mask scan and the branch that guards it, made into one instruction.
 *
 * A search over vectors compiles to two consumers of one movemask in two blocks, which §37 already
 * placed one instruction for:
 *
 *     %bits = pmovmskb %hits          the movemask, placed once
 *     %c    = cmp neq %bits, 0        `any(hits)`
 *     je %c -> hit, miss
 *   hit:
 *     %m    = or %bits, 0x10000       the sentinel, so that `bsf` never sees zero
 *     %f    = bsf %m                  `firstSet(hits)`
 *
 * **Two things are being computed twice here, and the machine computes both of them once.** `bsf`
 * sets ZF exactly when its operand was zero - which is the whole of what the comparison above it
 * asked - and its answer for a *nonzero* operand needs no sentinel, because the sentinel was only
 * ever there to keep the operand from being zero. So the four instructions are two:
 *
 *     %f = bsf %bits ; jne hit
 *
 * Two rewrites, and the second subsumes the first where it applies:
 *
 * - **the sentinel goes** wherever `%bits` is proved nonzero where the scan runs. The proof is the
 *   guard read *locally*, exactly as `isNonNegativeIn` reads one: the scan's block has a single
 *   predecessor, and that predecessor branches here on this mask being nonzero. No dominator tree
 *   and no reasoning about paths - one predecessor is what makes "the branch was taken" true of
 *   every entry to the block.
 * - **the scan moves into the guard's block and the comparison goes**, the branch reading the scan's
 *   own flags. `FormJccLive` is already the form for that - a branch whose condition is a live
 *   register it does not read - so what this needs from the form table is nothing.
 *
 * **The two scans answer the emptiness question in different flags**, which is the one thing here
 * that is not symmetric. `bsf` leaves its destination undefined for a zero operand and says so in
 * ZF; `tzcnt` answers the operand's width and says so in **CF**, ZF meaning something else entirely
 * (that the answer was zero, which is bit zero being set - the opposite of empty). So the condition
 * the branch carries is chosen by which of the two the scan is, and reading ZF off a `tzcnt` would
 * be a search that answered "found" exactly when it had found the mask's *first* lane.
 *
 * Hoisting the scan above the guard is speculation, and the cheapest kind: neither instruction
 * faults, neither touches memory, and the register it writes is dead on the arm that did not want
 * it. What the miss path pays is nothing at all - the `test` it used to run is what the scan
 * replaces.
 */
struct MaskScanGuard {
    LowerInstCmp* compare = nullptr;  // the `any` test, in the guard's block
    LowerInst* scan = nullptr;        // the `bsf`/`tzcnt`, in the guarded block
    LowerInst* sentinel = nullptr;    // the `or` in front of it, where there is one
    LowerValue* bits = nullptr;       // the movemask both of them read
    bool nonzeroIsThen = false;       // whether the guarded block is the branch's `then` arm
};

// The comparison a branch reads, where it is `%bits == 0` or `%bits != 0` and nothing else. The
// constant is asked for by value rather than by `isImm`, which answers "already embedded" and is
// false this early - see the note on it in §37.
static LowerInstCmp* maskEmptinessTest(LowerBase base, LowerInst* terminator) {
    if(terminator->kind != LowerInst::Je) return nullptr;

    auto je = (LowerInstJe*)terminator;
    auto condition = base[je->cond]->inst();
    if(condition->kind != LowerInst::Cmp) return nullptr;

    auto cmp = (LowerInstCmp*)condition;
    if(cmp->getCmp() != LowerCmp::eq && cmp->getCmp() != LowerCmp::neq) return nullptr;
    if(base[cmp->block] != base[terminator->block]) return nullptr;

    /*
     * **The branch has usually already been given this comparison's flags**, and that is not a
     * reason to decline - it is the shape this arrives in.
     *
     * `tryMergeCompare` runs over the *guard's* block in the same walk and reaches it first, blocks
     * being visited in order and a guard standing above what it guards. So by the time this asks,
     * `%c` is implicit and the branch carries its relation. What has to be checked is that the
     * relation is still this comparison's: a branch reading somebody else's flags names a condition
     * that has nothing to do with the value `cond` points at, and rewriting it would be silent.
     */
    auto embedded = je->getEmbeddedCmp();
    if(embedded && embedded.unwrap() != cmp->getCmp()) return nullptr;

    auto rhs = base[cmp->rhs]->inst();
    if(rhs->kind != LowerInst::Imm || ((LowerImm*)rhs)->i != 0) return nullptr;

    return cmp;
}

/*
 * The scan at the top of a guarded block, with the sentinel that may stand in front of it.
 *
 * `bits` names the value it has to read where the caller knows one - a single predecessor's guard
 * tested a value, and a scan of anything else is not this shape - and is null where the caller does
 * not, in which case the scan's own operand is taken and answered back. That second form is what a
 * *join* needs: which value it reads is what says which phi has to be proved nonzero, so the shape
 * is read first and the proof asked for afterwards.
 */
static bool findMaskScan(LowerBase base, LowerBlock* block, LowerValue*& bits, MaskScanGuard& out) {
    auto expected = bits;

    for(auto offset: block->instructions.contents(base)) {
        auto inst = base[offset];

        // A constant is not in the way of anything. The sentinel's own immediate is one of these and
        // is the first instruction of the block in the ordinary case, an `Imm` being placed where it
        // is read; skipping them is what lets this ask about the shape rather than the order.
        if(inst->kind == LowerInst::Imm) continue;

        if(inst->kind == LowerInst::Or) {
            auto binary = (LowerInstBinary*)inst;
            auto rhs = base[binary->rhs]->inst();

            // The sentinel and nothing else: an `or` of *this* mask with a constant, read by one
            // instruction. Anything else in the block is left alone and ends the search, since a
            // scan below it would no longer be the first thing the block does.
            if(rhs->kind != LowerInst::Imm) return false;
            if(expected && base[binary->lhs] != expected) return false;
            if(binary->result.uses.size() != 1) return false;
            if(out.sentinel) return false;

            out.sentinel = inst;
            bits = base[binary->lhs];
            expected = &binary->result;
            continue;
        }

        if(inst->kind != LowerInst::Intrinsic) return false;

        auto which = ((LowerInstIntrinsic*)inst)->getIntrinsic();
        if(which != LowerIntrinsic::Cttz && which != LowerIntrinsic::CttzWidth) return false;

        auto operand = base[inst->used()[0]];
        if(expected && operand != expected) return false;
        if(!out.sentinel) bits = operand;

        out.scan = inst;
        return true;
    }

    return false;
}

// Whether `from` ends in a branch that reaches `block` exactly when `bits` is nonzero, and the
// comparison it reads to do it. Which arm that is depends on the relation: `neq` holds where the
// value is nonzero, so the `then` arm is the nonzero one and `eq` is the other way round.
static LowerInstCmp* branchOnNonzero(LowerBase base, LowerBlock* from, LowerBlock* block,
                                     LowerValue* bits, bool& nonzeroIsThen) {
    auto cmp = maskEmptinessTest(base, base[from->terminator]);
    if(!cmp || base[cmp->lhs] != bits) return nullptr;

    auto je = (LowerInstJe*)base[from->terminator];

    nonzeroIsThen = cmp->getCmp() == LowerCmp::neq;
    if(base[nonzeroIsThen ? je->then : je->otherwise] != block) return nullptr;

    return cmp;
}

/*
 * §52 The same proof through a join: every way in, and not only the one.
 *
 * A search written as a loop and a masked tail branches into one hit block from both, and the bits
 * it scans are the phi of the two arms' bitmaps that `placeJoinedMaskBits` built. What the sentinel
 * is there for is that `bsf` is undefined at zero - and each arm branches here only where its *own*
 * alternative is nonzero, so the phi is nonzero however the block was entered.
 *
 * That is the single-predecessor proof with the quantifier moved and nothing else: it is still read
 * locally, still one branch per edge, and still no dominator tree. What it does not extend to is the
 * fusion below it - the scan cannot stand in two blocks at once, so a join keeps its `test` per arm
 * and loses only the sentinel.
 */
static bool isJoinedNonzero(LowerBase base, LowerBlock* block, LowerValue* bits) {
    auto phi = bits->inst();
    if(!isPhi(phi) || base[phi->block] != block) return false;

    auto sources = ((LowerInstPhi*)phi)->sources();
    if(sources.size() != block->incoming.size()) return false;

    for(Size i = 0; i < sources.size(); i++) {
        auto nonzeroIsThen = false;
        if(!branchOnNonzero(base, base[sources[i]], block, base[phi->used()[i]], nonzeroIsThen)) {
            return false;
        }
    }

    return true;
}

/*
 * The whole shape, recognized from the guarded block.
 *
 * Read from the *guarded* block rather than from the guard, because that is the side the proof is a
 * property of: what makes "the branch was taken" true of every entry to a block is a statement about
 * every edge into it, which the block is where to ask about.
 */
static bool findMaskScanGuard(LowerBase base, LowerBlock* block, MaskScanGuard& out) {
    LowerValue* bits = nullptr;

    if(block->incoming.size() == 1) {
        auto from = base[block->incoming.get(base, 0)];

        // The single predecessor's own test names the value, and `branchOnNonzero` then says whether
        // this block is the arm on which it is the nonzero one.
        auto test = maskEmptinessTest(base, base[from->terminator]);
        if(!test) return false;

        bits = base[test->lhs];

        auto cmp = branchOnNonzero(base, from, block, bits, out.nonzeroIsThen);
        if(!cmp) return false;

        out.compare = cmp;
        out.bits = bits;

        return findMaskScan(base, block, bits, out);
    }

    // A join, where the scan is found first and the phi it reads is what has to be proved - see
    // `isJoinedNonzero`. `compare` stays null, which is what tells the rewrite that there is one
    // guard per edge and no single branch to fold the scan into.
    if(!findMaskScan(base, block, bits, out)) return false;
    if(!isJoinedNonzero(base, block, bits)) return false;

    out.bits = bits;
    return true;
}

void fuseMaskScanIntoGuard(LowerBase base, LowerBlock* block) {
    MaskScanGuard found;
    if(!findMaskScanGuard(base, block, found)) return;

    /*
     * The sentinel, removed. `bsf` is undefined at zero and this is the proof that it never sees
     * one; `tzcnt` has no sentinel to begin with, its answer for zero being the width.
     *
     * Done before the fusion below rather than only inside it, because the two have different
     * conditions: this needs the guard alone, and the fusion needs the comparison to have no other
     * reader and the scan to be liftable. A shape that fails the second still gets the first.
     */
    if(found.sentinel) {
        setOperand(base, base[block->fun]->arena, found.scan, found.scan->used()[0], found.bits);
        removeInst(base, found.sentinel);

        auto constant = base[((LowerInstBinary*)found.sentinel)->rhs];
        if(constant->uses.isEmpty()) removeInst(base, constant->inst());
    }

    /*
     * And the fusion, which is the half a *join* does not get: there is one guard per edge and the
     * scan stands in one block, so what a second predecessor would have to read is a scan its own
     * branch does not precede. The sentinel above is the whole of what a join takes.
     */
    if(!found.compare) return;

    /*
     * §57 And it is refused where the scan is `bsf`, which is every tier below AVX2.
     *
     * The two forms of this scan are not two encodings of one instruction. `tzcnt` is a plain
     * three-cycle operation whose flags say what its operand was; `bsf` is the older one, it cannot
     * pair with the branch that reads it the way a `test` does, and putting it between the movemask
     * and the branch lengthens the path every iteration of a search waits on. Both were measured on
     * `VecString`'s inner loop with everything else - the index arithmetic, the register assignment,
     * the loop's alignment - held identical, over a 64 KiB buffer:
     *
     *   findAscii, SSE     ours 1.75 ms   scan on the exit 1.35 ms   (-23%)
     *   findAscii, AVX2    ours 0.85 ms   scan on the exit 0.79 ms   (-7%)
     *
     * `countAscii`, whose consumer is `popcnt` and which therefore has no scan in its loop at all,
     * is a tie with `llc` at both tiers - which is what says this is the scan and not the loop
     * around it. §48.2 measured the AVX2 half against llc's whole shape and found the fusion ahead,
     * and that reading stands; what it did not separate is this one instruction.
     *
     * So the fusion keeps the tier it was measured on and loses the one it was not. What is left
     * behind here is the `test` the fusion would have removed - two bytes, against a scan in the
     * dependency path of a branch that a search executes once per vector. See §57 of
     * test/bench/findings.md.
     */
    if(((LowerInstIntrinsic*)found.scan)->getIntrinsic() != LowerIntrinsic::CttzWidth) return;

    /*
     * Three conditions, each of which is a way the branch could stop being the only thing that reads
     * the comparison's answer or the scan could stop being liftable:
     *
     * - the comparison is read by the branch and by nothing else, since it is about to disappear;
     * - the guard's block ends with the branch that reads it, which is what puts the scan's new
     *   position directly in front of the instruction reading its flags - so there is no window to
     *   check, there being nothing between them;
     * - the mask is a register rather than a folded address, so that hoisting the scan above the
     *   guard speculates an instruction and not a load.
     */
    auto guard = base[block->incoming.get(base, 0)];
    auto je = (LowerInstJe*)base[guard->terminator];

    if(found.compare->result.uses.size() != 1) return;
    if(base[found.compare->result.uses.get(base, 0)] != je) return;
    if(base[found.scan->used()[0]]->inst()->kind == LowerInst::X86Address) return;

    // And the scan's own operand, which has to be readable from where the scan is going. Everything
    // else that may stand above it in this block is a constant, which `findMaskScan` skipped and
    // which the scan does not read - so this is the whole of what the move has to be told.
    if(base[base[found.scan->used()[0]]->inst()->block] == block) return;

    moveInstToEndOf(base, found.scan, guard);

    /*
     * Which flag says the mask was empty, and therefore which condition the branch carries.
     *
     * `bsf` sets ZF for a zero operand, so "nonzero" is `neq`. `tzcnt` sets **CF** for one - ZF on a
     * `tzcnt` means the answer was zero, which is the mask's first lane being set - so "nonzero" is
     * CF clear, which is the unsigned `ge` the encoder writes as `jae`.
     */
    auto width = ((LowerInstIntrinsic*)found.scan)->getIntrinsic() == LowerIntrinsic::CttzWidth;
    auto nonzero = width ? LowerCmp::ge : LowerCmp::neq;
    auto empty = width ? LowerCmp::lt : LowerCmp::eq;

    auto scanned = &found.scan->created()[0];
    replaceUse(base, &found.compare->result, je, scanned);
    je->cond = scanned - base;
    je->setEmbeddedCmp(Just(found.nonzeroIsThen ? nonzero : empty));

    removeInst(base, found.compare);

    auto constant = base[found.compare->rhs];
    if(constant->uses.isEmpty()) removeInst(base, constant->inst());
}
