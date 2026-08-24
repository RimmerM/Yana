#include "machine_internal.h"

// For `nameForInst`, which is what a refusal names the instruction by - see checkVectorSupported.
#include "../../lower/lower_print.h"

/*
 * Which form of a packed operation a lane type takes.
 *
 * A row per operation and a column per lane, with zero where the machine has no instruction - and a
 * zero reached is a hard failure rather than a fallback, because the neighbouring column is a
 * different width and would be silently wrong. `laneColumn` is the index, and it is the lane's own
 * order in LowerLane so that adding a lane type is a column rather than a renumbering.
 */
static Size laneColumn(LowerType type) {
    switch(type.lane) {
        case LowerLane::Int8:    return 0;
        case LowerLane::Int16:   return 1;
        case LowerLane::Int32:   return 2;
        case LowerLane::Int64:   return 3;
        case LowerLane::Float32: return 4;
        case LowerLane::Float64: return 5;
        case LowerLane::Pointer: break;
    }

    assertTrue("a vector of pointers has no lane column" == nullptr);
    return 0;
}

/*
 * Whether this backend has any packed form for a vector at all.
 *
 * A whole register, at one of the two widths this backend can hold one at. Sixteen bytes always;
 * thirty-two where the target has AVX2, which is the level at which `targetVectorBytes` starts
 * handing out a natural width of 32 - so the two answers move together and a wide value is never
 * built for a target with no form to spend it on.
 *
 * What stays refused is a vector narrower than a register, and it is the more surprising half:
 * `i32x2` is eight bytes and occupies an xmm quite happily, but the transfers this backend has are
 * `movups` and `movdqu`, which read and write the whole register whatever the type says. A *store*
 * of one would write eight bytes past its object. Closing it is `movq` and `movd` forms, which is
 * the same work the lane extract needs.
 */
bool isWholePackedRegister(LowerType type) {
    auto bytes = type.byteWidth();
    if(bytes == 16) return classForType(type) == ClassXmm128;
    if(bytes == 32) return classForType(type) == ClassYmm256 && (targetFeatures() & kFeatureAvx2) != 0;
    return false;
}

/*
 * The form of a packed operation at the register width its type occupies.
 *
 * `narrow` is always the 128-bit answer - every row and every direct selection below names one - and
 * this is the single place the wide tier is reached from. That is deliberate: a 32-byte value that
 * fell back to a 128-bit form would read and write half a vector and answer a plausible wrong
 * number, which is §5.6's failure exactly. So there is no fallback here at all - a missing twin is
 * an assertion, and `unsupportedVectorReason` is what turns the same gap into a diagnostic before
 * anything reaches this.
 */
MachineFormId widthForm(MachineFormId narrow, LowerType type) {
    if(!isWideVector(type)) return narrow;

    auto wide = machineTarget().form(narrow).wide;
    assertTrue(wide != 0); // no 256-bit form of an operation unsupportedVectorReason let through
    return wide;
}

/*
 * The `pshufd` control byte a shuffle's pattern becomes - see the declaration in machine.h.
 *
 * `pshufd` addresses four 32-bit lanes and its byte is two bits per *result* lane naming the source
 * lane it takes, which is the IR's pattern exactly at a 32-bit lane width. A 64-bit lane is the one
 * translation: the machine has no quadword shuffle before AVX-512, so a two-lane pattern is spent as
 * a four-lane one that moves each half as its pair of 32-bit lanes, and `[1, 0]` becomes 0x4e.
 *
 * A mask is shuffled by its lane *width* like any other vector: what a mask lane holds is a truth
 * value, but where it holds it is a lane of that width, and moving one is moving its bits.
 */
// The `pshufd` byte for a pattern of `lanes` entries that all name one source - the two-bit index
// per result lane at a 32-bit width, and the pair of 32-bit lanes each half is made of at a 64-bit
// one, since there is no quadword shuffle before AVX-512 and `[1, 0]` is therefore 0x4e.
static U8 oneSourceShuffleByte(LowerType type, Buffer<U8> pattern, U8 bias) {
    auto control = U8(0);

    if(laneBytes(type.lane) == 4) {
        for(Size i = 0; i < 4; i++) control |= U8((pattern[i] - bias) << (i * 2));
        return control;
    }

    for(Size i = 0; i < 2; i++) {
        auto half = U8((pattern[i] - bias) * 2);
        control |= U8(half << (i * 4));
        control |= U8((half + 1) << (i * 4 + 2));
    }

    return control;
}

/*
 * Whether a pattern interleaves the two sources, taking a lane of each in turn.
 *
 * `low` walks the bottom half of both and `high` the top: for four lanes they are `[0, 4, 1, 5]` and
 * `[2, 6, 3, 7]`. It is the one two-source shape `shufps` cannot state - that instruction takes a
 * *run* from each side - and it is what a lane-count conversion is built out of, so it has an
 * instruction at every lane width including the two that have no other shuffle at all.
 */
static bool isInterleave(Buffer<U8> pattern, U32 lanes, bool high) {
    auto base = U8(high ? lanes / 2 : 0);

    for(U32 k = 0; k < lanes / 2; k++) {
        if(pattern[k * 2] != base + k) return false;
        if(pattern[k * 2 + 1] != lanes + base + k) return false;
    }

    return true;
}

// The 128-bit families, asked about `lanes` result lanes selected from two sources of `lanes` lanes
// each. `type` supplies the lane width and kind and its own lane *count* is deliberately not read,
// because the wide tier asks this about one 128-bit half of a wider pattern - see below.
static Maybe<PackedShuffleChoice> narrowShuffleChoice(LowerType type, Buffer<U8> pattern, U32 lanes);

/*
 * A pattern at 256 bits, which is a different question from the same pattern at 128.
 *
 * **Every shuffle AVX2 has works inside each 128-bit half.** `vpshufd ymm` applies one control byte
 * to both halves independently, `vshufps ymm` takes its run from the corresponding half of each
 * source, and the interleaves interleave within a half. So the eight-lane pattern `[0, 8, 1, 9, 2,
 * 10, 3, 11]` - which reads as an interleave and *is* one at four lanes - is not an instruction here
 * at all, and a tier that answered `vpunpckldq` for it would have produced a plausible wrong vector.
 *
 * That gives two questions rather than one, asked in this order:
 *
 * - **Is it in-lane?** Every result lane takes a source lane from its own half. If so, the pattern
 *   reduces to a 128-bit one - the same one in both halves, since there is one control byte - and
 *   the answer is the narrow family's form widened. This is where the ordinary shuffles live.
 * - **Is it a permutation of whole halves?** `vperm2f128` builds its result out of two of the four
 *   halves its two sources hold. This is the *only* cross-half rearrangement in the tier, and it is
 *   what the top level of a reduction butterfly over eight lanes is - lane `j` paired with lane
 *   `j ^ 4` is exactly the two halves exchanged.
 *
 * In that order because a pattern that is both - the identity, or a swap of the halves of one source
 * that is also an in-lane shuffle - is cheaper as the in-lane one: `vperm2f128` crosses the halves
 * and is the slower of the two on every part that has both.
 *
 * Anything else is refused, and the refusal is real rather than a gap waiting to be filled. AVX2's
 * `vpermd` would express a general 32-bit lane permutation, but it takes its pattern out of a
 * *vector* register - which needs the constant pool this backend has not opened to vectors.
 */
static Maybe<PackedShuffleChoice> wideShuffleChoice(LowerType type, Buffer<U8> pattern, U32 lanes) {
    auto half = lanes / 2;
    assertTrue(half > 0); // a 32-byte vector of one lane, which no lane width this backend has produces

    // The source half a lane index names, numbering them as `vperm2f128` does: the first source's
    // low and high halves are 0 and 1, the second source's are 2 and 3.
    auto halfOf = [&](U8 lane) { return U8((lane / half) & 3); };

    /*
     * In-lane, and the reduced pattern it comes to.
     *
     * A result lane in half `k` may only name a source lane in half `k`, of either source. The
     * reduction renumbers what is left as a 128-bit pattern over two sources of `half` lanes - so
     * the first source's half becomes 0..half-1 and the second's becomes half..2*half-1 - and both
     * halves have to reduce to the *same* pattern, there being one control byte for the pair.
     */
    auto inLane = true;
    U8 reduced[kMaxVectorLanes / 2] = {};

    for(U32 k = 0; k < 2 && inLane; k++) {
        for(U32 j = 0; j < half; j++) {
            auto source = pattern[k * half + j];
            if((halfOf(source) & 1) != k) { inLane = false; break; }

            // Which side, and where inside that side's half. `source >= lanes` is the second source
            // by the numbering LowerInstVecShuffle states.
            auto within = U8(source % half);
            auto entry = U8(source >= lanes ? half + within : within);

            if(k == 0) reduced[j] = entry;
            else if(reduced[j] != entry) { inLane = false; break; }
        }
    }

    if(inLane) {
        auto choice = narrowShuffleChoice(type, Buffer<U8>(reduced, half), half);
        if(choice) {
            auto answer = choice.unwrap();
            answer.form = widthForm(answer.form, type);
            return Just(answer);
        }
    }

    /*
     * Whole halves, which is `vperm2f128`: each half of the result is one of the four the sources
     * hold, taken entire and in order. Its byte names the low half's source in bits 1:0 and the high
     * half's in bits 5:4 - the same numbering `halfOf` answers in, which is why that helper is
     * written in the instruction's terms rather than the IR's.
     */
    auto control = U8(0);

    for(U32 k = 0; k < 2; k++) {
        auto source = halfOf(pattern[k * half]);
        auto base = U8(source & 1 ? half : 0);
        if(pattern[k * half] >= lanes) base = U8(base + lanes);

        for(U32 j = 0; j < half; j++) {
            if(pattern[k * half + j] != base + j) return {};
        }

        control |= U8(source << (k * 4));
    }

    return Just(PackedShuffleChoice { FormVPerm2, control, true });
}

/*
 * Whether every entry of a shuffle's pattern names the same source.
 *
 * The one question `vpermd` adds to this file, and it is asked on both sides of the pass that spends
 * the answer - `checkVectorSupported` above and `lowerWideLanePermutes` below - which is
 * `packedCompareRelation`'s argument for why it lives here rather than in either of them.
 */
bool shuffleReadsOneSource(LowerInst* inst) {
    auto shuffle = (LowerInstVecShuffle*)inst;
    auto lanes = shuffle->result.type.lanes();
    auto pattern = shuffle->pattern();
    auto second = pattern[0] >= lanes;

    for(U32 k = 1; k < lanes; k++) {
        if((pattern[k] >= lanes) != second) return false;
    }

    return true;
}

Maybe<PackedShuffleChoice> packedShuffleChoice(LowerInst* inst) {
    if(inst->kind != LowerInst::VecShuffle) return {};

    auto shuffle = (LowerInstVecShuffle*)inst;
    auto type = shuffle->result.type;
    if(!isWholePackedRegister(type)) return {};

    auto lanes = type.lanes();
    auto pattern = shuffle->pattern();

    /*
     * A shuffle whose result holds a different number of lanes than its sources - which the lower IR
     * permits, `packLanes` being one - names its *sources'* lanes and produces a register of another
     * width. This function reads the pattern against the result's count, so such a shuffle would be
     * misread rather than refused; the entries out of range are what give it away without a second
     * type to compare against, since the validator has already checked them against the source's.
     *
     * That catches the narrowing direction, which is the one `packLanes` takes. The widening one is
     * caught where the type is available on both sides - see unsupportedVectorReason.
     */
    for(auto entry: pattern) {
        if(entry >= lanes * 2) return {};
    }

    if(isWideVector(type)) return wideShuffleChoice(type, pattern, lanes);
    return narrowShuffleChoice(type, pattern, lanes);
}

static Maybe<PackedShuffleChoice> narrowShuffleChoice(LowerType type, Buffer<U8> pattern, U32 lanes) {
    auto width = laneBytes(type.lane);

    // Which sides the pattern actually reads, which is what chooses the family: one source is a
    // `pshufd` of whichever operand it names, and two are one of the shapes below.
    auto readsFirst = false;
    auto readsSecond = false;

    for(Size i = 0; i < lanes; i++) {
        (pattern[i] < lanes ? readsFirst : readsSecond) = true;
    }

    /*
     * Interleaving first, because it is the only family with an instruction at every lane width -
     * asking the width first would refuse an `i8x16` interleave for want of a `pshufb` it does not
     * need. Both halves are checked even where only one source is read: `[0, 0, 1, 1]` is not an
     * interleave of one vector with itself in this numbering, so the test costs nothing false.
     */
    {
        static const MachineFormId kLow[6] = {
            FormVUnpackLow8, FormVUnpackLow16, FormVUnpackLow32, FormVUnpackLow64,
            FormVUnpackLowF32, FormVUnpackLowF64,
        };
        static const MachineFormId kHigh[6] = {
            FormVUnpackHigh8, FormVUnpackHigh16, FormVUnpackHigh32, FormVUnpackHigh64,
            FormVUnpackHighF32, FormVUnpackHighF64,
        };

        if(isInterleave(pattern, lanes, false)) return Just(PackedShuffleChoice { kLow[laneColumn(type)] });
        if(isInterleave(pattern, lanes, true)) return Just(PackedShuffleChoice { kHigh[laneColumn(type)] });
    }

    /*
     * A window out of the two sources placed end to end - `palignr`, and `Core.alignLanes`.
     *
     * Recognized here rather than below the lane-width test because the instruction shifts *bytes*:
     * it serves an `i8x16` and an `i64x2` alike, and the lane width appears only in the immediate.
     * That is also why it sits after the interleaves and before `shufps` - an interleave is not a
     * window, and a window is one of the two-source patterns `shufps` cannot express.
     *
     * The pattern this matches is `k, k+1, ...` read against the concatenation, which in this
     * numbering is: the second operand supplies the low half, so entry `i` is `lanes + k + i` while
     * that is below `2 * lanes` and `k + i - lanes` after it. `k = 0` never reaches here - it names
     * one source and the branch above has already answered - so the immediate is never zero.
     */
    if(readsFirst && readsSecond) {
        U32 start = pattern[0] >= lanes ? pattern[0] - lanes : pattern[0] + lanes;

        /*
         * The window has to *fit*, which is a bound on `start + lanes` and not on `start`.
         *
         * `start` is a lane of the concatenation and the window is `lanes` long, so the last lane it
         * names is `start + lanes - 1` and the whole of it fits exactly when `start <= lanes`. That
         * is also the range `alignLanes` admits - it refuses a `from` past the count for the same
         * reason - and `start * width` is then at most sixteen bytes, which is what `palignr`'s
         * immediate can mean.
         *
         * ~~`start < lanes * 2`~~ let a *wrapping* pattern through: the loop below reduces `k` modulo
         * the concatenation, so `[1, 2, 3, 4]` at four lanes was read as the window at `start = 5`
         * and emitted `palignr $20` - an immediate past the pair, which shifts zeros in. No named
         * pattern can produce one (`reverse`, `rotate` and the interleaves are not windows, and
         * `alignLanes` bounds its own start), so nothing reached it until `Core.shuffle2` let a
         * pattern be written out.
         */
        auto window = start <= lanes;

        for(U32 i = 0; i < lanes && window; i++) {
            auto k = start + i;
            window = pattern[i] == U8(k < lanes ? k + lanes : k - lanes);
        }

        if(window) return Just(PackedShuffleChoice { FormVAlign, U8(start * width), true });
    }

    // An 8- or 16-bit lane has nothing else. `pshuflw` and `pshufhw` reach half a register each and
    // `pshufb` is SSSE3 and takes its pattern from a *vector*, which needs the constant pool this
    // tier has not opened to vectors yet.
    if(width != 4 && width != 8) return {};

    // One source, in whichever operand holds it. A pattern naming only the second is a `pshufd` of
    // the other operand and nothing else, so it is a form rather than a refusal - which is what the
    // second of the two entries below is for.
    if(!readsSecond) {
        return Just(PackedShuffleChoice {
            FormVShuffle32, oneSourceShuffleByte(type, pattern, 0), true,
        });
    }

    if(!readsFirst) {
        return Just(PackedShuffleChoice {
            FormVShuffle32Second, oneSourceShuffleByte(type, pattern, U8(lanes)), true,
        });
    }

    /*
     * Both sources, which is `shufps` or `shufpd`: the low half of the result comes from the first
     * operand and the high half from the second, each lane named by its own field of the control
     * byte. A pattern whose halves are the other way round, or that crosses in the middle, is what
     * is left over - and it is what the interleaves above have already taken most of.
     */
    auto half = lanes / 2;

    for(U32 i = 0; i < half; i++) {
        if(pattern[i] >= lanes) return {};
    }

    for(U32 i = half; i < lanes; i++) {
        if(pattern[i] < lanes) return {};
    }

    auto control = U8(0);
    auto bits = width == 4 ? 2 : 1;

    for(U32 i = 0; i < lanes; i++) {
        auto index = U8(i < half ? pattern[i] : pattern[i] - lanes);
        control |= U8(index << (i * bits));
    }

    return Just(PackedShuffleChoice {
        width == 4 ? FormVShuffle2F32 : FormVShuffle2F64, control, true,
    });
}

U8 broadcastLaneByte(LowerType type, U8 index) {
    // A 32-bit lane is the index in all four positions; a 64-bit one is the pair of 32-bit lanes it
    // is made of, repeated - the same translation packedShufflePattern makes for a written pattern,
    // and for the same reason: there is no quadword shuffle before AVX-512.
    if(laneBytes(type.lane) == 4) return U8(index * 0x55);

    assertTrue(laneBytes(type.lane) == 8); // no shuffle addresses a lane of any other width here
    auto half = U8(index * 2);
    return U8(half | ((half + 1) << 2) | (half << 4) | ((half + 1) << 6));
}

U8 packedTrailingByte(LowerInst* inst) {
    switch(inst->kind) {
        /*
         * The rounding mode `roundsd` and its three siblings take - bits 1:0 name the direction and
         * bit 3 suppresses the precision exception.
         *
         * The suppression bit is set on all three, and it is not decoration: these are exact
         * operations by definition, so an inexact flag raised by one is a flag no program asked for
         * and one that a later `fetestexcept` would read as its own arithmetic's. It is what LLVM
         * emits for `llvm.trunc`/`llvm.floor`/`llvm.ceil` and what the mode field is for.
         *
         * `LowerInst::Round` has no row: ties-away is not one of the four directions this byte can
         * name, and `expandRoundAway` is what stands in for the encoding that does not exist.
         */
        case LowerInst::Trunc: return 0x0b;
        case LowerInst::Floor: return 0x09;
        case LowerInst::Ceil:  return 0x0a;

        case LowerInst::VecShuffle: {
            // `unwrap` rather than a check: the form was selected *because* this answered, so one
            // that does not now is a pattern something rewrote behind the selection. And a form
            // that carries no byte never reaches here, since only `patternImmediate` asks.
            auto choice = packedShuffleChoice(inst).unwrap();
            assertTrue(choice.hasByte); // a shuffle form with a trailing byte its pattern has none for
            return choice.byte;
        }

        /*
         * A lane read out. A float lane leaves the value in a vector register and gets there by a
         * shuffle, so its byte is a control byte; an integer one is `pextr`, whose byte is the index.
         *
         * Asked of this instruction's own *result* type, which needs no region to read: an extract's
         * result is one lane, so its width and its kind are the lane's.
         */
        case LowerInst::VecLane: {
            auto lane = (LowerInstVecLane*)inst;
            auto type = lane->result.type;

            if(isFloat(type.laneType())) return broadcastLaneByte(type, lane->getLane());
            return lane->getLane();
        }

        /*
         * A lane written in, whose byte is a third thing again - and the only form here that carries
         * one at a float lane is `insertps`, whose byte is three fields rather than an index:
         * bits 7:6 name the source's lane, 5:4 the destination's, and 3:0 which lanes to zero. The
         * source is a scalar, so its lane is zero and nothing is zeroed; what is left is the
         * destination lane, shifted into place.
         *
         * The baseline float forms carry no trailing byte at all (`movsd` and `unpcklpd` *are* the
         * lane they write), so they never reach here - a form asks for this only where it declared
         * `patternImmediate`.
         */
        case LowerInst::VecWithLane: {
            auto lane = (LowerInstVecLane*)inst;
            auto type = lane->result.type;

            if(isFloat(type.laneType())) return U8(lane->getLane() << 4);
            return lane->getLane();
        }

        default:
            assertTrue("this instruction has no trailing byte to write" == nullptr);
            return 0;
    }
}

LowerCmp packedCompareRelation(LowerCmp cmp) {
    switch(cmp) {
        case LowerCmp::gt:  return LowerCmp::lt;
        case LowerCmp::ge:  return LowerCmp::le;
        case LowerCmp::ilt: return LowerCmp::igt;

        // `a >= b` is `b <= a`, and `ile` is the one of the pair that has an expansion - `pcmpgt`
        // with the mask inverted. Exchanging here rather than inverting twice is what keeps that
        // expansion to a single shape.
        case LowerCmp::ige: return LowerCmp::ile;
        default:            return cmp;
    }
}

/*
 * Whether a packed integer comparison is the machine's relation complemented.
 *
 * The machine has `pcmpeq` and `pcmpgt` and nothing else, so three of the six relations a signed
 * lane can be compared with are the complement of one it does have: `neq` of `eq`, and `ile` of
 * `igt` - `ige` having already become `ile` by an exchange above. Complementing a mask is an
 * exclusive-or against all ones, and all ones is a register compared with itself, so each of the
 * three is the base instruction plus two.
 *
 * Float comparisons are never this. `cmpps` carries all eight relations in its predicate byte, so
 * the complement is a different byte and not a different instruction.
 */
bool packedCompareIsInverted(LowerType type, LowerCmp cmp) {
    if(isFloatVector(type)) return false;
    return cmp == LowerCmp::neq || cmp == LowerCmp::ile;
}

static MachineFormId packedForm(const MachineFormId (&row)[6], LowerType type) {
    assertTrue(isWholePackedRegister(type)); // no forms for a vector of any other width

    auto form = row[laneColumn(type)];
    assertTrue(form != 0); // no packed instruction for this operation at this lane width
    return widthForm(form, type);
}

/*
 * The form a packed operation of this kind and lane type takes, or zero where this instruction is
 * not a packed one at all.
 *
 * One place rather than an arm per kind, because the answer is a table lookup for every one of them
 * and because what is *missing* from the table is then in one place too: a zero row is a lane width
 * with no instruction, and reaching one is a hard failure by `packedForm` rather than a fallback to
 * a neighbouring width.
 */
/*
 * Which of the two nothing-constants a splat is, if it is one - §5.7.
 *
 * Every lane of a splat holds the same scalar, so the pattern is decided by that scalar alone: zero
 * is zero at every width, and all-ones is the lane's width of set bits. A float lane is asked about
 * its *bits* rather than its value, because what `pxor` and `pcmpeqd` produce is a bit pattern and
 * `-0.0` is not a zero in that sense - which is the same distinction `poolFloatConstants` draws when
 * it keeps positive zero an immediate and does not keep the negative one.
 */
enum class SplatPattern: U8 { Zero, Ones };

static Maybe<SplatPattern> splatConstantPattern(LowerBase base, LowerInst* inst) {
    auto splat = (LowerInstVecSplat*)inst;
    auto source = base[splat->from];
    if(source->inst()->kind != LowerInst::Imm) return Nothing();

    auto lane = splat->result.type.lane;
    auto imm = (LowerImm*)source->inst();
    U64 bits = 0;

    if(lane == LowerLane::Float32) {
        auto narrow = float(imm->f);
        U32 word = 0;
        copyMem(&narrow, &word, 4);
        bits = word;
    } else if(lane == LowerLane::Float64) {
        auto wide = imm->f;
        copyMem(&wide, &bits, 8);
    } else {
        auto width = laneBytes(lane);
        bits = width >= 8 ? imm->i : (imm->i & ((U64(1) << (width * 8)) - 1));
    }

    if(!bits) return Just(SplatPattern::Zero);

    auto width = laneBytes(lane);
    auto ones = width >= 8 ? ~U64(0) : ((U64(1) << (width * 8)) - 1);
    if(bits == ones) return Just(SplatPattern::Ones);

    return Nothing();
}

bool splatIsMachineConstant(LowerBase base, LowerInst* inst) {
    return inst->kind == LowerInst::VecSplat && splatConstantPattern(base, inst);
}

bool packedMinMaxSupported(LowerType type) {
    if(!isVectorLike(type) || !isWholePackedRegister(type)) return false;
    if(isFloatVector(type)) return true;

    // Every integer width but the quadword, which has no `pminsq` outside AVX-512 - the same gap the
    // form table's rows leave empty at that column.
    return isIntVector(type) && laneBytes(type.lane) < 8;
}

/*
 * The scalar every lane of a packed shift shares its count with, or nothing where the count is not
 * shared at all.
 *
 * Two spellings arrive here and mean one thing, exactly as `packedShiftConstantCount` describes: a
 * `.lower` fixture writes a scalar count and the language writes `vsplat(n)`, `Integral(a)` typing
 * both operands as `a`. Either is a count the machine's register form can take, since what that form
 * reads is one quadword and not one count per lane.
 *
 * What answers nothing is a count vector that is *not* a splat - a genuinely per-lane count, which
 * is AVX2's `vpsllv` family and no form here. Nothing in the language produces one today; this is
 * what keeps a hand-written lower IR from selecting a shared-count form for it.
 */
LowerValue* packedShiftSharedCount(LowerBase base, LowerInst* inst) {
    auto count = base[((LowerInstBinary*)inst)->rhs];
    if(!isVectorLike(count->type)) return count;

    auto splat = count->inst();
    if(splat->kind != LowerInst::VecSplat) return nullptr;

    return base[((LowerInstVecSplat*)splat)->from];
}

LowerImm* packedShiftConstantCount(LowerBase base, LowerInst* inst) {
    auto count = base[((LowerInstBinary*)inst)->rhs]->inst();

    // A splat of a constant is the language's spelling and a bare constant is the fixtures'. One
    // level of unwrapping and no more: a splat of a runtime value is the other machine form's
    // business, and a splat of a splat is not a thing this IR produces.
    if(count->kind == LowerInst::VecSplat) count = base[((LowerInstVecSplat*)count)->from]->inst();

    return count->kind == LowerInst::Imm ? (LowerImm*)count : nullptr;
}

MachineFormId selectPackedForm(LowerBase base, LowerInst* inst) {
    // Indexed by laneColumn: i8, i16, i32, i64, f32, f64.
    static const MachineFormId kAdd[6] = { FormVAdd8, FormVAdd16, FormVAdd32, FormVAdd64, FormVAddF32, FormVAddF64 };
    static const MachineFormId kSub[6] = { FormVSub8, FormVSub16, FormVSub32, FormVSub64, FormVSubF32, FormVSubF64 };
    // The 32-bit column is filled in below rather than here, because which of its two forms applies
    // is a feature question and a row is not.
    static const MachineFormId kMul[6] = { 0, FormVMul16, 0, 0, FormVMulF32, FormVMulF64 };
    static const MachineFormId kDiv[6] = { 0, 0, 0, 0, FormVDivF32, FormVDivF64 };
    // The high half, at the one lane width the machine has it: a row each, since this is where the
    // two signednesses stop agreeing about the bits.
    static const MachineFormId kMulHi[6]  = { 0, FormVMulHi16,  0, 0, 0, 0 };
    static const MachineFormId kIMulHi[6] = { 0, FormVIMulHi16, 0, 0, 0, 0 };

    static const MachineFormId kShlImm[6] = { 0, FormVShl16Imm, FormVShl32Imm, FormVShl64Imm, 0, 0 };
    static const MachineFormId kShrImm[6] = { 0, FormVShr16Imm, FormVShr32Imm, FormVShr64Imm, 0, 0 };
    static const MachineFormId kSarImm[6] = { 0, FormVSar16Imm, FormVSar32Imm, 0, 0, 0 };

    // The register-count rows have no lane column: one form per direction, the width being an opcode
    // byte the expansion reads rather than anything the allocator does. The widths the machine has
    // no shift at are refused by `unsupportedVectorReason` before this runs, as they are for the
    // immediate rows above - the difference is that here there is no zero in a table to fall into.
    static const MachineFormId kShiftReg[3] = { FormVShlReg, FormVShrReg, FormVSarReg };

    // A count per lane, which is AVX2's family and has one column per direction rather than a lane
    // table: the two widths it exists at are `VEX.W` apart. The arithmetic direction's quadword is
    // zero because there is no `vpsravq` before AVX-512 - `expandQuadwordSar` builds it out of
    // `vpsrlvq` and a bias, so nothing ever reads that entry.
    static const MachineFormId kShiftVar[3][2] = {
        { FormVShlVar32, FormVShlVar64 },
        { FormVShrVar32, FormVShrVar64 },
        { FormVSarVar32, 0 },
    };

    static const MachineFormId kCmpEq[6] = { FormVCmpEq8, FormVCmpEq16, FormVCmpEq32, FormVCmpEq64, FormVCmpF32, FormVCmpF64 };
    static const MachineFormId kCmpGt[6] = { FormVCmpGt8, FormVCmpGt16, FormVCmpGt32, FormVCmpGt64, FormVCmpF32, FormVCmpF64 };

    switch(inst->kind) {
        /*
         * `IMul` stands beside `Mul` and reaches the same row, which is not an approximation: the
         * low half of a product is the same bits whichever way the operands are read, and `pmullw`
         * is what both spellings of a 16-bit lane multiply take. It has to be here rather than only
         * in the unsigned kind because `signedOperand` answers a vector's lane's signedness, so an
         * ordinary `Vec(Int)` product is an `IMul` and would otherwise reach the scalar group-3
         * multiply and assert on its own type.
         *
         * The signed division kinds are deliberately *not* here. A quotient is not sign-agnostic,
         * there is no packed integer divide to reach anyway, and `unsupportedVectorReason` refuses
         * every one of them before this runs.
         */
        case LowerInst::Add:
        case LowerInst::Sub:
        case LowerInst::Mul:
        case LowerInst::IMul:
        case LowerInst::MulHi:
        case LowerInst::IMulHi:
        case LowerInst::Div: {
            auto type = ((LowerInstBinary*)inst)->result.type;
            if(!isVectorLike(type)) return 0;

            switch(inst->kind) {
                case LowerInst::Add: return packedForm(kAdd, type);
                case LowerInst::Sub: return packedForm(kSub, type);
                // The high half keeps its two kinds apart where the low half merges them, which is
                // the whole of what these two lines say that the two above them do not.
                case LowerInst::MulHi:  return packedForm(kMulHi, type);
                case LowerInst::IMulHi: return packedForm(kIMulHi, type);
                case LowerInst::Mul:
                case LowerInst::IMul:
                    // A 32-bit lane's product is `pmulld`, which is SSE4.1 and so is the floor.
                    // Which signedness it was written with does not enter into it: the low half of a
                    // product is the same bits either way, which is why `IMul` reaches this row.
                    if(laneColumn(type) == 2) return widthForm(FormVMul32, type);

                    return packedForm(kMul, type);
                default:              return packedForm(kDiv, type);
            }
        }

        // The bitwise three have no lane width at all: one instruction serves every vector and every
        // mask, which is why these are the only packed rows with no table.
        case LowerInst::And:
        case LowerInst::Or:
        case LowerInst::Xor: {
            auto type = ((LowerInstBinary*)inst)->result.type;
            if(!isVectorLike(type)) return 0;

            assertTrue(isWholePackedRegister(type)); // no forms for a vector of any other width

            // The one exception to "no lane width": an `and` over a *float* vector takes `andps`
            // rather than `pand`. Same bits, same length, and the result is read back in the domain
            // it was produced in - which is the whole of the difference and the reason the row
            // exists at all. A mask answers false here and keeps `pand`, a mask lane being a truth
            // value rather than a float.
            if(inst->kind == LowerInst::And && isFloatVector(type)) {
                return widthForm(laneBytes(type.lane) == 4 ? FormVAndF32 : FormVAndF64, type);
            }

            return widthForm(inst->kind == LowerInst::And ? FormVAnd : inst->kind == LowerInst::Or ? FormVOr : FormVXor, type);
        }

        case LowerInst::Shl:
        case LowerInst::Shr:
        case LowerInst::Sar: {
            auto binary = (LowerInstBinary*)inst;
            auto type = binary->result.type;
            if(!isVectorLike(type)) return 0;

            /*
             * One count every lane shares, which is either a constant - the immediate rows - or a
             * value in a general register that the expansion moves across the banks.
             *
             * Asked of the *value* rather than of `hasEmbeddedRhs`, because this runs before the
             * peephole that embeds a constant as well as after it: the load folding selects a form
             * for every instruction it walks past (tryFoldLoad), and at that point no immediate has
             * been embedded yet. Every other opcode has a form for both states to answer with and
             * this one has only the immediate form, so what it answers about is the thing that is
             * already settled - whether the count is a constant at all.
             *
             * The gap that leaves is a constant the peephole then declines to embed, which happens
             * only where some *other* use of it cannot take it in the encoding. `checkFormOperands`
             * is what catches it, one stage later and as an assertion rather than as a diagnostic.
             */
            // `isImm` is the *embedded* question and this one is not it - see above. Read through
            // the splat as well, because `tryFoldLoad` selects a form for every instruction it walks
            // past and that walk starts above `unwrapVectorShiftCounts`.
            if(!packedShiftConstantCount(base, inst)) {
                auto direction = inst->kind == LowerInst::Shl ? 0 : inst->kind == LowerInst::Shr ? 1 : 2;

                /*
                 * A count that is a whole *vector* is one per lane, which is AVX2's family and
                 * nothing at all below it. `scalarizeVectorLanes` takes away every one this has no
                 * row for, so reaching here with one means the row exists - which is asserted rather
                 * than answered around, a zero here reading as "not a packed operation".
                 */
                if(isVectorLike(base[((LowerInstBinary*)inst)->rhs]->type)) {
                    auto row = kShiftVar[direction][laneBytes(type.lane) == 8 ? 1 : 0];
                    assertTrue(row != 0 && laneBytes(type.lane) >= 4);

                    return widthForm(row, type);
                }

                assertTrue(packedShiftSharedCount(base, inst));
                return widthForm(kShiftReg[direction], type);
            }

            switch(inst->kind) {
                case LowerInst::Shl: return packedForm(kShlImm, type);
                case LowerInst::Shr: return packedForm(kShrImm, type);
                default:             return packedForm(kSarImm, type);
            }
        }

        /*
         * A comparison into a mask.
         *
         * The float forms are one instruction with a predicate, so every relation the IR can state
         * reaches them - `gt` and `ge` after `orderPackedCompare` has exchanged their operands. The
         * integer ones are two instructions and no predicate: equality and signed-greater, with
         * signed-less reaching the second by the same exchange. What is left over is refused, and
         * each refusal is the same missing piece - inverting a mask needs an all-ones vector, which
         * needs a broadcast or a constant this backend cannot yet produce.
         */
        case LowerInst::Cmp: {
            auto type = base[((LowerInstBinary*)inst)->lhs]->type;
            if(!isVectorLike(type)) return 0;

            // The three the machine has only the complement of are one pseudo whatever the lane
            // width, since what the widths differ in is a byte the expansion reads rather than
            // anything the allocator does.
            if(packedCompareIsInverted(type, ((LowerInstCmp*)inst)->getCmp())) return widthForm(FormVCmpInverted, type);

            switch(((LowerInstCmp*)inst)->getCmp()) {
                case LowerCmp::eq:
                case LowerCmp::neq:
                case LowerCmp::lt:
                case LowerCmp::le:
                case LowerCmp::uno:
                case LowerCmp::ord:
                    // Every float relation is one form; only equality is also an integer one.
                    assertTrue(isFloatVector(type) || ((LowerInstCmp*)inst)->getCmp() == LowerCmp::eq);
                    return packedForm(kCmpEq, type);

                case LowerCmp::igt:
                    assertTrue(isIntVector(type)); // a signed relation between float vectors
                    return packedForm(kCmpGt, type);

                default:
                    assertTrue("no packed comparison for this relation yet" == nullptr);
                    return 0;
            }
        }

        /*
         * The minimum and the maximum, which index their row by lane *and* by signedness - the one
         * packed family the machine spells twice at the same width.
         *
         * The four unsigned float entries are the signed ones repeated rather than left empty: a
         * float lane has one ordering, so `isSignedLanes` is false for every `minps` this reaches
         * and reading the row at the unsigned column has to find it there.
         */
        case LowerInst::X86MinMax: {
            static const MachineFormId kMinSigned[6] = {
                FormVMinI8, FormVMinI16, FormVMinI32, 0, FormVMinF32, FormVMinF64,
            };
            static const MachineFormId kMinUnsigned[6] = {
                FormVMinU8, FormVMinU16, FormVMinU32, 0, FormVMinF32, FormVMinF64,
            };
            static const MachineFormId kMaxSigned[6] = {
                FormVMaxI8, FormVMaxI16, FormVMaxI32, 0, FormVMaxF32, FormVMaxF64,
            };
            static const MachineFormId kMaxUnsigned[6] = {
                FormVMaxU8, FormVMaxU16, FormVMaxU32, 0, FormVMaxF32, FormVMaxF64,
            };

            auto minMax = (LowerInstX86MinMax*)inst;
            auto type = minMax->result.type;

            if(minMax->isMax()) return packedForm(minMax->isSignedLanes() ? kMaxSigned : kMaxUnsigned, type);
            return packedForm(minMax->isSignedLanes() ? kMinSigned : kMinUnsigned, type);
        }

        /*
         * The widening even-lane product, which is one row per signedness and no lane column at all:
         * `pmuludq` and `pmuldq` read 32-bit lanes and write 64-bit ones, and there is no other
         * width of either.
         *
         * The type read is the *result*'s, which is the 64-bit-lane one - so `widthForm` picks the
         * 128- or 256-bit twin from the register the answer occupies rather than from the operands',
         * and the two are the same register width by construction.
         */
        case LowerInst::X86MulWide: {
            auto wide = (LowerInstX86MulWide*)inst;
            auto type = wide->result.type;

            assertTrue(isIntVector(type) && laneBytes(type.lane) == 8);

            return widthForm(wide->isSignedLanes() ? FormVIMulWide32 : FormVMulWide32, type);
        }

        /*
         * A vector masked by a mask, which is the bitwise `and` and picks its row the way the
         * ordinary one does: by the *domain* rather than by the lane width, since neither `pand` nor
         * `pandn` has a lane at all.
         *
         * The type read is the instruction's result, which is the value's type and not the mask's -
         * a mask lane is a truth value, so asking `isFloatVector` of one would answer false for a
         * masked float vector and put its result back in the integer domain.
         */
        case LowerInst::X86MaskAnd: {
            auto masked = (LowerInstX86MaskAnd*)inst;
            auto type = masked->result.type;

            assertTrue(isWholePackedRegister(type)); // no forms for a vector of any other width

            if(isFloatVector(type)) {
                auto single = laneBytes(type.lane) == 4;

                if(masked->isComplemented()) {
                    return widthForm(single ? FormVAndNotF32 : FormVAndNotF64, type);
                }

                return widthForm(single ? FormVAndF32 : FormVAndF64, type);
            }

            return widthForm(masked->isComplemented() ? FormVAndNot : FormVAnd, type);
        }

        /*
         * The general lane permutation, whose two rows differ by the *domain* of the value being
         * permuted and by nothing else - the index vector is an integer vector in both.
         *
         * A 256-bit 32-bit lane and nothing else. `lowerWideLanePermutes` is what builds one, and it
         * builds none at any other shape; this asserts that rather than choosing a neighbouring row,
         * because there is no neighbouring row to choose - `vpermq` has an immediate pattern and
         * `vpermb` is AVX-512.
         */
        case LowerInst::X86Permute: {
            auto type = ((LowerInstX86Permute*)inst)->result.type;

            /*
             * The byte row, which is the *narrow* tier's and is `pshufb`. A byte lane's index and a
             * byte offset are the same number, so "one index per lane" reads the same here as it
             * does above - which is why `lowerByteLaneShuffles` emits this kind at a byte lane and
             * at no other, rather than this row growing a second meaning for its indices.
             */
            if(laneBytes(type.lane) == 1) {
                assertTrue(isWholePackedRegister(type) && !isWideVector(type));
                return FormVByteShuffle;
            }

            assertTrue(isWideVector(type) && laneBytes(type.lane) == 4);
            return isFloatVector(type) ? FormVPermuteF32 : FormVPermute32;
        }

        /*
         * A lane-wise select, which is one form for every lane type and for a mask: the three
         * bitwise instructions it expands into have no lane width, so nothing here indexes a row.
         */
        case LowerInst::Select: {
            auto type = ((LowerInstSelect*)inst)->result.type;
            if(!isVectorLike(type)) return 0;

            /*
             * `pblendvb` at 128 bits without VEX, and the expansion everywhere else.
             *
             * A 256-bit vector exists only in a build that has VEX, and a VEX build's expansion is
             * already the one instruction with the mask as a real operand - so the blend row is
             * exactly the case that is left: legacy encoding, 128 bits, SSE4.1 (which §38 made the
             * floor). See the row itself for what the xmm0 clobber is being traded for.
             */
            if(!isWideVector(type) && !packedNeedsVex()) return FormVSelectBlend;

            return widthForm(FormVSelect, type);
        }

        // A complement, which has no lane width for the same reason the bitwise pair above has none.
        case LowerInst::Not: {
            auto type = ((LowerInstUnary*)inst)->result.type;
            if(!isVectorLike(type)) return 0;
            return widthForm(FormVNot, type);
        }

        // A negation, which does: what it subtracts from zero at an integer lane is that lane's
        // width, and what it toggles at a float one is that lane's sign bit.
        case LowerInst::Neg: {
            static const MachineFormId kNegate[6] = {
                FormVNeg8, FormVNeg16, FormVNeg32, FormVNeg64, FormVNegF32, FormVNegF64,
            };

            auto type = ((LowerInstUnary*)inst)->result.type;
            if(!isVectorLike(type)) return 0;
            return packedForm(kNegate, type);
        }

        // The magnitude of an integer lane, at the three widths SSSE3 gives one.
        case LowerInst::Abs: {
            static const MachineFormId kAbsolute[6] = { FormVAbs8, FormVAbs16, FormVAbs32, 0, 0, 0 };

            auto type = ((LowerInstUnary*)inst)->result.type;
            if(!isVectorLike(type)) return 0;
            return packedForm(kAbsolute, type);
        }

        /*
         * The square root and the fused multiply-add, which are answered here for every width -
         * scalar included - rather than only for the packed ones.
         *
         * This function is "the packed form or nothing" everywhere else, and these two are the
         * exception because the machine makes them one: `sqrtss` and `sqrtps` are one opcode with
         * two prefixes, so splitting the answer across two switches would be describing one
         * instruction family in two places.
         */
        case LowerInst::Sqrt: {
            auto type = ((LowerInstUnary*)inst)->result.type;

            if(isVectorLike(type)) return widthForm(laneBytes(type.lane) == 4 ? FormVSqrtF32 : FormVSqrtF64, type);
            return type == LowerType::Float32 ? FormSqrt32 : FormSqrt64;
        }

        case LowerInst::Fma: {
            auto type = ((LowerInstFma*)inst)->result.type;

            if(isVectorLike(type)) return widthForm(laneBytes(type.lane) == 4 ? FormVFmaF32 : FormVFmaF64, type);
            return type == LowerType::Float32 ? FormFma32 : FormFma64;
        }

        // The three directed roundings, answered here at every width for `Sqrt`'s reason: `roundss`
        // and `roundps` are one instruction family, so one switch answers for the whole of it. Which
        // of the three a form is selected for is not a question the form knows - the mode is the
        // trailing byte, and `packedTrailingByte` reads it off the kind.
        case LowerInst::Trunc:
        case LowerInst::Floor:
        case LowerInst::Ceil: {
            auto type = ((LowerInstUnary*)inst)->result.type;

            if(isVectorLike(type)) return widthForm(laneBytes(type.lane) == 4 ? FormVRoundF32 : FormVRoundF64, type);
            return type == LowerType::Float32 ? FormRound32 : FormRound64;
        }

        // A copy, and a conversion between the two lane kinds. Both are ordinary forms; they are
        // here rather than in the switch below because that one answers for scalars and would take
        // a vector into a general register.
        case LowerInst::Set: {
            auto type = ((LowerInstUnary*)inst)->result.type;
            if(!isVectorLike(type)) return 0;
            return widthForm(FormVMove, type);
        }

        case LowerInst::Cast: {
            auto to = ((LowerInstCast*)inst)->result.type;
            auto from = base[((LowerInstCast*)inst)->from]->type;
            if(!isVectorLike(to) && !isVectorLike(from)) return 0;

            // The lane count is preserved by the IR's own rule, so the register width is preserved
            // exactly when the lane width is - and the only pair this tier has at one width is the
            // 32-bit one. `unsupportedVectorReason` refuses the rest.
            return widthForm(isFloatVector(to) ? FormVCastIToF32 : FormVCastFToI32, to);
        }

        /*
         * Lanes rearranged within one vector.
         *
         * The only packed form whose availability is decided by an instruction *field* rather than
         * by its type, so it is the only one that asks a question rather than indexing a row: a
         * pattern `pshufd` expresses is a form and every other pattern is a sequence this tier has
         * yet to write. Refused here rather than at the encoder, where the allocator would already
         * have placed operands for an instruction that cannot be written.
         */
        case LowerInst::VecShuffle: {
            auto choice = packedShuffleChoice(inst);

            if(!choice) {
                assertTrue("no single packed shuffle expresses this pattern yet" == nullptr);
                return 0;
            }

            return choice.unwrap().form;
        }

        // Every lane the same scalar. The row is short where the others are wide: an 8- or 16-bit
        // lane arrives as an Int32 and would need the byte and word shuffles this tier has not
        // written, so those two columns are the machine's gap rather than the IR's.
        case LowerInst::VecSplat: {
            // The two narrow columns are the AVX2 rows; a target without the extension takes the
            // baseline stand-in below, which is the same arrangement `pmulld` has.
            static const MachineFormId kBroadcast[6] = {
                FormVBroadcast8, FormVBroadcast16, FormVBroadcast32, FormVBroadcast64,
                FormVBroadcastF32, FormVBroadcastF64
            };

            auto type = ((LowerInstVecSplat*)inst)->result.type;

            /*
             * The two constants that are an instruction rather than a broadcast - §5.7.
             *
             * Asked of the operand rather than of the lane, which is the same shape the packed shift
             * uses and is asked for the same reason §5.6 gives: what is already settled by the time
             * `selectForm` has to be total is whether the operand is a constant at all, and this one
             * is settled earlier still - `poolVectorConstants` leaves exactly these two behind and
             * pools everything else, so a splat of a constant reaching here is one of the two.
             */
            if(auto pattern = splatConstantPattern(base, inst)) {
                auto wide = isWideVector(type);

                if(pattern.unwrap() == SplatPattern::Zero) return wide ? FormVWideZero : FormVZero;
                return wide ? FormVWideOnes : FormVOnes;
            }

            // The baseline's byte and word broadcasts, which are sequences rather than instructions
            // - `packedForm` would answer the AVX2 row, which this build cannot encode.
            if(!(targetFeatures() & kFeatureAvx2) && laneBytes(type.lane) < 4) {
                return laneBytes(type.lane) == 1 ? FormVBroadcast8Sse : FormVBroadcast16Sse;
            }

            return packedForm(kBroadcast, type);
        }

        /*
         * One lane out of a vector, and the one place in this table where the *feature set* chooses
         * between two forms of different reach rather than between two encodings of one.
         *
         * A float lane is a shuffle at every level. An integer lane is `pextr` where SSE4.1 is
         * claimed and `movd`/`movq` otherwise, and the second reaches lane zero only - which is why
         * this is not an `alternative` chain: an alternative is the same operation encoded better,
         * and these two do not do the same thing.
         */
        case LowerInst::VecLane: {
            auto lane = (LowerInstVecLane*)inst;
            auto type = base[lane->from]->type;
            if(!isVectorLike(type)) return 0;

            // At 256 bits a lane access is a different operation rather than a wider encoding of
            // this one - every lane instruction AMD64 has names a lane inside one 128-bit register -
            // so the wide tier has its own pair of pseudos and this row is not widened into it.
            if(isWideVector(type)) {
                if(isFloatVector(type)) {
                    return laneBytes(type.lane) == 4 ? FormVWideExtractF32 : FormVWideExtractF64;
                }

                // One row for every lane a general register receives, the *opcode* being what the
                // width decides - see `emitVecWideLane`, which picks `vpextrb`/`vpextrw`/`vpextrd`
                // off the lane. A mask reads at 32 bits whatever its lanes are, for the reason the
                // narrow tier's note below gives.
                return laneBytes(type.lane) == 8 && !type.isMask()
                    ? FormVWideExtract64 : FormVWideExtract32;
            }

            /*
             * A float lane, whose lane zero is the register it is already in - a scalar float *is*
             * the low lane, so there is nothing to move and the `Low` row says so with a copy the
             * allocator usually makes disappear. Every other index is the shuffle that brings the
             * wanted lane down to it.
             */
            if(isFloatVector(type)) {
                auto narrow = laneBytes(type.lane) == 4;

                if(lane->getLane() == 0) return narrow ? FormVExtractF32Low : FormVExtractF64Low;
                return narrow ? FormVExtractF32 : FormVExtractF64;
            }

            /*
             * A mask reads at 32 bits whatever its lanes are wide, because `scalarFormOf` says a
             * mask's scalar form is an `Int32` at every lane width - what a lane holds is a truth
             * value and not a number of that width. Reading the lane's own width instead would
             * define a 64-bit register for a value the rest of the function has typed as 32-bit,
             * which is a class disagreement rather than a wrong number.
             */
            auto wide = laneBytes(type.lane) == 8 && !type.isMask();

            /*
             * And lane zero, which `movd`/`movq` reach in two opcode bytes and no index where
             * `pextrd`/`pextrq` take three and one - six bytes down to four, or five down to seven
             * at the quadword where REX.W is spent either way.
             *
             * The two rows exist already, `extractZero` above having built them as the baseline's
             * only reachable index back when there was a baseline below SSE4.1. §38 removed the
             * feature question and left them selected by nothing; what selects them now is the
             * *index*, which is the question they were always the answer to. Lane zero is where
             * every reduction ends, so this is the row the butterfly's last step reads through.
             *
             * Still not an `alternative` of the `pextr` row - see the note there. An alternative is
             * interchangeable at every call site and this one reaches one index, so the choice has
             * to be made here where the index is known rather than by `selectForm`, which sees only
             * the feature set.
             */
            /*
             * A lane narrower than four bytes, which has to be read at *its* width and not at the
             * register's: `movd` and `pextrd` both move four bytes, so a byte lane read through
             * either answers the three lanes above it as well - a silent wrong number, and one that
             * looks right whenever the neighbouring lanes are zero.
             *
             * `pextrb`/`pextrw` zero-extend, which is the whole answer for an unsigned lane. A
             * signed one is sign-extended by the two instructions `Value::VecLane` puts after this
             * in resolve/lower_calc.cpp, that being where the lane's signedness still exists - a
             * `LowerLane::Int8` states a width and nothing else.
             *
             * A **mask** keeps the 32-bit rows whatever its lanes are wide, for the reason above:
             * `scalarFormOf` makes a mask's scalar an `Int32`, so the value this defines and the one
             * the rest of the function reads have to agree on that width.
             */
            if(!type.isMask() && laneBytes(type.lane) < 4) {
                return laneBytes(type.lane) == 1 ? FormVExtract8 : FormVExtract16;
            }

            if(lane->getLane() == 0) return wide ? FormVExtract64Zero : FormVExtract32Zero;

            return wide ? FormVExtract64 : FormVExtract32;
        }

        /*
         * One lane into a vector, which is where the machine's list is longest and least regular.
         *
         * An integer lane is `pinsr` at its own width under SSE4.1 and `pinsrw` and nothing else
         * without it - so the 32- and 64-bit columns are empty at the baseline, and what fills them
         * is `lowerLaneInserts`, which has already rewritten such an insert into the pair or the
         * quadruple of *word* inserts it is made of. Reaching this with one is that pass not having
         * run, so the row asserts rather than answering a neighbouring width.
         *
         * A float lane is decided by the index as much as by the width, which no other packed form
         * here is: `insertps` reaches every lane and is SSE4.1, and the two baseline instructions
         * each write one nameable half of a two-lane vector.
         */
        case LowerInst::VecWithLane: {
            auto lane = (LowerInstVecLane*)inst;
            auto type = lane->result.type;
            if(!isVectorLike(type)) return 0;

            // The wide tier's own pair, for the reason the extract above gives.
            if(isWideVector(type)) {
                if(isFloatVector(type)) {
                    return laneBytes(type.lane) == 4 ? FormVWideInsertF32 : FormVWideInsertF64;
                }

                return laneBytes(type.lane) == 8 ? FormVWideInsert64 : FormVWideInsert32;
            }

            if(isFloatVector(type)) {
                // A quadword lane is one of the two halves of a two-lane vector and each has its own
                // baseline instruction, so this width never needs the feature and never asks.
                if(laneBytes(type.lane) == 8) {
                    return lane->getLane() == 0 ? FormVInsertF64Low : FormVInsertF64High;
                }

                return FormVInsertF32;
            }

            static const MachineFormId kInsert[6] = {
                FormVInsert8, FormVInsert16, FormVInsert32, FormVInsert64, 0, 0
            };

            return packedForm(kInsert, type);
        }

        /*
         * The one reduction that is an instruction. Every other kind has been expanded into a tree
         * by `lowerVectorReductions` long before this runs, so reaching this row with one is that
         * pass not having run - which the `opcodeFor` arm asserts rather than this one, since a
         * form of zero here is read as "not a packed operation" and would fall through silently.
         *
         * Three rows at each tier, chosen by how many bits of the answer a lane is worth: a 32- or
         * 64-bit lane takes `movmskps`/`movmskpd` and gets a bit each, everything else takes
         * `pmovmskb` and gets one per byte. `maskBitsPerLane` in transform_reduce.cpp is where the same
         * choice is made for the arithmetic above the instruction, and the two have to agree - the
         * shift a consumer applies is exactly what this row decides.
         */
        case LowerInst::VecReduce: {
            if(((LowerInstVecReduce*)inst)->getReduce() != LowerReduce::Bits) return 0;

            auto type = base[((LowerInstVecReduce*)inst)->from]->type;
            auto width = laneBytes(type.lane);
            auto id = width == 4 ? FormVMaskBitsF32 : width == 8 ? FormVMaskBitsF64 : FormVMaskBits;

            return widthForm(id, type);
        }

        default:
            return 0;
    }
}

/*
 * What this backend cannot do to a vector, said in words - see checkVectorSupported in machine.h.
 *
 * Written directly beside `selectPackedForm` because it is the same set of refusals read the other
 * way round, and the two have to be kept in step by being read together. Every branch here has a
 * counterpart above that asserts, so a debug build compiling anything that reaches one of these
 * checks the pair against each other.
 *
 * Answers nothing where the instruction is not a vector one at all, or is one this tier can emit.
 */
static Maybe<StringView> unsupportedVectorReason(LowerBase base, LowerInst* inst) {
    auto packedType = [&]() -> Maybe<LowerType> {
        /*
         * The type the operation works at, which is not always its result's.
         *
         * A comparison answers a mask, which states the lane width but not what the machine had to
         * do to produce it. A lane extract and a reduction answer a *scalar*, so asking the result
         * would conclude that neither is a vector operation at all - and this function would then
         * answer "nothing wrong" for the two instructions it most needs to refuse.
         */
        switch(inst->kind) {
            case LowerInst::Cmp:
                return Just(base[((LowerInstBinary*)inst)->lhs]->type);
            case LowerInst::VecLane:
                return Just(base[((LowerInstVecLane*)inst)->from]->type);
            case LowerInst::VecReduce:
                return Just(base[((LowerInstVecReduce*)inst)->from]->type);

            /*
             * Whichever end of a bitcast is a vector, which is not always the result and not always
             * the source.
             *
             * A vector bitcast is legal exactly where *both* ends are vectors of one width - the
             * lower validator says so and `resolve`'s verifier refuses the mixed form outright - so
             * naming the vector end here is enough to reach the case below, which is where the pair
             * is checked against each other rather than one of them against the machine.
             */
            case LowerInst::Bitcast: {
                auto source = base[((LowerInstUnary*)inst)->from]->type;
                return Just(isVectorLike(source) ? source : inst->created()[0].type);
            }
            default:
                break;
        }

        if(inst->createdCount != 1) return {};
        return Just(inst->created()[0].type);
    }();

    if(!packedType || !isVectorLike(packedType.unwrap())) return {};
    auto type = packedType.unwrap();

    /*
     * A whole register, at one of the two widths this backend holds one at.
     *
     * Two refusals wearing one message before the wide tier landed, and they are worth telling
     * apart now that one of them has moved. **Wider** is 512 bits, which needs the EVEX move row and
     * the mask bank - `targetVectorBytes` answers 64 only under AVX-512, so a program reaches this
     * by naming that level. **Narrower** is the more surprising one and has not moved at all:
     * `i32x2` is eight bytes and sits in an xmm quite happily, but every transfer here reads and
     * writes the whole register whatever the type says, so a *store* of one would write eight bytes
     * past its object.
     */
    if(!isWholePackedRegister(type)) {
        if(isWideVector(type) && !(targetFeatures() & kFeatureAvx2)) {
            return Just("a vector wider than 128 bits needs AVX2, which this target does not claim - the wide tier's forms are all VEX-encoded"_v);
        }

        if(type.byteWidth() > 32) {
            return Just("this backend holds a vector in a 128- or a 256-bit register, and has no way to move one wider than that - a 512-bit value needs the EVEX move row and the mask bank"_v);
        }

        return Just("this backend holds a vector in a whole register, and has no way to move one narrower than that - a transfer reads and writes the register's own width whatever the type says"_v);
    }

    switch(inst->kind) {
        /*
         * A reduction, which `lowerVectorReductions` expands into a tree of shuffles and pairwise
         * operations - so what can be refused about one is what its *expansion* would reach, and
         * this is the one entry in this function that has to read a pass rather than a form table.
         *
         * Two things it can reach and this tier cannot emit. The shuffle at every level of the tree
         * is a `pshufd` and there is no byte or word one, so a lane narrower than four bytes has no
         * route; and the pairwise step of a `mul` is a packed integer multiply, which exists at a
         * 16-bit lane alone - a width the first rule has already excluded, so an integer product
         * reduction has no lane width at all here.
         */
        case LowerInst::VecReduce: {
            /*
             * ~~A lane narrower than four bytes needs the byte and word shuffles this backend does
             * not have.~~ `expandNarrowReduce` is what such a reduction takes now: the levels whose
             * partner is a whole 32-bit lane away are the `pshufd` this backend already has, and the
             * one or two levels inside a 32-bit lane happen after the crossing to a general register
             * that every reduction ends in anyway.
             *
             * What is left of the old rule is the product, and only at a byte lane: the pairwise step
             * is a packed multiply, and the machine's narrowest is `pmullw` at sixteen bits.
             */
            /*
             * ~~What is left of the old rule is the product, and only at a byte lane: the pairwise
             * step is a packed multiply, and the machine's narrowest is `pmullw` at sixteen
             * bits.~~ Nothing is left of it. Every integer lane width has a multiply now - the byte
             * and the quadword are expansions rather than forms (`expandByteMul`,
             * `expandQuadwordMul`), and both run below `lowerVectorReductions`, so the tree this
             * builds is expanded along with every other multiply in the function.
             */

            /*
             * A minimum or a maximum, whose pairwise step is a packed comparison - so it reaches
             * exactly the lane widths one does. That used to be the 32-bit lane alone for the
             * *unsigned* pair, because the bias `biasUnsignedPackedCompares` builds is a splat and
             * the narrow broadcasts did not exist; ~~what is left is the quadword, where `pcmpgtq`
             * is SSE4.2~~ - which is the floor, so nothing is left. The pairwise step of a quadword
             * ordering is `pcmpgtq` and a lane-wise select, neither of which needs a feature this
             * target does not claim, and there is no lane width here to refuse.
             *
             * It is still not a `pminsq`: `packedMinMaxSupported` answers false at this width and
             * keeps the comparison and the blend the reduction was written as, which is why lifting
             * the refusal needed no new form beyond the comparison itself.
             */

            return {};
        }

        // A lane extract is reachable at every index and every feature level: `pextrd`/`pextrq` take
        // one directly under SSE4.1, and `lowerLaneExtracts` brings the wanted lane down to zero
        // with a shuffle otherwise. Nothing to refuse.
        case LowerInst::VecLane:
            return {};

        /*
         * A lane insert, which is the one packed operation with a hole in it at the baseline.
         *
         * The integer half is complete at every width once `lowerLaneInserts` has run - it takes the
         * 32- and 64-bit lanes down to the word inserts `pinsrw` can write - and the byte lane is the
         * exception, because a byte is *half* a word and reaching one needs the word around it read
         * back out first.
         *
         * The float half is `insertps` under SSE4.1 and, without it, `movsd`/`unpcklpd` for a
         * quadword lane and lane zero alone for a single one. So the refusal is a lane index rather
         * than a lane width, which no other entry in this function is.
         */
        case LowerInst::VecWithLane: {
            auto lane = ((LowerInstVecLane*)inst)->getLane();

            /*
             * A mask, whose scalar form is an `Int32` at every lane width (`scalarFormOf`) because
             * what a lane holds is a truth value rather than a number of that width. So the operand
             * this would be handed and the lane it would be written into disagree about their width
             * for every mask but a 32-bit one, and there is no instruction that means "write this
             * truth value into that lane" to settle it in either direction.
             *
             * Nothing produces one - `withLane` is declared over `Vec(a, n)` and a mask arrives from
             * a comparison - so this guards a hand-written lower IR rather than catching a program.
             */
            if(type.isMask()) {
                return Just("a lane of a mask cannot be written here - a mask lane is all-ones or all-zeros and its scalar form states no width, so build the mask with a comparison instead"_v);
            }

            // Every lane width has an insert at the floor: `pinsrb`, `pinsrw`, `pinsrd`, `pinsrq`
            // and `insertps` are all SSE4.1, which is v2. The byte lane was the one this refused
            // before the floor was named, since `pinsrw` writes a whole word.
            return {};
        }

        // A vector read at another lane shape, which is the register itself - so what is refused is
        // an end that is not a vector this backend can hold at all. The width check above has
        // already answered for whichever end this function was pointed at; this is the other one.
        case LowerInst::Bitcast: {
            auto other = isVectorLike(base[((LowerInstUnary*)inst)->from]->type)
                ? inst->created()[0].type : base[((LowerInstUnary*)inst)->from]->type;

            if(!isVectorLike(other) || !isWholePackedRegister(other)) {
                return Just("a bitcast between a vector and something that is not one has no meaning here - a lane is read with `vlane` and a vector built with `vsplat`"_v);
            }

            return {};
        }

        /*
         * A splat of an 8- or 16-bit lane used to be refused here - there is no byte or word
         * broadcast below SSSE3 - and then became a *pass*, the scalar replicated into a 32-bit
         * pattern with one `imul` before the 32-bit broadcast did the rest. It is a form again now,
         * and two of them: `vpbroadcastb`/`vpbroadcastw` where AVX2 is there, and `pshufb` against
         * zeros or a pair of shuffles where it is not. Nothing about the lane width is a refusal at
         * any feature level, and nothing about it is a pass either.
         */
        case LowerInst::VecSplat:
            return {};

        case LowerInst::VecShuffle:
            if(!packedShuffleChoice(inst)) {
                if(isWideVector(type)) {
                    /*
                     * The refusal that is specific to this tier: every shuffle AVX2 has works
                     * *inside* each 128-bit half, so a pattern that moves a lane across the middle
                     * is not an instruction unless it moves the whole half.
                     *
                     * ~~`vpermd` would express the general case and takes its pattern out of a
                     * vector register, which needs the constant pool this backend has not opened to
                     * vectors.~~ The pool is open, and `lowerWideLanePermutes` is what spends it: a
                     * 32-bit lane pattern naming **one** source is `vpermd`/`vpermps` and a pooled
                     * index vector, so it is let through here and lowered below.
                     *
                     * What is still refused is the two-source case at that width, and every pattern
                     * at a 64-bit lane that is neither in-lane nor a half exchange - `vpermq` states
                     * its pattern as an immediate and so belongs to `wideShuffleChoice`, which does
                     * not have it yet.
                     */
                    if(laneBytes(type.lane) == 4 && shuffleReadsOneSource(inst)) return {};

                    return Just("no single instruction here expresses this lane pattern at 256 bits - every shuffle at this width works inside each 128-bit half, the only crossing is an exchange of whole halves, and the general permute reads one source where this pattern names two"_v);
                }

                /*
                 * ~~An 8- or 16-bit lane has nothing beyond those.~~ A **byte** lane has `pshufb`,
                 * which expresses any permutation of one register and reads its control out of a
                 * vector - so it is `lowerByteLaneShuffles` and a pooled constant rather than a
                 * form, on exactly the terms `lowerWideLanePermutes` is let through above. Both of
                 * the reasons the old text gave have expired: SSSE3 is inside the x86-64-v2 floor,
                 * and `poolVectorConstants` opened `.rodata` to vectors.
                 *
                 * What is still refused: the two-source case, which is two `pshufb` and a `por`; and
                 * a 16-bit lane, whose pattern is a byte pattern with the pairs written out and
                 * which a program reaches today by bitcasting to `Vec(U8, 16)`.
                 */
                if(laneBytes(type.lane) == 1 && !type.isMask() && shuffleReadsOneSource(inst)) return {};

                return Just("no single instruction here expresses this lane pattern - `shufps` takes a run of lanes from each source and the interleaves take one of each, a 16-bit lane has nothing beyond those, and the general byte permute reads one source where this pattern names two"_v);
            }

            return {};

        /*
         * The multiply, in both signednesses.
         *
         * `IMul` and not only `Mul`, which is the whole reason this branch is worth its own note:
         * `signedOperand` answers a vector's *lane's* signedness, so an ordinary `Vec(Int)`
         * multiplication arrives here as `IMul` and never as `Mul` (§9.5 of
         * Implementation-Vector.md records the same rename in the goldens). A check written for the
         * unsigned kind alone passes every program anybody writes.
         *
         * `pmullw` keeps the low half, which is the same bits for both signednesses, so the 16-bit
         * lane is one form for both kinds and the rest is the machine's gap: there is no packed
         * multiply of a byte or a quadword at any level, and the 32-bit one is SSE4.1's `pmulld`.
         */
        case LowerInst::Mul:
        case LowerInst::IMul:
            // ~~The machine has no packed integer multiply of a byte or quadword lane at any feature
            // level.~~ It has not, and both are built out of the ones it does have: the byte lane is
            // the word product masked, the quadword is long multiplication over `pmuludq`. See
            // `expandByteMul` and `expandQuadwordMul` in transform_expand.cpp. Nothing left to refuse.
            return {};

        // The high half, which exists at a 16-bit lane (`pmulhw`/`pmulhuw`) and nowhere else: there
        // is no packed multiply of a byte or a quadword at any level, and a 32-bit lane's `pmulld`
        // keeps the low half alone - the widening `pmuludq` is the only route to its top half and is
        // an expansion rather than a form. So the refusal is every lane width but the word.
        case LowerInst::MulHi:
        case LowerInst::IMulHi:
            // A 16-bit lane is a form (`pmulhw`/`pmulhuw`) and the 32- and 64-bit ones are
            // `expandVectorMulHi`, which builds both out of the widening even-lane product. ~~A
            // 32-bit lane's product keeps its low half only~~ is true of `pmulld` and false of the
            // instruction one would use. The byte lane is what is left, `pmuludq` being the
            // narrowest widening product the machine has.
            if(laneColumn(type) == 0) {
                return Just("the machine has no packed multiply-high of a byte lane, and no widening product narrow enough to build one out of - `pmuludq` is the narrowest there is"_v);
            }

            return {};

        case LowerInst::Div:
        case LowerInst::IDiv:
        case LowerInst::Rem:
        case LowerInst::IRem:
            // ~~No x86 has a packed integer divide or remainder - it expands lane by lane, which this
            // backend does not do yet.~~ It does now: `scalarizeVectorDivision` is that expansion,
            // and the note there is why it is worth having even though it is the one packed
            // operation that does not beat scalar code on its own.
            return {};

        case LowerInst::Shl:
        case LowerInst::Shr:
        case LowerInst::Sar: {
            /*
             * Read through the splat the language's spelling wraps the count in - see
             * `packedShiftSharedCount`, and `unwrapVectorShiftCounts` in transform_expand.cpp, which is the
             * pass this stands on the other side of.
             *
             * ~~What is left refused is a count that is not a constant at all.~~ A count that is not
             * a constant is the machine's register form, whose count sits in the low quadword of a
             * vector register - so what the expansion adds in front of it is the bank crossing, and
             * `movd` clearing everything above the count is exactly what that form needs.
             *
             * What is still refused is a count that is not *shared*: one count per lane is AVX2's
             * `vpsllv` family and no row here. The language cannot write one - `Integral(a)` types
             * both operands as the same `a`, so a vector count reaches here as a splat - so this
             * guards a hand-written lower IR.
             */
            /*
             * ~~A count that is not *shared* has no form here: one count per lane is AVX2's `vpsllv`
             * family and no row here. The language cannot write one - `Integral(a)` types both
             * operands as the same `a`, so a vector count reaches here as a splat - so this guards a
             * hand-written lower IR.~~
             *
             * It types them as the same `a`, which is exactly why `v `shl` w` over two vectors
             * typechecks and why that refusal was reachable from a program. It is
             * `scalarizeVectorLanes`' business now, one lane at a time, since AVX2 has the family
             * and v2 has nothing at all.
             */

            // ~~The machine has no packed shift of a byte lane.~~ It has not, and it does not have
            // to: `expandByteShifts` borrows the word shift and masks away the bits that crossed the
            // byte boundary, which is two instructions and a constant for sixteen lanes.

            /*
             * ~~There is no packed arithmetic shift of a quadword before AVX-512.~~ There is not,
             * and there does not have to be: `expandQuadwordSar` builds one out of the logical shift
             * the machine does have and a bias, which is the standard identity
             *
             *     x >>a n  ==  ((x >>l n) ^ K) - K,   K = 1 << (63 - n)
             *
             * and costs three instructions with the bias hoisted. Every other lane width is a form,
             * and this one is refused nowhere now.
             */

            return {};
        }

        /*
         * The two rotations, which have a packed form only at AVX-512 (`vprold`) and are refused
         * nowhere anyway: `expandVectorRotate` turns one into the two shifts above and an `or`, all
         * three of which this arm has just finished saying are supported at every lane width.
         *
         * A separate arm rather than a label on the shifts, because what makes them supported is not
         * the same fact: a shift is a form or an expansion of one, and a rotation never reaches a
         * form at all below v4. This check runs at the top of `transformFunction` and so sees the
         * rotation the expansion is about to remove, which is why the arm has to exist.
         */
        case LowerInst::Rol:
        case LowerInst::Ror:
            return {};

        /*
         * A conversion between the two lane kinds, which at this register width is one pair.
         *
         * The IR's rule is that a `Cast` between vectors preserves the lane *count*, so a conversion
         * that changes the lane width changes the register width with it - and there is one register
         * here. `i32x4` to `f32x4` is the pair that keeps both; `i32x4` to `f64x4` is thirty-two
         * bytes and belongs to the ymm tier.
         */
        case LowerInst::Cast: {
            auto to = ((LowerInstCast*)inst)->result.type;
            auto from = base[((LowerInstCast*)inst)->from]->type;

            if(!isVectorLike(to) || !isVectorLike(from) || !isWholePackedRegister(from)) {
                return Just("a conversion between a vector and something that is not one has no meaning here"_v);
            }

            if(laneBytes(to.lane) != 4 || laneBytes(from.lane) != 4 || isFloatVector(to) == isFloatVector(from)) {
                return Just("the only packed conversion that keeps one register width is between 32-bit integer lanes and float ones - any other lane width would need a register twice as wide on one side"_v);
            }

            return {};
        }

        // A copy and a negation, at every lane type the table has a row for. The negation's rows are
        // complete, since what each of them needs is a constant the expansion builds rather than an
        // instruction the machine may not have.
        case LowerInst::Set:
        case LowerInst::Neg:
            return {};

        case LowerInst::Cmp: {
            // The relation this will be *emitted* at, which is not always the one it was written
            // with: `canonicalizeOperands` has not run when this check does, so asking the written
            // relation refuses a `cmp_ilt` that reaches `pcmpgt` by an exchange one pass later.
            auto relation = packedCompareRelation(((LowerInstCmp*)inst)->getCmp());

            /*
             * ~~There is no packed compare of a quadword lane before SSE4.1 (`pcmpeqq`) and SSE4.2
             * (`pcmpgtq`)~~ - which is a refusal that outlived the floor it was written against.
             * Both of those levels are inside v2, so the quadword lane has the same two relations
             * every other integer width has, the three complements built on top of them, and the
             * four unsigned ones the sign-bit bias turns into those - a bias at a 64-bit lane being
             * a pooled constant splat like the bias at any other.
             *
             * That leaves nothing about an integer lane's *width* to refuse here, and the switch
             * below - which is about the relation - is the whole of what this arm does.
             */
            if(isFloatVector(type)) return {};

            // The six a signed lane can be compared with: two the machine has outright, one that
            // reaches `pcmpgt` by an exchange, and three that are the complement of one of those.
            switch(relation) {
                case LowerCmp::eq:
                case LowerCmp::igt:
                case LowerCmp::neq:
                case LowerCmp::ile:
                    return {};

                /*
                 * And the four unsigned ones, which `biasUnsignedPackedCompares` turns into the four
                 * above by flipping the top bit of every lane. ~~The bias is a splat, so it is a
                 * 32-bit lane alone - the two narrower ones have no broadcast here.~~ Every lane
                 * width the signed relations have: the bias is a constant splat, which is pooled
                 * before it is anything, and a *runtime* splat of a narrow lane is a form of its own
                 * at every feature level now.
                 *
                 * `ige` is not in this list and needs no entry: `packedCompareRelation` has already
                 * turned it into `ile`, which is.
                 */
                case LowerCmp::lt:
                case LowerCmp::le:
                case LowerCmp::gt:
                case LowerCmp::ge:
                    return {};

                default:
                    break;
            }

            return Just("there is no unsigned packed comparison on this machine, and biasing the operands into a signed one needs a broadcast this lane width does not have"_v);
        }

        // A complement, at every lane width and over a mask: `pcmpeqd` against a scratch register
        // makes the all-ones vector it needs out of whatever that register held.
        case LowerInst::Not:
            return {};

        // The square root, at both packed widths, and the multiply-add, which has a form where the
        // target claims FMA3 and is the multiply and the add `expandFusedMultiplyAdd` writes where
        // it does not. Neither is ever refused - the validator has already held both to a float.
        case LowerInst::Sqrt:
        case LowerInst::Fma:
        // The three directed roundings, which have a form at both packed widths, and the ties-away
        // one, which `expandRoundAway` has removed before this runs. None is ever refused - the
        // validator has already held all four to a float.
        case LowerInst::Trunc:
        case LowerInst::Floor:
        case LowerInst::Ceil:
        case LowerInst::Round:
            return {};

        /*
         * Everything left, refused - which is the polarity that matters and is the one this function
         * did not have.
         *
         * A `default` answering "nothing wrong" means every instruction kind that acquires a vector
         * operand later is silently supported until somebody notices, and §5.6's whole lesson is
         * that "silently" here means a release build emitting a scalar form over a vector value. It
         * cost a wrong answer twice while this tier was being filled in: `Neg` over a float vector
         * reached the scalar sign-bit toggle, and `Set` over a vector reached a general-register
         * move. Both compiled, neither asserted in the configuration anybody runs, and the fixture
         * that caught them reported an exit code twenty-four short.
         *
         * The kinds that carry a vector and belong to no form of this table are the ones the *frame*
         * and the conventions answer for - an argument, a phi, a return, a call operand, a load, a
         * store - and each of them names its list here rather than falling through, so that the
         * question "is this kind supported" has one place that answers it.
         */
        case LowerInst::Arg:
        /*
         * The magnitude, which is a form at three integer lane widths and an `and` against a pooled
         * mask at both float ones. ~~The quadword integer lane is the gap, there being no `pabsq`
         * outside AVX-512 and no `pcmpgtq` below SSE4.2 to build the comparison-and-select fallback
         * out of either.~~ The second half of that stopped being true when §38 named v2 the floor,
         * and the fallback is `expandVectorAbs`' business now - a comparison against a hoisted zero,
         * a negation and a blend. Nothing here to refuse at any lane width.
         */
        case LowerInst::Abs:
        // The widening even-lane product, which the backend writes for itself and only at the one
        // shape it has rows for - `expandQuadwordMul` builds none other, and `selectPackedForm`
        // asserts that rather than answering a neighbouring width.
        case LowerInst::X86MulWide:
        case LowerInst::Phi:
        case LowerInst::Load:
        case LowerInst::Store:
        case LowerInst::Ret:
        case LowerInst::Call:
        case LowerInst::Select:
        case LowerInst::Add:
        case LowerInst::Sub:
        case LowerInst::And:
        case LowerInst::Or:
        case LowerInst::Xor:
            return {};

        default:
            return Just("this backend has no packed form for this operation"_v);
    }
}

/*
 * A wide vector value inside a function that either is encoded without a vector prefix or clears the
 * vector state - the one thing neither can survive, reported rather than emitted.
 *
 * **Wide, and not "vector"**, which is the whole precision of this check. `vzeroupper` zeroes bits
 * 128 and up and leaves the low half of every register alone, so a `Vec(U32, 4)` held in an xmm is
 * untouched by it and a `Vec(I32, 8)` is destroyed. An earlier reading of this had the entry reset
 * unsafe in any function taking a vector argument, and concluded that such a function needed a
 * calling convention that preserved nothing - which was two mistakes: an xmm argument survives, and
 * `Complex` already clobbers all sixteen vector registers anyway, so no convention this compiler
 * enters a function under has a callee-saved one to protect.
 *
 * The attribute promises that no instruction in this body carries a VEX prefix, which is what stops
 * the crossing between prefixed and unprefixed encodings that costs 140x on the part it was measured
 * on - see `legacyVectorEncodings` in target.h. `vectorClassNeedsVex` keeps that promise for the
 * three *narrow* classes and answers `false` for the wide ones, because they have no legacy spelling
 * to be chosen against: a 256-bit move is VEX whatever any function says. So a wide value inside a
 * marked function is a body that mixes the two encodings while claiming not to, and nothing else in
 * the backend would say so.
 *
 * **This is a hole in the mechanism as it stood rather than one the attribute introduced.** The pass
 * that used to decide the same flag by walking the call graph had it too, and could not have
 * reported it: it marked a function *because* it held a SHA instruction, so there was no declaration
 * to point a diagnostic at and no author to have written it.
 *
 * Asked of the types rather than of the placement, which is a superset: a function naming a 32-byte
 * type and having every one of them folded away before a register is handed out would be reported
 * here and would have been harmless. That is the right way to be wrong. The exact answer only exists
 * after register allocation, which is far past the point a diagnostic can name a declaration, and a
 * region written in legacy encodings has no business naming a wide vector in the first place.
 */
bool checkLegacyVectorEncoding(Context& ctx, LowerBase base, LowerFunction& fun) {
    auto ok = true;

    /*
     * The other direction, and the backstop the declared form needs: a function holding an
     * instruction with *no* VEX spelling and not marked.
     *
     * This is what the deleted pass computed to find its roots - `functionHasLegacyOnlyVectors` -
     * kept as an assertion now that nothing infers the answer. The closure it then ran downward
     * through the call graph is what went; the root test is still exactly right, and without it the
     * attribute would fail silently in the one way that matters. An **inlined** marked function is
     * the case: the flag belongs to a `LowerFunction`, a spliced body has none, and the SHA
     * instructions would arrive in a VEX-encoded caller with nothing to say so.
     *
     * Reported against the caller, which is where the fix is: give the callee `@noinline`, or mark
     * the caller too. A kind test rather than a form test, so that the answer exists before
     * selection has run.
     */
    auto holdsReset = false;

    if(!fun.legacyVectors) {
        for(auto offset: fun.blocks.contents(base)) {
            auto block = base[offset];

            for(auto i: block->instructions.contents(base)) {
                auto inst = base[i];
                if(inst->kind == LowerInst::VZeroUpper) holdsReset = true;
                if(inst->kind != LowerInst::ShaBinary && inst->kind != LowerInst::Sha256Rounds) continue;

                ctx.diagnostics.error("x64: %@ holds a SHA-extension instruction but is not marked `@x86_legacy_sse` - that instruction has no VEX spelling, so this body would cross between prefixed and unprefixed encodings. Mark this function, or give the one it was inlined from `@noinline`"_v,
                                      inst->source, ctx.findName(fun.name));
                ok = false;
            }
        }

        // An unmarked function may still call `X86.vzeroupper()`, and the wide-vector test below is
        // what makes that safe - so it is asked whenever either is true rather than only when the
        // attribute is written. A marked function reaches it because it is marked; this one reaches
        // it because it holds the instruction the test is about.
        if(!holdsReset) return ok;
    } else {
        for(auto offset: fun.blocks.contents(base)) {
            auto block = base[offset];

            for(auto i: block->instructions.contents(base)) {
                if(base[i]->kind == LowerInst::VZeroUpper) holdsReset = true;
            }
        }
    }

    /*
     * The other half of what makes an entry reset safe, and the half that is a property of the
     * *convention* rather than of any value: no register this function is expected to give back may
     * be one the instruction clears.
     *
     * **Checked rather than assumed, and the difference is not academic.** `Complex` - what every
     * function gets by default - happens to clobber all sixteen vector registers today, so this
     * passes without a convention being written. That is a fact about the current internal
     * convention and not a promise: the backend is entitled to start preserving some of them if that
     * allocates better, and the day it does, every `X86.vzeroupper()` in the program would begin
     * corrupting a caller's register with nothing to say so. A library that wants the guarantee in
     * its own declaration writes `@convention(sysv)`, whose vector file is caller-saved *by the
     * psABI* rather than by this compiler's current judgement.
     */
    if(holdsReset) {
        auto& convention = targetConstraints().getConvention(fun.callType);

        if(convention.calleeSaved.banks[BankVector] != 0) {
            ctx.diagnostics.error("x64: %@ calls `X86.vzeroupper()` under the `%@` convention, which preserves vector registers - the instruction would clear the upper half of one this function never named and never saved. Write `@convention(sysv)`, whose vector file is caller-saved by the ABI"_v,
                                  fun.source, ctx.findName(fun.name), nameForCallType(fun.callType));
            ok = false;
        }
    }

    auto report = [&](LocationId source, StringView why) {
        ctx.diagnostics.error("x64: %@ holds a vector wider than 128 bits, and %@"_v,
                              source, ctx.findName(fun.name), why);
        ok = false;
    };

    auto why = fun.legacyVectors
        ? "is marked `@x86_legacy_sse` - a wide vector has no unprefixed encoding, so this body would mix VEX and legacy instructions, which is what the attribute is there to prevent. Narrow the type, or drop the attribute"_v
        : "calls `X86.vzeroupper()` - that instruction zeroes bits 128 and up of every vector register, so a wide value live across it is silently truncated. Narrow the type, or move the call"_v;

    for(auto type: fun.returnTypes.contents(base)) {
        if(isWideVector(LowerType(type))) report(fun.source, why);
    }

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(auto i: block->instructions.contents(base)) {
            auto inst = base[i];
            for(auto& value: inst->created()) {
                if(isWideVector(value.type)) report(inst->source, why);
            }
        }
    }

    return ok;
}

bool checkVectorSupported(Context& ctx, LowerBase base, LowerFunction& fun) {
    auto ok = true;

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(auto i: block->instructions.contents(base)) {
            auto inst = base[i];
            auto reason = unsupportedVectorReason(base, inst);
            if(!reason) continue;

            // Named by the instruction rather than by the opcode it would have taken, because it has
            // not got one - that is the whole of what is being reported.
            ctx.diagnostics.error("x64: `%@` in %@ cannot be emitted by this backend: %@"_v,
                                  inst->source, nameForInst(base, *inst), ctx.findName(fun.name),
                                  reason.unwrap());
            ok = false;
        }
    }

    return ok;
}
