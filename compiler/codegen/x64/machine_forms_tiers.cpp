#include "machine_internal.h"

/*
 * The two derived tiers: 256 bits, and the VEX prefix at 128.
 *
 * Neither is written out. Each row here is a form from machine_forms_packed.cpp with a stated set of
 * changes applied to it, because one statement of what an operation is - its opcode, its operands,
 * its flags effect, its width - is the only way the tiers cannot come to disagree about it.
 */

void MachineFormBuilder::registerWideForms() {
    /*
     * The 256-bit tier.
     *
     * Every form below is its 128-bit source with four changes and no fifth: a VEX prefix, `L`
     * set, every whole-register operand moved from ClassXmm128 to ClassYmm256, and - where the
     * source was two-address because the machine had no other spelling - the tie removed and
     * VEX.vvvv naming the operand the result used to be written over.
     *
     * Derived rather than written out, and that is the point rather than the economy. A wide
     * form that stated its own opcode could state a different one from its narrow twin, and the
     * failure would be an instruction that is *nearly* right at one width - the shape §5.6 keeps
     * finding. Here there is one statement of what `paddd` is and the wide row cannot disagree
     * with it.
     *
     * `kFeatureAvx2` on all of them, the float ones included, for the reason target.h gives: a
     * target with AVX and not AVX2 has a natural width of 16, so no 32-byte value is ever built
     * for one and a row it could reach would be a row nothing selects.
     */
    auto widenClass = [](Array<MachineOperandConstraint>& list) {
        // Only the whole-register class moves. A form's other operands are the same width at
        // either tier - the scalar a broadcast reads, the general register a lane extract
        // writes, the immediate a shift carries - and moving those would be describing a
        // different instruction rather than a wider one.
        for(auto& c: list) {
            if(c.regClass == ClassXmm128) c.regClass = ClassYmm256;
        }
    };

    auto wideTwin = [&](MachineFormId id, MachineFormId sourceId, StringView formName) -> MachineForm& {
        // A copy, taken before `add` below can move the array out from under a reference - the
        // same hazard `prefixedTwin` guards against, for the same reason.
        auto twin = forms[sourceId];
        auto& e = twin.encoding;

        e.prefixEncoding = PrefixEncoding::Vex;
        e.vectorLength = 1;

        widenClass(twin.uses);
        widenClass(twin.defs);

        /*
         * The tie, which VEX removes for every form that has a real encoding.
         *
         * Not for a pseudo. A pseudo's bytes are its own emitter's rather than the descriptor's,
         * so dropping the tie here would tell the allocator the destination is free while the
         * emitter went on writing the two-address sequence into it. The emitters take the width
         * and write VEX with `vvvv` naming the destination, which is the same operation the
         * legacy bytes performed - see genPackedTwoAddress.
         */
        if(e.family != EncodingFamily::Pseudo) dropTie(twin);

        twin.id = id;
        twin.name = formName;

        /*
         * Both, and `validateMachineForms` is what says so - the same rule §9.4 records the FMA
         * forms learning, arriving a second time and caught the same way.
         *
         * A form written with a vector prefix has to require the extension that *defines the
         * prefix* whatever else it also needs. AVX2 implies AVX on every part that has it and
         * `x64FeaturesFor` will not claim one without the other, but a form's features are what
         * selection reads, and a rule the settings happen to maintain is not the same as one the
         * form states. `kFeatureAvx2` alone passed every test in the default build.
         */
        twin.requiredFeatures |= kFeatureAvx | kFeatureAvx2;

        // The links are rebuilt rather than inherited, exactly as `prefixedTwin` rebuilds them:
        // a wide form's own memory twin would be derived from *it*, and its alternative is
        // nothing at all - a VEX-encoded form is already the last tier this backend writes.
        twin.memorySource = 0;
        twin.memorySourceOf = 0;
        twin.alternative = 0;
        twin.alternativeOf = 0;
        twin.wide = 0;
        twin.wideOf = sourceId;

        auto& form = add(id, twin.opcode, formName);
        form = twin;

        forms[sourceId].wide = id;
        return form;
    };

    wideTwin(FormVWideAdd8,   FormVAdd8,   "vpaddb ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideAdd16,  FormVAdd16,  "vpaddw ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideAdd32,  FormVAdd32,  "vpaddd ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideAdd64,  FormVAdd64,  "vpaddq ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideAddF32, FormVAddF32, "vaddps ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideAddF64, FormVAddF64, "vaddpd ymm, ymm, ymm/m"_v);

    wideTwin(FormVWideSub8,   FormVSub8,   "vpsubb ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideSub16,  FormVSub16,  "vpsubw ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideSub32,  FormVSub32,  "vpsubd ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideSub64,  FormVSub64,  "vpsubq ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideSubF32, FormVSubF32, "vsubps ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideSubF64, FormVSubF64, "vsubpd ymm, ymm, ymm/m"_v);

    // `vpmulld` and no wide twin of the SSE2 route beside it: AVX2 is above SSE4.1 on the one
    // ladder `x64FeaturesFor` reads, so a target that can hold a wide vector always has the one
    // instruction and `selectPackedForm` never asks for the seven-instruction expansion.
    wideTwin(FormVWideMul16,  FormVMul16,  "vpmullw ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideMul32,  FormVMul32,  "vpmulld ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideMulF32, FormVMulF32, "vmulps ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideMulF64, FormVMulF64, "vmulpd ymm, ymm, ymm/m"_v);

    wideTwin(FormVWideMulHi16,  FormVMulHi16,  "vpmulhuw ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideIMulHi16, FormVIMulHi16, "vpmulhw ymm, ymm, ymm/m"_v);

    wideTwin(FormVWideMulWide32,  FormVMulWide32,  "vpmuludq ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideIMulWide32, FormVIMulWide32, "vpmuldq ymm, ymm, ymm/m"_v);

    wideTwin(FormVWideDivF32, FormVDivF32, "vdivps ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideDivF64, FormVDivF64, "vdivpd ymm, ymm, ymm/m"_v);

    wideTwin(FormVWideAnd,    FormVAnd,    "vpand ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideOr,     FormVOr,     "vpor ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideXor,    FormVXor,    "vpxor ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideAndNot, FormVAndNot, "vpandn ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideAndF32, FormVAndF32, "vandps ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideAndF64, FormVAndF64, "vandpd ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideAndNotF32, FormVAndNotF32, "vandnps ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideAndNotF64, FormVAndNotF64, "vandnpd ymm, ymm, ymm/m"_v);

    // The count operand stays an xmm at this width - `vpsllw ymm, ymm, xmm` is what the machine
    // has, a shared count being one quadword whatever the vector is - and `widenClass` leaves it
    // alone for the same reason it leaves a broadcast's general register alone. What widens is
    // the vector, and the tie stays because a pseudo's bytes are its emitter's.
    wideTwin(FormVWideShlReg, FormVShlReg, "vmovd xmm11, r; vpsllw/d/q ymm, ymm, xmm11"_v);
    wideTwin(FormVWideShrReg, FormVShrReg, "vmovd xmm11, r; vpsrlw/d/q ymm, ymm, xmm11"_v);
    wideTwin(FormVWideSarReg, FormVSarReg, "vmovd xmm11, r; vpsraw/d ymm, ymm, xmm11"_v);

    wideTwin(FormVWideShlVar32, FormVShlVar32, "vpsllvd ymm, ymm, ymm"_v);
    wideTwin(FormVWideShlVar64, FormVShlVar64, "vpsllvq ymm, ymm, ymm"_v);
    wideTwin(FormVWideShrVar32, FormVShrVar32, "vpsrlvd ymm, ymm, ymm"_v);
    wideTwin(FormVWideShrVar64, FormVShrVar64, "vpsrlvq ymm, ymm, ymm"_v);
    wideTwin(FormVWideSarVar32, FormVSarVar32, "vpsravd ymm, ymm, ymm"_v);

    wideTwin(FormVWideShl16Imm, FormVShl16Imm, "vpsllw ymm, ymm, imm8"_v);
    wideTwin(FormVWideShl32Imm, FormVShl32Imm, "vpslld ymm, ymm, imm8"_v);
    wideTwin(FormVWideShl64Imm, FormVShl64Imm, "vpsllq ymm, ymm, imm8"_v);
    wideTwin(FormVWideShr16Imm, FormVShr16Imm, "vpsrlw ymm, ymm, imm8"_v);
    wideTwin(FormVWideShr32Imm, FormVShr32Imm, "vpsrld ymm, ymm, imm8"_v);
    wideTwin(FormVWideShr64Imm, FormVShr64Imm, "vpsrlq ymm, ymm, imm8"_v);
    wideTwin(FormVWideSar16Imm, FormVSar16Imm, "vpsraw ymm, ymm, imm8"_v);
    wideTwin(FormVWideSar32Imm, FormVSar32Imm, "vpsrad ymm, ymm, imm8"_v);

    wideTwin(FormVWideCmpEq8,  FormVCmpEq8,  "vpcmpeqb ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideCmpEq16, FormVCmpEq16, "vpcmpeqw ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideCmpEq32, FormVCmpEq32, "vpcmpeqd ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideCmpEq64, FormVCmpEq64, "vpcmpeqq ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideCmpGt8,  FormVCmpGt8,  "vpcmpgtb ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideCmpGt16, FormVCmpGt16, "vpcmpgtw ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideCmpGt32, FormVCmpGt32, "vpcmpgtd ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideCmpGt64, FormVCmpGt64, "vpcmpgtq ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideCmpF32,  FormVCmpF32,  "vcmpps ymm, ymm, ymm/m, predicate"_v);
    wideTwin(FormVWideCmpF64,  FormVCmpF64,  "vcmppd ymm, ymm, ymm/m, predicate"_v);

    // The eight saturating rows and `pmaddwd`, whose 256-bit twins are AVX2's.
    wideTwin(FormVWideSatAddI8,  FormVSatAddI8,  "vpaddsb ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideSatAddU8,  FormVSatAddU8,  "vpaddusb ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideSatAddI16, FormVSatAddI16, "vpaddsw ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideSatAddU16, FormVSatAddU16, "vpaddusw ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideSatSubI8,  FormVSatSubI8,  "vpsubsb ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideSatSubU8,  FormVSatSubU8,  "vpsubusb ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideSatSubI16, FormVSatSubI16, "vpsubsw ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideSatSubU16, FormVSatSubU16, "vpsubusw ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideMulAddWords, FormVMulAddWords, "vpmaddwd ymm, ymm, ymm/m"_v);

    // The minimum and the maximum, whose 256-bit integer rows are AVX2's own and whose float
    // ones are AVX's - stated as AVX2 with everything else here, a target below that having no
    // wide value to take the minimum of.
    wideTwin(FormVWideMinI8,  FormVMinI8,  "vpminsb ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideMinU8,  FormVMinU8,  "vpminub ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideMinI16, FormVMinI16, "vpminsw ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideMinU16, FormVMinU16, "vpminuw ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideMinI32, FormVMinI32, "vpminsd ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideMinU32, FormVMinU32, "vpminud ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideMinF32, FormVMinF32, "vminps ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideMinF64, FormVMinF64, "vminpd ymm, ymm, ymm/m"_v);

    wideTwin(FormVWideMaxI8,  FormVMaxI8,  "vpmaxsb ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideMaxU8,  FormVMaxU8,  "vpmaxub ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideMaxI16, FormVMaxI16, "vpmaxsw ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideMaxU16, FormVMaxU16, "vpmaxuw ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideMaxI32, FormVMaxI32, "vpmaxsd ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideMaxU32, FormVMaxU32, "vpmaxud ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideMaxF32, FormVMaxF32, "vmaxps ymm, ymm, ymm/m"_v);
    wideTwin(FormVWideMaxF64, FormVMaxF64, "vmaxpd ymm, ymm, ymm/m"_v);

    // The pseudos, which keep their ties and their clobber. What changes for each is the
    // width its emitter writes - see genPackedTwoAddress - and xmm11 becoming ymm11, which is
    // the same physical register named at the width the expansion now works at.
    wideTwin(FormVWideCmpInverted, FormVCmpInverted, "vpcmpXX; vpcmpeqd; vpxor (inverted)"_v);
    wideTwin(FormVWideNot,         FormVNot,         "vpcmpeqd; vpxor (complement)"_v);
    wideTwin(FormVWideSelect,      FormVSelect,      "vpblendvb ymm, ymm, ymm, mask (lanewise select)"_v);

    wideTwin(FormVWideNeg8,   FormVNeg8,   "vpxor; vpsubb (negate)"_v);
    wideTwin(FormVWideNeg16,  FormVNeg16,  "vpxor; vpsubw (negate)"_v);
    wideTwin(FormVWideNeg32,  FormVNeg32,  "vpxor; vpsubd (negate)"_v);
    wideTwin(FormVWideNeg64,  FormVNeg64,  "vpxor; vpsubq (negate)"_v);
    wideTwin(FormVWideNegF32, FormVNegF32, "vpcmpeqd; vpslld; vxorps (negate)"_v);
    wideTwin(FormVWideNegF64, FormVNegF64, "vpcmpeqd; vpsllq; vxorpd (negate)"_v);

    wideTwin(FormVWideAbs8,  FormVAbs8,  "vpabsb ymm, ymm/m"_v);
    wideTwin(FormVWideAbs16, FormVAbs16, "vpabsw ymm, ymm/m"_v);
    wideTwin(FormVWideAbs32, FormVAbs32, "vpabsd ymm, ymm/m"_v);

    /*
     * The shuffles, every one of which works *inside* each 128-bit half at this width.
     *
     * `vpshufd ymm` applies its one control byte to both halves independently, and the two
     * interleave families take their lanes from the corresponding half of each source. So these
     * forms encode a strictly smaller set of patterns than their names suggest, and
     * `packedShuffleChoice` is where that is enforced: a pattern that crosses the halves reaches
     * `FormVPerm2` or is refused, and a pattern that does not is one of these with the same
     * control byte the 128-bit form would have taken.
     */
    wideTwin(FormVWideShuffle32,       FormVShuffle32,       "vpshufd ymm, ymm, pattern"_v);
    wideTwin(FormVWideShuffle32Second, FormVShuffle32Second, "vpshufd ymm, ymm, pattern (second source)"_v);
    wideTwin(FormVWideShuffle2F32,     FormVShuffle2F32,     "vshufps ymm, ymm, ymm, pattern"_v);
    wideTwin(FormVWideShuffle2F64,     FormVShuffle2F64,     "vshufpd ymm, ymm, ymm, pattern"_v);

    wideTwin(FormVWideUnpackLow8,   FormVUnpackLow8,   "vpunpcklbw ymm, ymm, ymm"_v);
    wideTwin(FormVWideUnpackLow16,  FormVUnpackLow16,  "vpunpcklwd ymm, ymm, ymm"_v);
    wideTwin(FormVWideUnpackLow32,  FormVUnpackLow32,  "vpunpckldq ymm, ymm, ymm"_v);
    wideTwin(FormVWideUnpackLow64,  FormVUnpackLow64,  "vpunpcklqdq ymm, ymm, ymm"_v);
    wideTwin(FormVWideUnpackLowF32, FormVUnpackLowF32, "vunpcklps ymm, ymm, ymm"_v);
    wideTwin(FormVWideUnpackLowF64, FormVUnpackLowF64, "vunpcklpd ymm, ymm, ymm"_v);

    wideTwin(FormVWideUnpackHigh8,   FormVUnpackHigh8,   "vpunpckhbw ymm, ymm, ymm"_v);
    wideTwin(FormVWideUnpackHigh16,  FormVUnpackHigh16,  "vpunpckhwd ymm, ymm, ymm"_v);
    wideTwin(FormVWideUnpackHigh32,  FormVUnpackHigh32,  "vpunpckhdq ymm, ymm, ymm"_v);
    wideTwin(FormVWideUnpackHigh64,  FormVUnpackHigh64,  "vpunpckhqdq ymm, ymm, ymm"_v);
    wideTwin(FormVWideUnpackHighF32, FormVUnpackHighF32, "vunpckhps ymm, ymm, ymm"_v);
    wideTwin(FormVWideUnpackHighF64, FormVUnpackHighF64, "vunpckhpd ymm, ymm, ymm"_v);

    // `vpshufb ymm`, which is `pshufb` in each half with the control's matching half - so the same
    // in-lane restriction as the rest of this group, and `lowerByteLaneShuffles` writes each half's
    // control relative to its own half. The tie `dropTie` removes is the permuted vector's, which
    // becomes `vvvv`; the control stays the r/m operand.
    wideTwin(FormVWideByteShuffle, FormVByteShuffle, "vpshufb ymm, ymm, ymm/m"_v);

    wideTwin(FormVWideBitcast, FormVBitcast, "vmovaps ymm, ymm"_v);
    wideTwin(FormVWideMove,    FormVMove,    "vmovaps ymm, ymm"_v);

    // Eight lanes each side rather than four, which is the whole of what widens: the lane count
    // is preserved by a `Cast` between vectors and the register width follows it.
    wideTwin(FormVWideCastIToF32, FormVCastIToF32, "vcvtdq2ps ymm, ymm"_v);
    wideTwin(FormVWideCastFToI32, FormVCastFToI32, "vcvttps2dq ymm, ymm"_v);

    wideTwin(FormVWideSqrtF32, FormVSqrtF32, "vsqrtps ymm, ymm"_v);
    wideTwin(FormVWideSqrtF64, FormVSqrtF64, "vsqrtpd ymm, ymm"_v);
    wideTwin(FormVWideRoundF32, FormVRoundF32, "vroundps ymm, ymm, mode"_v);
    wideTwin(FormVWideRoundF64, FormVRoundF64, "vroundpd ymm, ymm, mode"_v);

    // The one pair whose source was already VEX-encoded and already three-operand, so what the
    // twin changes is `L` alone. `requiredFeatures` picks up AVX2 beside the AVX and FMA3 the
    // source already states, which is the rule §9.4 records: a form written with a vector prefix
    // requires the extension that defines the prefix *and* whatever else it needs.
    wideTwin(FormVWideFmaF32, FormVFmaF32, "vfmadd213ps ymm, ymm, ymm"_v);
    wideTwin(FormVWideFmaF64, FormVFmaF64, "vfmadd213pd ymm, ymm, ymm"_v);

    wideTwin(FormVWideLoad,     FormVLoad,     "vmovups ymm, [address]"_v);
    wideTwin(FormVWideLoadInt,  FormVLoadInt,  "vmovdqu ymm, [address]"_v);
    wideTwin(FormVWideStore,    FormVStore,    "vmovups [address], ymm"_v);
    wideTwin(FormVWideStoreInt, FormVStoreInt, "vmovdqu [address], ymm"_v);

    /*
     * The cross-half permute, which is the one instruction in this tier that moves bytes between
     * the two 128-bit halves of a register.
     *
     * `vperm2f128 ymm1, ymm2, ymm3, imm8` builds its result out of two of the four halves the
     * two sources hold: bits 1:0 choose the low half of the result and bits 5:4 the high one,
     * numbering the sources' halves 0-3 in order. Bits 3 and 7 zero the corresponding half
     * instead, which nothing here uses.
     *
     * Three-operand and non-destructive, like `pshufd` one tier down and for the same reason it
     * matters: the top level of a reduction butterfly is a cross-half swap, and a tie on it
     * would put a copy in every wide reduction.
     *
     * AVX rather than AVX2 - it is a float-domain permute and predates the integer tier - but it
     * is stated as AVX2 with everything else here, since a target below that has no wide value
     * to permute.
     */
    {
        auto& form = add(FormVPerm2, OpVShuffle, "vperm2f128 ymm, ymm, ymm, pattern"_v);
        form.uses.push(anyReg(ClassYmm256));
        form.uses.push(anyReg(ClassYmm256));
        form.defs.push(def(ClassYmm256));
        form.requiredFeatures = kFeatureAvx | kFeatureAvx2;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = 0x06,
            .escape = 0x0f, .prefix = 0x66,
            .regField = defRef(0), .rmField = useRef(1),
            .width = OperationWidth::FromResult,
            .widthInPrefix = true,
            .prefixEncoding = PrefixEncoding::Vex,
            .opcodeMap = kOpcodeMap0F3A,
            .vectorLength = 1,
        };
        form.encoding.vvvvField = useRef(0);
        form.encoding.patternImmediate = true;
    }

    /*
     * The general crossing, which is the other half of the answer `vperm2f128` starts.
     *
     * `vpermd ymm1, ymm2, ymm3/m256` writes result lane `i` with source lane `ymm2[i] & 7` - one
     * index per lane, in a register rather than in a byte, and no notion of a 128-bit half at
     * all. So every eight-lane 32-bit pattern over *one* source is this instruction, including
     * the ones that are neither in-lane nor a swap of whole halves.
     *
     * `vpermps` is the same operation in the float domain, and the domain follows the value
     * being permuted rather than the indices - which are an integer vector in both rows, a lane
     * index being a number whatever it is indexing.
     *
     * Three-operand and non-destructive, and the indices are `vvvv`: the operand order here is
     * the machine's, so use 0 is the index vector and use 1 is the vector being permuted. That
     * is the same order `LowerInstX86Permute` states, for the reason `X86MaskAnd` states its own
     * in the machine's order - a form and the pass that writes for it agreeing is cheaper than a
     * form that reorders.
     *
     * There is no 128-bit twin. `vpermilps` would be one, and a 128-bit 32-bit-lane pattern is
     * already `pshufd` in one instruction with no pooled constant at all - so the narrow tier
     * has nothing to gain and this family exists only at the width where the halves are a
     * problem.
     */
    auto permute = [&](MachineFormId id, StringView formName, U8 opcode) {
        auto& form = add(id, OpVPermute, formName);
        form.uses.push(anyReg(ClassYmm256));
        form.uses.push(anyReg(ClassYmm256));
        form.defs.push(def(ClassYmm256));
        form.requiredFeatures = kFeatureAvx | kFeatureAvx2;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = opcode,
            .escape = 0x0f, .prefix = 0x66,
            .regField = defRef(0), .rmField = useRef(1),
            .width = OperationWidth::FromResult,
            .widthInPrefix = true,
            .prefixEncoding = PrefixEncoding::Vex,
            .opcodeMap = kOpcodeMap0F38,
            .vectorLength = 1,
        };
        form.encoding.vvvvField = useRef(0);
    };

    permute(FormVPermute32,  "vpermd ymm, ymm, ymm/m"_v,  0x36);
    permute(FormVPermuteF32, "vpermps ymm, ymm, ymm/m"_v, 0x16);

    /*
     * The general crossing at a 64-bit lane, whose pattern *is* an immediate: `vpermq ymm1,
     * ymm2/m256, imm8` writes result lane `i` with source lane `(imm8 >> 2i) & 3`. So where `vpermd`
     * above needs its pattern made into a pooled vector first, this is a form like `pshufd` - and,
     * like `pshufd`, twice, the second row reading the other operand for a pattern that names only
     * the second source (`wideShuffleChoice` answers both, J236).
     *
     * One row serves every lane kind. `wideShuffleChoice` reads a byte, word or doubleword pattern
     * that moves aligned eight-byte groups as this one, and a float lane is permuted in the integer
     * domain the way `pshufd` permutes it one tier down - `vpermpd` would be a second pair of rows for
     * a forwarding cycle on some parts.
     *
     * `VEX.W1` is the lane width - `vpermq` and the W0 encoding of the same opcode are not the same
     * instruction - stated as `Fixed64`, which is how `vpsllvq` states its own. No 512-bit twin:
     * at that width the immediate permutes inside each 256-bit half, which is why `registerFullForms`
     * lists the opcode as structural.
     */
    auto permute64 = [&](MachineFormId id, StringView formName, bool second) {
        auto& form = add(id, OpVShuffle, formName);
        form.uses.push(anyReg(ClassYmm256));
        if(second) form.uses.push(anyReg(ClassYmm256));
        form.defs.push(def(ClassYmm256));
        form.requiredFeatures = kFeatureAvx | kFeatureAvx2;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = 0x00,
            .escape = 0x0f, .prefix = 0x66,
            .regField = defRef(0), .rmField = useRef(second ? 1 : 0),
            .width = OperationWidth::Fixed64,
            .prefixEncoding = PrefixEncoding::Vex,
            .opcodeMap = kOpcodeMap0F3A,
            .vectorLength = 1,
        };
        form.encoding.patternImmediate = true;
    };

    permute64(FormVPermute64,       "vpermq ymm, ymm, pattern"_v, false);
    permute64(FormVPermute64Second, "vpermq ymm, ymm, pattern (second source)"_v, true);

    /*
     * The two halves read on their own, which is how a 256-bit vector and a 128-bit one meet.
     *
     * A lane above the low half cannot be reached by any of the extracts one tier down - those
     * name a lane inside one register's worth of bytes - so `lowerLaneExtracts` brings the
     * wanted half down with `vextracti128` and then reads the lane out of it with the machinery
     * that already exists. `vinserti128` is the mirror, and is also what puts a 128-bit value
     * into the upper half of a wide one.
     *
     * The extract's destination is a 128-bit class and its source a 256-bit one, which is the
     * one place in this table where a form's two ends are different vector classes. Both name
     * the same physical registers, so the allocator will usually put them in one - and with
     * `omitWhenSame` the index-zero extract then emits nothing at all, which is exactly what
     * "the low half of a ymm *is* the xmm" should cost.
     */
    {
        auto& form = add(FormVExtract128, OpVExtract, "vextracti128 xmm, ymm, half"_v);
        form.uses.push(anyReg(ClassYmm256));
        form.defs.push(def(ClassXmm128));
        form.requiredFeatures = kFeatureAvx | kFeatureAvx2;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = 0x39,

            // The destination is the r/m operand and the source is ModRM.reg, which is the
            // opposite way round from most of this table and is what the extract direction
            // encodes - the same shape `pextrd` has one tier down.
            .escape = 0x0f, .prefix = 0x66,
            .regField = useRef(0), .rmField = defRef(0),
            .width = OperationWidth::FromUse0,
            .widthInPrefix = true,
            .prefixEncoding = PrefixEncoding::Vex,
            .opcodeMap = kOpcodeMap0F3A,
            .vectorLength = 1,
        };
        form.encoding.patternImmediate = true;
    }

    {
        auto& form = add(FormVInsert128, OpVInsert, "vinserti128 ymm, ymm, xmm, half"_v);
        form.uses.push(anyReg(ClassYmm256));
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(def(ClassYmm256));
        form.requiredFeatures = kFeatureAvx | kFeatureAvx2;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = 0x38,
            .escape = 0x0f, .prefix = 0x66,
            .regField = defRef(0), .rmField = useRef(1),
            .width = OperationWidth::FromResult,
            .widthInPrefix = true,
            .prefixEncoding = PrefixEncoding::Vex,
            .opcodeMap = kOpcodeMap0F3A,
            .vectorLength = 1,
        };
        form.encoding.vvvvField = useRef(0);
        form.encoding.patternImmediate = true;
    }

    /*
     * Every lane the same scalar, at 256 bits.
     *
     * Not a twin of the narrow broadcast, because the instruction is a different one rather than
     * a wider spelling: the 128-bit route is `pshufd`, which is in-lane and so cannot reach the
     * upper half at all. AVX2's `vpbroadcastd`/`vpbroadcastq` and `vbroadcastss`/`vbroadcastsd`
     * take lane zero of an xmm and fill a ymm with it, which is the whole operation in one
     * instruction and no scratch.
     *
     * Still a pseudo, and for the reason the narrow one is: an integer lane's scalar is in a
     * general register and has to cross banks first, which is `movd`/`movq` and is decided by
     * the operand's class rather than by anything in the encoding.
     */
    auto wideBroadcast = [&](MachineFormId id, StringView formName, RegisterClassId source) {
        auto& form = add(id, OpVBroadcast, formName);
        form.uses.push(anyReg(source));
        form.defs.push(def(ClassYmm256));
        form.requiredFeatures = kFeatureAvx | kFeatureAvx2;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::VecBroadcast,
        };
    };

    // The narrow pair, which at this width exists only under AVX2 - as does the 32-byte vector
    // they fill, so there is no second row for a target without it.
    wideBroadcast(FormVWideBroadcast8,   "vmovd xmm, r32; vpbroadcastb ymm, xmm"_v,  ClassGpr32);
    wideBroadcast(FormVWideBroadcast16,  "vmovd xmm, r32; vpbroadcastw ymm, xmm"_v,  ClassGpr32);
    wideBroadcast(FormVWideBroadcast32,  "vmovd xmm, r32; vpbroadcastd ymm, xmm"_v,  ClassGpr32);
    wideBroadcast(FormVWideBroadcast64,  "vmovq xmm, r64; vpbroadcastq ymm, xmm"_v,  ClassGpr64);
    wideBroadcast(FormVWideBroadcastF32, "vbroadcastss ymm, xmm"_v, ClassFloat32);
    wideBroadcast(FormVWideBroadcastF64, "vbroadcastsd ymm, xmm"_v, ClassFloat64);

    forms[FormVBroadcast8].wide = FormVWideBroadcast8;
    forms[FormVBroadcast16].wide = FormVWideBroadcast16;
    forms[FormVBroadcast32].wide = FormVWideBroadcast32;
    forms[FormVBroadcast64].wide = FormVWideBroadcast64;
    forms[FormVBroadcastF32].wide = FormVWideBroadcastF32;
    forms[FormVBroadcastF64].wide = FormVWideBroadcastF64;

    forms[FormVWideBroadcast8].wideOf = FormVBroadcast8;
    forms[FormVWideBroadcast16].wideOf = FormVBroadcast16;
    forms[FormVWideBroadcast32].wideOf = FormVBroadcast32;
    forms[FormVWideBroadcast64].wideOf = FormVBroadcast64;
    forms[FormVWideBroadcastF32].wideOf = FormVBroadcastF32;
    forms[FormVWideBroadcastF64].wideOf = FormVBroadcastF64;

    /*
     * A lane out of, and a lane into, a 256-bit vector - see PseudoKind::VecWideLane.
     *
     * The clobber is declared on three of the four and is what the expansion holds the wanted
     * half in. The float *extract* is the exception: its destination is a vector register of its
     * own, so the half can be brought down straight into it and there is no third register in
     * the sequence at all.
     *
     * The insert's result is **not tied** to its vector operand, which every 128-bit insert's is
     * and which is worth stating: `vinserti128` is three-operand, so the half that was not
     * written comes from the source and the destination need not be it. That removes the copy
     * the narrow tier pays whenever the vector is still live afterwards - so a chain of inserts,
     * which is what `iota` is, is shorter at 256 bits than at 128.
     */
    auto wideExtract = [&](MachineFormId id, StringView formName, RegisterClassId cls, bool scratch) {
        auto& form = add(id, OpVExtract, formName);
        form.uses.push(anyReg(ClassYmm256));
        form.defs.push(def(cls));
        form.requiredFeatures = kFeatureAvx | kFeatureAvx2;
        if(scratch) form.clobbers.add(vectorReg(kVectorScratch));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::VecWideLane,
        };
    };

    wideExtract(FormVWideExtract32,  "vextracti128; vpextrb/w/d r32, xmm, lane"_v, ClassGpr32, true);
    wideExtract(FormVWideExtract64,  "vextracti128; vpextrq r64, xmm, lane"_v, ClassGpr64, true);
    wideExtract(FormVWideExtractF32, "vextractf128; vpshufd xmm, xmm, lane"_v, ClassFloat32, false);
    wideExtract(FormVWideExtractF64, "vextractf128; vpshufd xmm, xmm, lane"_v, ClassFloat64, false);

    // `vpmovmskb r32, ymm` - the one wide row that is a real encoding rather than a pseudo,
    // because AVX2 widened the instruction itself: thirty-two bytes in, thirty-two bits out, and
    // the general register it writes is the same width at either tier.
    wideTwin(FormVWideMaskBits, FormVMaskBits, "vpmovmskb r32, ymm"_v);
    wideTwin(FormVWideMaskBitsF32, FormVMaskBitsF32, "vmovmskps r32, ymm"_v);
    wideTwin(FormVWideMaskBitsF64, FormVMaskBitsF64, "vmovmskpd r32, ymm"_v);

    auto wideInsert = [&](MachineFormId id, StringView formName, RegisterClassId cls) {
        auto& form = add(id, OpVInsert, formName);
        form.uses.push(anyReg(ClassYmm256));
        form.uses.push(anyReg(cls));
        form.defs.push(def(ClassYmm256));
        form.requiredFeatures = kFeatureAvx | kFeatureAvx2;
        form.clobbers.add(vectorReg(kVectorScratch));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::VecWideWithLane,
        };
    };

    wideInsert(FormVWideInsert32,  "vpinsrd; vinserti128 (vextracti128 for the upper half)"_v,  ClassGpr32);
    wideInsert(FormVWideInsert64,  "vpinsrq; vinserti128 (vextracti128 for the upper half)"_v,  ClassGpr64);
    wideInsert(FormVWideInsertF32, "vinsertps; vinserti128 (vextracti128 for the upper half)"_v, ClassFloat32);
    wideInsert(FormVWideInsertF64, "vmovsd/vunpcklpd; vinserti128 (vextracti128 for the upper half)"_v, ClassFloat64);

    /*
     * VAES and VPCLMULQDQ: the round and the multiply over each 128-bit half of a ymm register.
     * The VEX.256 spelling is the extension's own - AVX2 is not what defines it, and a part with
     * AVX2 and no VAES (every Zen before 3, Haswell to Comet Lake) has no such instruction - so the
     * twin requires the flag on top of what `wideTwin` states.
     */
    auto wideCrypto = [&](MachineFormId id, MachineFormId sourceId, StringView formName, FeatureSet feature) {
        wideTwin(id, sourceId, formName).requiredFeatures |= feature;
    };

    wideCrypto(FormVWideAesEnc,     FormAesEnc,     "vaesenc ymm, ymm, ymm/m"_v,               kFeatureVaes);
    wideCrypto(FormVWideAesEncLast, FormAesEncLast, "vaesenclast ymm, ymm, ymm/m"_v,           kFeatureVaes);
    wideCrypto(FormVWideAesDec,     FormAesDec,     "vaesdec ymm, ymm, ymm/m"_v,               kFeatureVaes);
    wideCrypto(FormVWideAesDecLast, FormAesDecLast, "vaesdeclast ymm, ymm, ymm/m"_v,           kFeatureVaes);
    wideCrypto(FormVWideClmul,      FormClmul,      "vpclmulqdq ymm, ymm, ymm/m, selector"_v,  kFeatureVpclmul);
}

void MachineFormBuilder::registerVexTier() {
    /*
     * The VEX tier at 128 bits.
     *
     * Every form above that touches a vector register and is spelled as a legacy SSE encoding gets a
     * twin here with a VEX prefix on it, and selection takes the twin wherever the target has AVX -
     * so a build with the extension emits no legacy vector instruction at all.
     *
     * That last sentence is the point, and it is not about the byte the prefix costs or saves.
     *
     *  - **A legacy SSE instruction that writes an xmm register leaves bits 128 and above
     *    unchanged**, which is a partial write of a register a `ymm` value may be living in the top
     *    of. A processor whose upper halves are dirty pays for that: a save and a restore on the
     *    parts §5.4 was written for, and on everything since a merging uop with a false dependency
     *    on the half that was not written. A VEX-encoded write *zeroes* those bits instead, so code
     *    that is VEX throughout cannot be in the state that costs anything. The instruction that
     *    clears it is then owed at foreign boundaries alone, which is where emitVectorZeroUpper
     *    puts it.
     *  - **Three operands.** Every two-address form here loses its tie (see `dropTie`), and with it
     *    the copy the allocator was putting in front of every packed operation whose result outlives
     *    one of its sources. That is the same win the wide tier records and it is larger down here,
     *    the 128-bit tier being what most vector code is.
     *
     * Derived rather than written out, for the reason the wide tier is: one statement of what
     * `paddd` is, and a row that cannot disagree with it. Written as a *sweep* rather than as a list
     * of which forms widen, though, because the answer here is "all of them" - a legacy vector form
     * left off a list would be one nothing selects the twin of, and it would be invisible in exactly
     * the way this whole tier exists to prevent. `validateNoLegacyVectorForms` is the assertion that
     * the sweep reached everything.
     *
     * The ids are taken from the end of the table rather than named in the enum, as `addIntrinsics`
     * takes its own: nothing reaches one of these by name. Selection finds them through
     * `alternative` and everything downstream reads them as it reads their sources.
     */
    {
        auto isVectorClass = [](RegisterClassId regClass) {
            return regClass == ClassFloat32 || regClass == ClassFloat64 || regClass == ClassXmm128;
        };

        // A constraint's class means nothing for the kinds that occupy no register, and the default
        // it carries is a general one - so this asks the kind first rather than trusting the field.
        auto occupiesVector = [&](const MachineOperandConstraint& c) {
            switch(c.kind) {
                case OperandConstraintKind::Register:
                case OperandConstraintKind::FixedRegister:
                case OperandConstraintKind::RegisterSubset:
                case OperandConstraintKind::RegisterOrMemory:
                    return isVectorClass(c.regClass);
                default:
                    return false;
            }
        };

        auto touchesVector = [&](const MachineForm& form) {
            for(auto& c: form.uses) if(occupiesVector(c)) return true;
            for(auto& c: form.defs) if(occupiesVector(c)) return true;
            return false;
        };

        /*
         * Whether a form's VEX twin can read its operand from an address the *program* computed,
         * where the legacy form could not.
         *
         * This is the alignment rule and nothing else. A legacy-encoded packed operation faults on a
         * memory operand that is not 16-byte aligned, and a `Slot128` is aligned by construction
         * where an address the program worked out is not - so `packed` takes an operand from a frame
         * slot and `memoryTwin` is deliberately never called for it (see the table above). A
         * VEX-encoded packed operation has no alignment requirement at all, which removes the only
         * reason the twin did not exist.
         *
         * Asked of the memory operand's *class*, so it means "the refusal was about alignment" and
         * not "the form has an operand in memory". A form whose memory-capable operand is a general
         * register was never refused for this reason and gets no twin it did not have.
         */
        auto foldableUnderVex = [&](const MachineForm& form) {
            auto memory = form.memoryUse();
            if(memory < 0) return false;

            // Every vector class and not the three narrow ones, because this is asked of a wide form
            // as well: `ClassYmm256` is the operand class a 256-bit packed operation reads its
            // address through, and the alignment rule it is free of is the same rule.
            auto regClass = form.uses[memory].regClass;
            return isVectorClass(regClass) || regClass == ClassYmm256 || regClass == ClassZmm512;
        };

        /*
         * Which forms are skipped, and why each one is not a hole:
         *
         *  - a form that is already prefixed, or that already has an alternative built for it by
         *    hand (the scalar arithmetic and the comparisons, which carry an EVEX tier above the
         *    VEX one that this sweep has no way to build), or that *is* an alternative.
         *  - a memory twin, which is derived from the VEX form of its own register source below
         *    rather than from itself - that is the order `selectForm` states, and building it here
         *    as well would give one operation two VEX spellings.
         *  - a pseudo, whose bytes are its own emitter's rather than this descriptor's. Those choose
         *    their spelling from the target directly - see `vexPacked` in gen.cpp - because a
         *    pseudo's operand constraints do not change with the prefix and a second form would
         *    describe the same allocation twice.
         *  - a form that emits nothing at all, and a form with no vector operand, neither of which
         *    can execute as a legacy SSE instruction.
         */
        auto needsVexTwin = [&](const MachineForm& form) {
            if(form.encoding.prefixEncoding != PrefixEncoding::Legacy) return false;
            if(form.legacyOnly) return false;  // no VEX encoding exists - see MachineForm::legacyOnly
            if(form.alternative != 0 || form.alternativeOf != 0) return false;
            if(form.memorySourceOf != 0) return false;
            if(form.encoding.family == EncodingFamily::None) return false;
            if(form.encoding.family == EncodingFamily::Pseudo) return false;
            return touchesVector(form);
        };

        // The names, built before anything points into the buffer they live in. One `v` per twin and
        // the source name after it, twice over for the memory twin a form may also need.
        Size nameBytes = 0;
        Size twinCount = 0;

        // The suffix a twin built by the second rule carries, there being no legacy memory form to
        // take a name from: `vpaddd xmm, xmm/m` and `vpaddd xmm, xmm/m (folded)` are two forms and a
        // diagnostic naming one of them has to say which.
        auto folded = " (folded)"_v;

        for(Size i = 0; i < kMachineFormCount; i++) {
            if(!needsVexTwin(forms[i])) continue;

            nameBytes += forms[i].name.length + 1;
            twinCount++;

            if(auto memory = forms[i].memorySource) {
                nameBytes += forms[memory].name.length + 1;
                twinCount++;
            } else if(foldableUnderVex(forms[i])) {
                nameBytes += forms[i].name.length + 1 + folded.length;
                twinCount++;
            }
        }

        derivedNames.reserve(U32(nameBytes));
        forms.reserve(U32(forms.size() + twinCount));

        auto vexName = [&](StringView source, StringView suffix = StringView {}) {
            auto at = derivedNames.size();
            derivedNames.push('v');
            for(Size i = 0; i < source.length; i++) derivedNames.push(source.ptr[i]);
            for(Size i = 0; i < suffix.length; i++) derivedNames.push(suffix.ptr[i]);

            return StringView { &derivedNames[at], source.length + 1 + suffix.length };
        };

        // A snapshot, because the loop appends to the very table it walks: a twin is not itself a
        // source, and a sweep that saw its own output would ask for the VEX form of a VEX form.
        auto described = forms.size();

        for(Size i = 0; i < described; i++) {
            if(!needsVexTwin(forms[i])) continue;

            // Read out before the twin is added: `add` may move the array, and a reference into it
            // taken before that is a reference into the block it used to occupy.
            auto sourceId = MachineFormId(i);
            auto sourceName = forms[i].name;
            auto memorySource = forms[i].memorySource;
            auto memoryName = memorySource ? forms[memorySource].name : StringView {};
            auto twoAddress = forms[i].tiedResult() >= 0;
            auto opensFold = !memorySource && foldableUnderVex(forms[i]);

            auto vexId = MachineFormId(forms.size());
            prefixedTwin(vexId, sourceId, vexName(sourceName), twoAddress, PrefixEncoding::Vex, kFeatureAvx);

            /*
             * And the folded-address twin of the VEX form, where the legacy form had one.
             *
             * Built from the VEX form so that the three-operand shape and the folded address come
             * from one place, with the legacy memory form pointed at it as well - which is what
             * makes the two swaps `selectForm` performs commute, whichever order it applies them in.
             */
            if(memorySource) {
                auto memoryId = MachineFormId(forms.size());
                memoryTwin(memoryId, vexId, vexName(memoryName)).alternativeOf = memorySource;
                forms[memorySource].alternative = memoryId;
            } else if(opensFold) {
                /*
                 * And the twin that exists on this tier alone, the legacy form having been refused
                 * one by the alignment rule - see `foldableUnderVex`.
                 *
                 * No `alternativeOf` and no link back, because there is no legacy form for this to
                 * be an alternative *to*: a packed operation reading an address is a shape that
                 * simply does not exist below AVX. That is what makes the fold target-dependent
                 * without a feature test anywhere near it - `tryFoldLoad` asks the form `selectForm`
                 * would choose, which on a target without the extension is the legacy one with no
                 * memory source, so the fold declines itself.
                 *
                 * It also makes `selectForm`'s two swaps stop commuting, and the comment there says
                 * so: the alternative has to be applied before the memory twin, because for these
                 * the memory twin is only reachable through it.
                 */
                auto memoryId = MachineFormId(forms.size());
                memoryTwin(memoryId, vexId, vexName(sourceName, folded));
            }
        }

        /*
         * And the same opening at 256 bits.
         *
         * A wide form is already VEX, so the sweep above passes over it - but its memory twin is
         * missing for a *different* reason and the reason has run out too: `wideTwin` rebuilds every
         * link from scratch rather than inheriting one, on the grounds that a wide form's twin would
         * have to be derived from *it* rather than from a narrow form's. That derivation is this,
         * and without it the fold would be a thing the 128-bit tier does and the 256-bit tier
         * silently does not, which is the asymmetry a reader trips over rather than a decision.
         *
         * No `alternative` link again, for the reason the narrow ones have none: nothing below VEX
         * can encode a packed operation reading an address, so there is no legacy form this is an
         * alternative to.
         */
        Size wideNameBytes = 0;
        Size wideCount = 0;

        for(Size i = 0; i < described; i++) {
            auto& form = forms[i];
            if(!form.wideOf || form.memorySource || !foldableUnderVex(form)) continue;

            wideNameBytes += form.name.length + folded.length;
            wideCount++;
        }

        derivedNames.reserve(U32(derivedNames.size() + wideNameBytes));
        forms.reserve(U32(forms.size() + wideCount));

        auto wideName = [&](StringView source) {
            auto at = derivedNames.size();
            for(Size i = 0; i < source.length; i++) derivedNames.push(source.ptr[i]);
            for(Size i = 0; i < folded.length; i++) derivedNames.push(folded.ptr[i]);

            return StringView { &derivedNames[at], source.length + folded.length };
        };

        for(Size i = 0; i < described; i++) {
            if(!forms[i].wideOf || forms[i].memorySource || !foldableUnderVex(forms[i])) continue;

            auto sourceId = MachineFormId(i);
            auto sourceName = forms[i].name;

            memoryTwin(MachineFormId(forms.size()), sourceId, wideName(sourceName));
        }

        assertTrue(derivedNames.size() == nameBytes + wideNameBytes); // the name buffer moved under the views into it
    }

}

/*
 * The 512-bit tier - Analysis-Avx512.md §1.2's stage 2, and step 10 of its order of work.
 *
 * Every 256-bit form with an EVEX spelling, one tier up: `L'L` = 2 and every ymm operand a zmm, and
 * nothing else. Derived for the reason `wideTwin` is - one statement of what `vpaddd` is, so the
 * widths cannot disagree - and from the 256-bit form rather than the 128-bit one because that is the
 * form already three-operand, with its tie gone and its fold built. `writeVectorPrefix` writes EVEX
 * for the length alone, re-spelling a VEX form through `evexSpellingOf` as it does one naming xmm16.
 *
 * Linked as `full` from the 256-bit form, and so reached by `widthForm` from the 128-bit row every
 * selection names. What has no twin is left to `splitFullVectors`, which runs the operation as two
 * halves before selection could look for one:
 *
 *  - a pseudo, whose bytes are its emitter's rather than the descriptor's - each is taken up by hand
 *    where it is worth a 512-bit spelling, and split where it is not;
 *  - a VEX instruction EVEX has no identical spelling of: `vpblendvb`, `vpmovmskb`, a comparison into
 *    a vector, `vptest`. None has a place at this width, where every mask is in a mask register;
 *  - an instruction whose meaning is the 128-bit lanes of a 256-bit register rather than an operation
 *    on it - `vpermq` by an immediate permutes within each 256-bit half at 512 bits, `vinserti128`
 *    names one of two lanes where there are now four - and a conversion between lane counts, whose
 *    narrower operand would have to widen as well, which is a different instruction and not this one.
 *
 * Mask operands keep `ClassMask32`. The two mask classes differ in how a register is moved and
 * spilled, which is the *value's* class and `classForType`'s answer; an instruction reads a `k`
 * register at its opcode's own width, so a byte comparison at 512 bits writes all sixty-four bits
 * whatever its operand is called here. The k-register operations whose width *is* the opcode -
 * `kandq`, `knotq`, `kortestq`, `kmovq` - are rows of their own (registerEvexForms).
 */
void MachineFormBuilder::registerFullForms() {
    auto structural = [](const EncodingDescriptor& e) {
        if(e.opcodeMap == kOpcodeMap0F3A) {
            switch(e.opcode) {
                case 0x00: case 0x01:                   // vpermq/vpermpd by an immediate
                case 0x06: case 0x46:                   // vperm2f128/vperm2i128
                case 0x18: case 0x19: case 0x38: case 0x39: // the 128-bit inserts and extracts
                    return true;
                default:
                    return false;
            }
        }

        if(e.opcodeMap == kOpcodeMap0F38) {
            // The 128-bit broadcasts, and the zero and sign extensions between lane counts.
            if(e.opcode == 0x1a || e.opcode == 0x5a) return true;
            if((e.opcode >= 0x20 && e.opcode <= 0x25) || (e.opcode >= 0x30 && e.opcode <= 0x35)) return true;
        }

        return false;
    };

    // The broadcasts read an element out of an xmm at every width, which is the one shape where a
    // narrow vector operand beside a wide one is the same instruction one tier up.
    auto broadcast = [](const EncodingDescriptor& e) {
        if(e.opcodeMap != kOpcodeMap0F38) return false;
        return e.opcode == 0x18 || e.opcode == 0x19 || e.opcode == 0x58 || e.opcode == 0x59
            || e.opcode == 0x78 || e.opcode == 0x79;
    };

    auto spelled = [&](const MachineForm& form) {
        auto& e = form.encoding;
        if(e.family == EncodingFamily::None || e.family == EncodingFamily::Pseudo) return false;
        if(e.prefixEncoding == PrefixEncoding::Legacy) return false;
        if(structural(e)) return false;

        auto narrow = false;
        auto wide = false;
        for(auto& c: form.uses) { narrow = narrow || c.regClass == ClassXmm128; wide = wide || c.regClass == ClassYmm256; }
        for(auto& c: form.defs) { narrow = narrow || c.regClass == ClassXmm128; wide = wide || c.regClass == ClassYmm256; }
        if(!wide || (narrow && !broadcast(e))) return false;

        if(e.prefixEncoding == PrefixEncoding::Evex) return true;
        if(!evexSpellingOf(e.prefix, e.opcodeMap, e.opcode, e.extension).exists) return false;
        return e.opcodeAlt == 0 || evexSpellingOf(e.prefix, e.opcodeMap, e.opcodeAlt, e.extension).exists;
    };

    // The 256-bit register forms, and their masked twins - a memory twin is derived from its own
    // register form's twin below, so the two swaps `selectForm` makes still commute.
    auto eligible = [&](Size i) {
        auto& form = forms[i];
        return form.wideOf != 0 && form.memorySourceOf == 0 && spelled(form);
    };

    /*
     * The pseudos taken up by hand - each emitter reads its width from the type (`vectorLengthOf`)
     * and has a 512-bit spelling of its sequence: the two constants with no pool, the complement and
     * the negation (`vpternlogd` stands in for the all-ones `vpcmpeqd` that no longer answers a
     * vector), a shift by a count in a general register, and the broadcast out of a lane.
     */
    static const MachineFormId kPseudos[] = {
        FormVWideZero, FormVWideOnes, FormVWideNot,
        FormVWideNeg8, FormVWideNeg16, FormVWideNeg32, FormVWideNeg64, FormVWideNegF32, FormVWideNegF64,
        FormVWideShlReg, FormVWideShrReg, FormVWideSarReg,
        FormVWideBroadcast8, FormVWideBroadcast16, FormVWideBroadcast32, FormVWideBroadcast64,
        FormVWideBroadcastF32, FormVWideBroadcastF64,
    };

    auto pseudo = [&](Size i) {
        for(auto id: kPseudos) if(id == i) return true;
        return false;
    };

    auto described = forms.size();
    Size count = 0;
    Size nameBytes = 0;

    auto reserveFor = [&](MachineFormId id) {
        count++;
        nameBytes += forms[id].name.length;
    };

    for(auto id: kPseudos) reserveFor(id);

    for(Size i = 0; i < described; i++) {
        if(!eligible(i)) continue;

        reserveFor(MachineFormId(i));
        if(auto memory = forms[i].memorySource) reserveFor(memory);
        if(auto masked = forms[i].masked) reserveFor(masked);
    }

    forms.reserve(U32(forms.size() + count));
    derivedNames.reserve(U32(derivedNames.size() + nameBytes));

    // The source's name with every `ymm` a `zmm`, which is the whole of what the twin changes.
    auto fullName = [&](StringView source) {
        auto at = derivedNames.size();

        for(Size k = 0; k < source.length; k++) {
            auto c = source.ptr[k];
            if(c == 'y' && k + 2 < source.length && source.ptr[k + 1] == 'm' && source.ptr[k + 2] == 'm') c = 'z';
            derivedNames.push(c);
        }

        return StringView { &derivedNames[at], source.length };
    };

    auto twin = [&](MachineFormId sourceId) -> MachineFormId {
        auto copy = forms[sourceId];
        auto name = fullName(copy.name);

        for(auto& c: copy.uses) if(c.regClass == ClassYmm256) c.regClass = ClassZmm512;
        for(auto& c: copy.defs) if(c.regClass == ClassYmm256) c.regClass = ClassZmm512;

        // A pseudo's bytes are its emitter's, which reads the width from the type; its descriptor
        // states no length at any width.
        if(copy.encoding.family != EncodingFamily::Pseudo) copy.encoding.vectorLength = 2;

        // Every extension of the level, for `hasEvexRow`'s reason: they move together, and asking for
        // the most is the direction that cannot admit an instruction the build would not decode.
        copy.requiredFeatures |= kFeatureAvx512f | kFeatureAvx512bw | kFeatureAvx512dq;

        auto id = MachineFormId(forms.size());
        copy.id = id;
        copy.name = name;
        copy.alternative = 0;
        copy.alternativeOf = 0;
        copy.memorySource = 0;
        copy.memorySourceOf = 0;
        copy.wide = 0;
        copy.wideOf = 0;
        copy.full = 0;
        copy.fullOf = sourceId;
        copy.masked = 0;

        auto& form = add(id, copy.opcode, name);
        form = copy;
        forms[sourceId].full = id;
        return id;
    };

    for(auto id: kPseudos) {
        assertTrue(forms[id].encoding.family == EncodingFamily::Pseudo && !eligible(id));
        twin(id);
    }

    for(Size i = 0; i < described; i++) {
        if(!eligible(i)) continue;
        assertTrue(!pseudo(i));

        auto sourceId = MachineFormId(i);
        auto memory = forms[i].memorySource;
        auto masked = forms[i].masked;

        auto full = twin(sourceId);

        if(memory) {
            auto memoryName = fullName(forms[memory].name);
            memoryTwin(MachineFormId(forms.size()), full, memoryName);
        }

        if(masked) forms[full].masked = twin(masked);
    }

    assertTrue(forms.size() == described + count); // a twin reserved for and not built, or the reverse
}
