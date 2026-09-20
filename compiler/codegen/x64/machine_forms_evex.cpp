#include "machine_internal.h"

/*
 * The rows only AVX-512 has an instruction for - Analysis-Avx512.md §2.4, and step 3 of its order of
 * work.
 *
 * Every operation below already had an answer at `sse4` and `avx2`, and that answer is an expansion
 * rather than a form: `expandVectorAbs` writes a quadword magnitude as a comparison and a select,
 * `expandQuadwordSar` builds the arithmetic shift out of the logical one and a bias, `expandQuadwordMul`
 * spends three widening products, `expandVectorRotate` two shifts and an `or`, `scalarizeVectorLanes`
 * a word shift per lane one lane at a time, `expandBankConversions` an unsigned conversion as a halving
 * and a doubling. What these rows add is the one instruction each of those stood in for, and what makes
 * a build take the row is that the expansion declines itself where the target has it - so the table
 * says only that the instruction exists, and the sse4/avx2 code is byte for byte what it was.
 *
 * **EVEX by construction.** None of these has a VEX spelling, so each states its own EVEX.W and needs
 * the prefix whatever register it lands in - `writeVectorPrefix` in gen.cpp takes an EVEX form as
 * given. The §2.4 rows are register-only for now: each has a memory form, and none sits where a load
 * feeds it. The comparisons into a mask register, the zero-masking move and `vpternlogd` below do read
 * an address, each stating its `disp8*N` tuple.
 *
 * Each 128-bit row has a 256-bit twin linked through `MachineForm::wide`, for the reason every packed
 * row has one: selection asks for a width by the type, and never falls back.
 */

void MachineFormBuilder::registerEvexForms() {
    // Every row here is VL at 128 and 256 bits, and the level has every extension of AVX-512 it
    // names - see kFeatureAvx512 in target.h. A row states the extension that defines its instruction
    // beside the two every one of them needs.
    auto features = [](FeatureSet extension) {
        return kFeatureAvx512f | kFeatureAvx512vl | extension;
    };

    auto evexRegRm = [](U8 prefix, U8 map, U8 opcode, bool w) {
        auto e = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = opcode,
            .escape = 0x0f, .prefix = prefix,
            .regField = defRef(0), .rmField = useRef(0),
            .width = OperationWidth::FromResult,
            .widthInPrefix = true,
            .prefixEncoding = PrefixEncoding::Evex,
            .opcodeMap = map,
        };

        e.evexW = w;
        return e;
    };

    /*
     * The 256-bit twin of the row just added: the same descriptor with `L` set and every whole-register
     * operand a class wider. `wideTwin` in machine_forms_tiers.cpp is the same derivation for the VEX
     * forms; this one has no tie to drop, every row here being three-operand already.
     */
    auto widen = [&](MachineFormId id, MachineFormId narrowId, StringView formName) {
        auto twin = forms[narrowId];

        for(auto& c: twin.uses) if(c.regClass == ClassXmm128) c.regClass = ClassYmm256;
        for(auto& c: twin.defs) if(c.regClass == ClassXmm128) c.regClass = ClassYmm256;

        twin.encoding.vectorLength = 1;
        twin.id = id;
        twin.name = formName;
        twin.wide = 0;
        twin.wideOf = narrowId;

        auto& form = add(id, twin.opcode, formName);
        form = twin;
        forms[narrowId].wide = id;
    };

    /*
     * `vpabsq` - EVEX.66.0F38.W1 1F. The one integer lane `pabsb`, `pabsw` and `pabsd` left out, and at
     * `avx2` a comparison against zero, a subtraction and a select.
     */
    {
        auto& form = add(FormVAbs64, OpVAbs, "vpabsq xmm, xmm"_v);
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(def(ClassXmm128));
        form.requiredFeatures = features(0);
        form.encoding = evexRegRm(0x66, kOpcodeMap0F38, 0x1f, true);
    }
    widen(FormVWideAbs64, FormVAbs64, "vpabsq ymm, ymm"_v);

    /*
     * `vpsraq` by a constant - EVEX.66.0F.W1 72 /4 ib. The same group-13 opcode and extension `psrad`
     * is, one W apart, which is exactly why the EVEX table in evex.cpp refuses to re-spell `psrad` at W1:
     * written that way it is this instruction.
     *
     * The destination is `vvvv` and the source the r/m operand, as it is for every shift by an immediate
     * under a vector prefix - see `dropTie`.
     */
    {
        auto& form = add(FormVSar64Imm, OpVSar, "vpsraq xmm, xmm, imm8"_v);
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(immediate(ImmediateWidth::Imm8));
        form.defs.push(def(ClassXmm128));
        form.requiredFeatures = features(0);
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RmExtImm,
            .opcode = 0x72,
            .escape = 0x0f, .prefix = 0x66, .extension = 4,
            .rmField = useRef(0), .immField = useRef(1),
            .width = OperationWidth::FromResult,
            .widthInPrefix = true,
            .prefixEncoding = PrefixEncoding::Evex,
        };
        form.encoding.vvvvField = defRef(0);
        form.encoding.evexW = true;
    }
    widen(FormVWideSar64Imm, FormVSar64Imm, "vpsraq ymm, ymm, imm8"_v);

    /*
     * The shifts by a count per lane that AVX2 left out: `vpsravq` (F), and the whole word family -
     * `vpsllvw`, `vpsrlvw`, `vpsravw` (BW). Below this level the first is `vpsrlvq` and a bias, and the
     * word lane is scalarized one lane at a time.
     *
     * The operand order is AVX2's `vpsllvd` row exactly: the value shifted is `vvvv` and the counts are
     * the r/m operand.
     */
    auto shiftVar = [&](MachineFormId id, MachineFormId wideId, MachineOpcodeId opcode, StringView name128,
                        StringView name256, U8 op, FeatureSet extension) {
        auto& form = add(id, opcode, name128);
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(def(ClassXmm128));
        form.requiredFeatures = features(extension);
        form.encoding = evexRegRm(0x66, kOpcodeMap0F38, op, true);
        form.encoding.rmField = useRef(1);
        form.encoding.vvvvField = useRef(0);

        widen(wideId, id, name256);
    };

    shiftVar(FormVSarVar64, FormVWideSarVar64, OpVSar, "vpsravq xmm, xmm, xmm"_v, "vpsravq ymm, ymm, ymm"_v, 0x46, 0);
    shiftVar(FormVShlVar16, FormVWideShlVar16, OpVShl, "vpsllvw xmm, xmm, xmm"_v, "vpsllvw ymm, ymm, ymm"_v, 0x12, kFeatureAvx512bw);
    shiftVar(FormVShrVar16, FormVWideShrVar16, OpVShr, "vpsrlvw xmm, xmm, xmm"_v, "vpsrlvw ymm, ymm, ymm"_v, 0x10, kFeatureAvx512bw);
    shiftVar(FormVSarVar16, FormVWideSarVar16, OpVSar, "vpsravw xmm, xmm, xmm"_v, "vpsravw ymm, ymm, ymm"_v, 0x11, kFeatureAvx512bw);

    /*
     * The quadword minimum and maximum at both signednesses - `vpminsq`, `vpminuq`, `vpmaxsq`,
     * `vpmaxuq`, the W1 halves of the SSE4.1 dword rows. Below this level `selectPackedMinMax` leaves the
     * comparison and the select standing, and the unsigned pair is biased on top of that.
     */
    auto minMax = [&](MachineFormId id, MachineFormId wideId, MachineOpcodeId opcode, StringView name128,
                      StringView name256, U8 op) {
        auto& form = add(id, opcode, name128);
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(def(ClassXmm128));
        form.requiredFeatures = features(0);
        form.encoding = evexRegRm(0x66, kOpcodeMap0F38, op, true);
        form.encoding.rmField = useRef(1);
        form.encoding.vvvvField = useRef(0);

        widen(wideId, id, name256);
    };

    minMax(FormVMinI64, FormVWideMinI64, OpVMin, "vpminsq xmm, xmm, xmm"_v, "vpminsq ymm, ymm, ymm"_v, 0x39);
    minMax(FormVMinU64, FormVWideMinU64, OpVMin, "vpminuq xmm, xmm, xmm"_v, "vpminuq ymm, ymm, ymm"_v, 0x3b);
    minMax(FormVMaxI64, FormVWideMaxI64, OpVMax, "vpmaxsq xmm, xmm, xmm"_v, "vpmaxsq ymm, ymm, ymm"_v, 0x3d);
    minMax(FormVMaxU64, FormVWideMaxU64, OpVMax, "vpmaxuq xmm, xmm, xmm"_v, "vpmaxuq ymm, ymm, ymm"_v, 0x3f);

    /*
     * `vpmullq` - EVEX.66.0F38.W1 40, the W1 half of `pmulld` and AVX512DQ.
     *
     * **measure**, in Analysis-Avx512.md's terms: public tables give it several µops and a long latency
     * on Intel cores. It is taken anyway, because what it replaces is eight instructions and three
     * widening products, and §4.12's harness measured the representative multi-instruction rows as a
     * win on Zen 5.
     */
    {
        auto& form = add(FormVMul64, OpVMul, "vpmullq xmm, xmm, xmm"_v);
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(def(ClassXmm128));
        form.requiredFeatures = features(kFeatureAvx512dq);
        form.encoding = evexRegRm(0x66, kOpcodeMap0F38, 0x40, true);
        form.encoding.rmField = useRef(1);
        form.encoding.vvvvField = useRef(0);
    }
    widen(FormVWideMul64, FormVMul64, "vpmullq ymm, ymm, ymm"_v);

    /*
     * The rotations. `vprold`/`vprolq` and `vprord`/`vprorq` by a constant are group 13 at /1 and /0 -
     * the two extensions `psrad` and its siblings never used - and the per-lane `vprolv*`/`vprorv*` are
     * 0F38 15 and 14. W is the lane width at all four.
     *
     * Every count is taken modulo the lane width by the instruction itself, which is what a rotation
     * means in this language - so neither form needs the `and` the shift expansion puts in front of its
     * two distances.
     */
    auto rotateImm = [&](MachineFormId id, MachineFormId wideId, MachineOpcodeId opcode, StringView name128,
                         StringView name256, U8 extension, bool quad) {
        auto& form = add(id, opcode, name128);
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(immediate(ImmediateWidth::Imm8));
        form.defs.push(def(ClassXmm128));
        form.requiredFeatures = features(0);
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RmExtImm,
            .opcode = 0x72,
            .escape = 0x0f, .prefix = 0x66, .extension = extension,
            .rmField = useRef(0), .immField = useRef(1),
            .width = OperationWidth::FromResult,
            .widthInPrefix = true,
            .prefixEncoding = PrefixEncoding::Evex,
        };
        form.encoding.vvvvField = defRef(0);
        form.encoding.evexW = quad;

        widen(wideId, id, name256);
    };

    rotateImm(FormVRol32Imm, FormVWideRol32Imm, OpVRol, "vprold xmm, xmm, imm8"_v, "vprold ymm, ymm, imm8"_v, 1, false);
    rotateImm(FormVRol64Imm, FormVWideRol64Imm, OpVRol, "vprolq xmm, xmm, imm8"_v, "vprolq ymm, ymm, imm8"_v, 1, true);
    rotateImm(FormVRor32Imm, FormVWideRor32Imm, OpVRor, "vprord xmm, xmm, imm8"_v, "vprord ymm, ymm, imm8"_v, 0, false);
    rotateImm(FormVRor64Imm, FormVWideRor64Imm, OpVRor, "vprorq xmm, xmm, imm8"_v, "vprorq ymm, ymm, imm8"_v, 0, true);

    auto rotateVar = [&](MachineFormId id, MachineFormId wideId, MachineOpcodeId opcode, StringView name128,
                         StringView name256, U8 op, bool quad) {
        auto& form = add(id, opcode, name128);
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(def(ClassXmm128));
        form.requiredFeatures = features(0);
        form.encoding = evexRegRm(0x66, kOpcodeMap0F38, op, quad);
        form.encoding.rmField = useRef(1);
        form.encoding.vvvvField = useRef(0);

        widen(wideId, id, name256);
    };

    rotateVar(FormVRolVar32, FormVWideRolVar32, OpVRol, "vprolvd xmm, xmm, xmm"_v, "vprolvd ymm, ymm, ymm"_v, 0x15, false);
    rotateVar(FormVRolVar64, FormVWideRolVar64, OpVRol, "vprolvq xmm, xmm, xmm"_v, "vprolvq ymm, ymm, ymm"_v, 0x15, true);
    rotateVar(FormVRorVar32, FormVWideRorVar32, OpVRor, "vprorvd xmm, xmm, xmm"_v, "vprorvd ymm, ymm, ymm"_v, 0x14, false);
    rotateVar(FormVRorVar64, FormVWideRorVar64, OpVRor, "vprorvq xmm, xmm, xmm"_v, "vprorvq ymm, ymm, ymm"_v, 0x14, true);

    /*
     * A word lane rotated left by a constant, which is `vpshldw` with the value as both of its sources -
     * EVEX.66.0F3A.W1 70 /r ib, AVX512VBMI2.
     *
     * The instruction shifts its first source left and fills the vacated bits from the top of its
     * second; with the two the same value that *is* the rotation, and a right rotation by `n` is this
     * one by `16 - n`, which `expandVectorRotate` writes before anything asks for a form. Two operand
     * fields name one use, which the descriptor states rather than the allocator seeing twice.
     */
    {
        auto& form = add(FormVRol16Imm, OpVRol, "vpshldw xmm, xmm, xmm, imm8"_v);
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(immediate(ImmediateWidth::Imm8));
        form.defs.push(def(ClassXmm128));
        form.requiredFeatures = features(kFeatureAvx512vbmi2);
        form.encoding = evexRegRm(0x66, kOpcodeMap0F3A, 0x70, true);
        form.encoding.vvvvField = useRef(0);
        form.encoding.immField = useRef(1);
        form.encoding.immediateByte = true;
    }
    widen(FormVWideRol16Imm, FormVRol16Imm, "vpshldw ymm, ymm, ymm, imm8"_v);

    /*
     * `vcvtqq2pd` - EVEX.F3.0F.W1 E6, AVX512DQ. The conversion `Math.yana`'s `vecDoubleFromWholeBits`
     * exists to avoid needing, and the one direction of the pair that is exact: every 64-bit integer
     * rounds to a double the way `cvtsi2sd` rounds one. The other direction needs the saturating clamp
     * every float-to-integer conversion here owes, and is not a row.
     */
    {
        auto& form = add(FormVCastI64ToF64, OpCast, "vcvtqq2pd xmm, xmm"_v);
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(def(ClassXmm128));
        form.requiredFeatures = features(kFeatureAvx512dq);
        form.encoding = evexRegRm(0xf3, kOpcodeMap0F, 0xe6, true);
    }
    widen(FormVWideCastI64ToF64, FormVCastI64ToF64, "vcvtqq2pd ymm, ymm"_v);

    /*
     * The packed bit counts - Analysis-Avx512.md §3.2, and the one group here standing behind three
     * different extension bits.
     *
     * `vpopcntb`/`vpopcntw` are EVEX.66.0F38.W0/W1 54 and need BITALG; `vpopcntd`/`vpopcntq` are the
     * same shape at opcode 55 and need VPOPCNTDQ; `vplzcntd`/`vplzcntq` are EVEX.66.0F38.W0/W1 44 and
     * need CD. Three `EvexRow` entries name the three, so that a build whose level ever splits them
     * declines the right subset rather than all or none.
     *
     * **There is no packed trailing-zero count at any level and so no row for one.**
     * `expandVectorBitCounts` writes `popcount((x & -x) - 1)`, which is four instructions here and
     * the same four at `sse4` with a longer population count inside it.
     */
    auto count = [&](MachineFormId id, MachineFormId wideId, StringView formName,
                     StringView wideName, MachineOpcodeId opcode, U8 op, bool w,
                     FeatureSet extension) {
        auto& form = add(id, opcode, formName);
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(def(ClassXmm128));
        form.requiredFeatures = features(extension);
        form.encoding = evexRegRm(0x66, kOpcodeMap0F38, op, w);

        // Written beside the narrow row rather than in a second sweep: `add` asserts that a form's
        // id is the position it lands at, so the two have to be built in the order they are numbered.
        widen(wideId, id, wideName);
    };

    count(FormVPopcount8, FormVWidePopcount8, "vpopcntb xmm, xmm"_v, "vpopcntb ymm, ymm"_v,
          OpVPopcount, 0x54, false, kFeatureAvx512bitalg);
    count(FormVPopcount16, FormVWidePopcount16, "vpopcntw xmm, xmm"_v, "vpopcntw ymm, ymm"_v,
          OpVPopcount, 0x54, true, kFeatureAvx512bitalg);
    count(FormVPopcount32, FormVWidePopcount32, "vpopcntd xmm, xmm"_v, "vpopcntd ymm, ymm"_v,
          OpVPopcount, 0x55, false, kFeatureAvx512vpopcntdq);
    count(FormVPopcount64, FormVWidePopcount64, "vpopcntq xmm, xmm"_v, "vpopcntq ymm, ymm"_v,
          OpVPopcount, 0x55, true, kFeatureAvx512vpopcntdq);
    count(FormVLeadingZeros32, FormVWideLeadingZeros32, "vplzcntd xmm, xmm"_v, "vplzcntd ymm, ymm"_v,
          OpVLeadingZeros, 0x44, false, kFeatureAvx512cd);
    count(FormVLeadingZeros64, FormVWideLeadingZeros64, "vplzcntq xmm, xmm"_v, "vplzcntq ymm, ymm"_v,
          OpVLeadingZeros, 0x44, true, kFeatureAvx512cd);

    /*
     * The five AVX-512 float operations - Analysis-Avx512.md §3.7, and the one group here whose
     * portable route is *already written*: `lib/Math/Math.yana`'s `Real` members and the functions
     * over them are what a build below this level runs, and `lib/Math/Math.avx512.yana` replaces six
     * of those bodies with one of these each.
     *
     * `vgetexpps`/`pd` are EVEX.66.0F38.W0/W1 42 and `vscalefps`/`pd` EVEX.66.0F38.W0/W1 2C, both F.
     * `vgetmantps`/`pd` are EVEX.66.0F3A.W0/W1 26 /r ib, also F. `vreduceps`/`pd` (0F3A 56) and
     * `vrangeps`/`pd` (0F3A 50) are DQ, which `avx512` implies - the level is x86-64-v4 and more
     * (§0.2), so there is no `EvexRow` here: a build that has these declarations has the instructions.
     *
     * Register-only, as the §2.4 rows above are: each has a memory form and none sits where a load
     * feeds it, every caller holding the value in a register already.
     */
    auto floatAdjust = [&](MachineFormId id, MachineFormId wideId, StringView formName,
                           StringView wideName, U8 map, U8 op, bool w, FeatureSet extension,
                           bool immediate) {
        auto& form = add(id, OpVFloatAdjust, formName);
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(def(ClassXmm128));
        form.requiredFeatures = features(extension);
        form.encoding = evexRegRm(0x66, map, op, w);
        form.encoding.patternImmediate = immediate;

        widen(wideId, id, wideName);
    };

    // The destination is ModRM.reg, the first operand `vvvv` and the second r/m - the three-operand
    // shape every EVEX binary here has.
    auto floatCombine = [&](MachineFormId id, MachineFormId wideId, StringView formName,
                            StringView wideName, U8 map, U8 op, bool w, FeatureSet extension,
                            bool immediate) {
        auto& form = add(id, OpVFloatCombine, formName);
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(def(ClassXmm128));
        form.requiredFeatures = features(extension);
        form.encoding = evexRegRm(0x66, map, op, w);
        form.encoding.vvvvField = useRef(0);
        form.encoding.rmField = useRef(1);
        form.encoding.patternImmediate = immediate;

        widen(wideId, id, wideName);
    };

    auto dq = kFeatureAvx512dq;

    floatAdjust(FormVGetExpF32, FormVWideGetExpF32, "vgetexpps xmm, xmm"_v, "vgetexpps ymm, ymm"_v,
                kOpcodeMap0F38, 0x42, false, 0, false);
    floatAdjust(FormVGetExpF64, FormVWideGetExpF64, "vgetexppd xmm, xmm"_v, "vgetexppd ymm, ymm"_v,
                kOpcodeMap0F38, 0x42, true, 0, false);
    floatAdjust(FormVGetMantF32, FormVWideGetMantF32, "vgetmantps xmm, xmm, control"_v,
                "vgetmantps ymm, ymm, control"_v, kOpcodeMap0F3A, 0x26, false, 0, true);
    floatAdjust(FormVGetMantF64, FormVWideGetMantF64, "vgetmantpd xmm, xmm, control"_v,
                "vgetmantpd ymm, ymm, control"_v, kOpcodeMap0F3A, 0x26, true, 0, true);
    floatAdjust(FormVFpReduceF32, FormVWideFpReduceF32, "vreduceps xmm, xmm, control"_v,
                "vreduceps ymm, ymm, control"_v, kOpcodeMap0F3A, 0x56, false, dq, true);
    floatAdjust(FormVFpReduceF64, FormVWideFpReduceF64, "vreducepd xmm, xmm, control"_v,
                "vreducepd ymm, ymm, control"_v, kOpcodeMap0F3A, 0x56, true, dq, true);

    floatCombine(FormVScaleF32, FormVWideScaleF32, "vscalefps xmm, xmm, xmm"_v,
                 "vscalefps ymm, ymm, ymm"_v, kOpcodeMap0F38, 0x2c, false, 0, false);
    floatCombine(FormVScaleF64, FormVWideScaleF64, "vscalefpd xmm, xmm, xmm"_v,
                 "vscalefpd ymm, ymm, ymm"_v, kOpcodeMap0F38, 0x2c, true, 0, false);
    floatCombine(FormVRangeF32, FormVWideRangeF32, "vrangeps xmm, xmm, xmm, control"_v,
                 "vrangeps ymm, ymm, ymm, control"_v, kOpcodeMap0F3A, 0x50, false, dq, true);
    floatCombine(FormVRangeF64, FormVWideRangeF64, "vrangepd xmm, xmm, xmm, control"_v,
                 "vrangepd ymm, ymm, ymm, control"_v, kOpcodeMap0F3A, 0x50, true, dq, true);

    /*
     * The four widening multiply-accumulates - Analysis-Avx512.md §3.6.
     *
     * `vpdpbusd` is EVEX.66.0F38.W0 50 and `vpdpwssd` is 52 (VNNI); `vpmadd52luq` is
     * EVEX.66.0F38.W1 B4 and `vpmadd52huq` B5 (IFMA). `avx512` implies both extensions, so these
     * carry no `EvexRow` for the float rows' reason: a build that has these declarations has the
     * instructions.
     *
     * **The destination is the accumulator**, tied to operand zero - the only rows in this file with
     * a tie, every other EVEX form here being a three-operand instruction that writes somewhere new.
     * So ModRM.reg is the tied definition, `vvvv` is the first product operand and r/m the second,
     * which is what `vpdpbusd`'s operand order means on the machine.
     *
     * **`vpdpbusd`'s operands are not interchangeable**: the first is read as unsigned bytes and the
     * second as signed ones. Nothing here can enforce that - the lower IR has no signedness - and
     * nothing has to: the kind arrives with the operands in the order the resolver checked.
     */
    auto mulAccumulate = [&](MachineFormId id, MachineFormId wideId, StringView formName,
                             StringView wideName, U8 op, bool w, FeatureSet extension) {
        auto& form = add(id, extension == kFeatureAvx512ifma ? OpVMulAdd52 : OpVDotProduct, formName);
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(tiedDef(0, ClassXmm128));
        form.requiredFeatures = features(extension);
        form.encoding = evexRegRm(0x66, kOpcodeMap0F38, op, w);
        form.encoding.regField = defRef(0);
        form.encoding.vvvvField = useRef(1);
        form.encoding.rmField = useRef(2);

        widen(wideId, id, wideName);
    };

    mulAccumulate(FormVDotBytes, FormVWideDotBytes, "vpdpbusd xmm, xmm, xmm"_v,
                  "vpdpbusd ymm, ymm, ymm"_v, 0x50, false, kFeatureAvx512vnni);
    mulAccumulate(FormVDotWords, FormVWideDotWords, "vpdpwssd xmm, xmm, xmm"_v,
                  "vpdpwssd ymm, ymm, ymm"_v, 0x52, false, kFeatureAvx512vnni);
    mulAccumulate(FormVMulAdd52Lo, FormVWideMulAdd52Lo, "vpmadd52luq xmm, xmm, xmm"_v,
                  "vpmadd52luq ymm, ymm, ymm"_v, 0xb4, true, kFeatureAvx512ifma);
    mulAccumulate(FormVMulAdd52Hi, FormVWideMulAdd52Hi, "vpmadd52huq xmm, xmm, xmm"_v,
                  "vpmadd52huq ymm, ymm, ymm"_v, 0xb5, true, kFeatureAvx512ifma);

    /*
     * The truncating narrowings - `vpmovwb`, `vpmovdw`, `vpmovqd`, EVEX.F3.0F38.W0 30/33/35.
     *
     * A 256-bit vector of one lane width into a 128-bit vector of half that width, keeping the low half
     * of every lane: the three shapes where both ends are a whole register. The *source* is ModRM.reg
     * and the destination r/m, which is the extract direction - the same shape `vextracti128` has.
     */
    auto narrow = [&](MachineFormId id, StringView formName, U8 op, FeatureSet extension) {
        auto& form = add(id, OpCast, formName);
        form.uses.push(anyReg(ClassYmm256));
        form.defs.push(def(ClassXmm128));
        form.requiredFeatures = features(extension);
        form.encoding = evexRegRm(0xf3, kOpcodeMap0F38, op, false);
        form.encoding.regField = useRef(0);
        form.encoding.rmField = defRef(0);
        form.encoding.width = OperationWidth::FromUse0;
        form.encoding.vectorLength = 1;
    };

    narrow(FormVNarrow16To8,  "vpmovwb xmm, ymm"_v, 0x30, kFeatureAvx512bw);
    narrow(FormVNarrow32To16, "vpmovdw xmm, ymm"_v, 0x33, 0);
    narrow(FormVNarrow64To32, "vpmovqd xmm, ymm"_v, 0x35, 0);

    /*
     * `vcvtusi2ss` and `vcvtusi2sd` - EVEX.F3/F2.0F 7B, with W the integer's width as `cvtsi2sd` has
     * it. At `avx2` a 32-bit source is zero-extended and converted signed, and a 64-bit one is halved,
     * converted and doubled with both arms computed and a select between them (README §3.0).
     *
     * Like the signed conversion, it writes one lane and names where the rest of the register comes
     * from, which is the destination - see `mergesIntoDestination`.
     */
    auto unsignedToFloat = [&](MachineFormId id, StringView formName, U8 prefix, RegisterClassId cls) {
        auto& form = add(id, OpCast, formName);
        form.uses.push(anyReg());
        form.defs.push(def(cls));
        form.requiredFeatures = kFeatureAvx512f;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = 0x7b,
            .escape = 0x0f, .prefix = prefix,
            .regField = defRef(0), .rmField = useRef(0),
            .width = OperationWidth::FromUse0,
            .prefixEncoding = PrefixEncoding::Evex,
        };
        form.encoding.vvvvField = defRef(0);
        form.encoding.mergesIntoDestination = true;
    };

    unsignedToFloat(FormCastUToF32, "vcvtusi2ss xmm, xmm, r"_v, 0xf3, ClassFloat32);
    unsignedToFloat(FormCastUToF64, "vcvtusi2sd xmm, xmm, r"_v, 0xf2, ClassFloat64);

    /*
     * ---------------------------------------------------------------------------------------------
     * Masks in mask registers - Analysis-Avx512.md §2.1.
     *
     * A mask register holds one bit per lane, and **zeros above the lanes**: every comparison below
     * writes it that way, and every operation that reads two masks and writes one keeps it, so the
     * one place the invariant costs anything is the complement (`PseudoKind::MaskNot`). That is what
     * lets `kmovd` into a general register be the movemask outright, with nothing masked after it.
     *
     * Every mask here is `ClassMask32`, which is every mask of a 128- or 256-bit register. The one
     * mask with more lanes - bytes at 512 bits - reads these rows as they are, each instruction reading
     * the register at its own opcode's width, and has `q` rows of its own for the few whose width *is*
     * the opcode (see the end of this function, and `registerFullForms`).
     * ---------------------------------------------------------------------------------------------
     */

    /*
     * The comparisons, at every lane width and every relation.
     *
     * `vpcmpeq*` and `vpcmpgt*` are the two relations with an opcode of their own, one byte shorter
     * than the predicate form, and the rest are `vpcmp*` and `vpcmpu*` with the relation in a trailing
     * byte - all eight of them, signed and unsigned, which is what retires the sign-bit bias and the
     * all-ones complement the vector-domain comparison needs for five of the ten. The float pair is
     * `vcmpps`/`vcmppd` with the predicate `cmpps` already has.
     *
     * The mask register is ModRM.reg, the first operand `vvvv` and the second r/m - which may be an
     * address, so a comparison reads its second vector straight out of memory as the vector-domain
     * one does. The doubleword, quadword and float rows may read that operand as one element repeated;
     * the byte and word rows have no broadcast form.
     */
    auto compare = [&](MachineFormId id, MachineFormId wideId, StringView name128, StringView name256,
                       U8 prefix, U8 map, U8 opcode, bool w, FeatureSet extension, bool predicate, bool floating) {
        auto& form = add(id, OpVCmp, name128);
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(regOrMem(MemoryAccessKind::Read, ClassXmm128));
        form.defs.push(def(ClassMask32));
        form.requiredFeatures = features(extension);
        form.encoding = evexRegRm(prefix, map, opcode, w);
        form.encoding.rmField = useRef(1);
        form.encoding.vvvvField = useRef(0);
        form.encoding.maskPredicateImmediate = predicate && !floating;
        form.encoding.conditionImmediate = predicate && floating;
        form.encoding.evexTuple = EvexTuple::Vector;
        form.encoding.evexBroadcast = extension != kFeatureAvx512bw;

        // The width the operand is read at is the vectors', not the mask's: what asks is whether a
        // load or a frame slot is the right size to stand in for the second operand.
        form.encoding.width = OperationWidth::FromUse0;

        widen(wideId, id, name256);
    };

    auto bw = kFeatureAvx512bw;

    compare(FormKCmpEq8,  FormVWideKCmpEq8,  "vpcmpeqb k, xmm, xmm"_v, "vpcmpeqb k, ymm, ymm"_v, 0x66, kOpcodeMap0F,   0x74, false, bw, false, false);
    compare(FormKCmpEq16, FormVWideKCmpEq16, "vpcmpeqw k, xmm, xmm"_v, "vpcmpeqw k, ymm, ymm"_v, 0x66, kOpcodeMap0F,   0x75, false, bw, false, false);
    compare(FormKCmpEq32, FormVWideKCmpEq32, "vpcmpeqd k, xmm, xmm"_v, "vpcmpeqd k, ymm, ymm"_v, 0x66, kOpcodeMap0F,   0x76, false, 0,  false, false);
    compare(FormKCmpEq64, FormVWideKCmpEq64, "vpcmpeqq k, xmm, xmm"_v, "vpcmpeqq k, ymm, ymm"_v, 0x66, kOpcodeMap0F38, 0x29, true,  0,  false, false);
    compare(FormKCmpGt8,  FormVWideKCmpGt8,  "vpcmpgtb k, xmm, xmm"_v, "vpcmpgtb k, ymm, ymm"_v, 0x66, kOpcodeMap0F,   0x64, false, bw, false, false);
    compare(FormKCmpGt16, FormVWideKCmpGt16, "vpcmpgtw k, xmm, xmm"_v, "vpcmpgtw k, ymm, ymm"_v, 0x66, kOpcodeMap0F,   0x65, false, bw, false, false);
    compare(FormKCmpGt32, FormVWideKCmpGt32, "vpcmpgtd k, xmm, xmm"_v, "vpcmpgtd k, ymm, ymm"_v, 0x66, kOpcodeMap0F,   0x66, false, 0,  false, false);
    compare(FormKCmpGt64, FormVWideKCmpGt64, "vpcmpgtq k, xmm, xmm"_v, "vpcmpgtq k, ymm, ymm"_v, 0x66, kOpcodeMap0F38, 0x37, true,  0,  false, false);
    compare(FormKCmp8,    FormVWideKCmp8,    "vpcmpb k, xmm, xmm, predicate"_v, "vpcmpb k, ymm, ymm, predicate"_v, 0x66, kOpcodeMap0F3A, 0x3f, false, bw, true, false);
    compare(FormKCmp16,   FormVWideKCmp16,   "vpcmpw k, xmm, xmm, predicate"_v, "vpcmpw k, ymm, ymm, predicate"_v, 0x66, kOpcodeMap0F3A, 0x3f, true,  bw, true, false);
    compare(FormKCmp32,   FormVWideKCmp32,   "vpcmpd k, xmm, xmm, predicate"_v, "vpcmpd k, ymm, ymm, predicate"_v, 0x66, kOpcodeMap0F3A, 0x1f, false, 0,  true, false);
    compare(FormKCmp64,   FormVWideKCmp64,   "vpcmpq k, xmm, xmm, predicate"_v, "vpcmpq k, ymm, ymm, predicate"_v, 0x66, kOpcodeMap0F3A, 0x1f, true,  0,  true, false);
    compare(FormKCmpU8,   FormVWideKCmpU8,   "vpcmpub k, xmm, xmm, predicate"_v, "vpcmpub k, ymm, ymm, predicate"_v, 0x66, kOpcodeMap0F3A, 0x3e, false, bw, true, false);
    compare(FormKCmpU16,  FormVWideKCmpU16,  "vpcmpuw k, xmm, xmm, predicate"_v, "vpcmpuw k, ymm, ymm, predicate"_v, 0x66, kOpcodeMap0F3A, 0x3e, true,  bw, true, false);
    compare(FormKCmpU32,  FormVWideKCmpU32,  "vpcmpud k, xmm, xmm, predicate"_v, "vpcmpud k, ymm, ymm, predicate"_v, 0x66, kOpcodeMap0F3A, 0x1e, false, 0,  true, false);
    compare(FormKCmpU64,  FormVWideKCmpU64,  "vpcmpuq k, xmm, xmm, predicate"_v, "vpcmpuq k, ymm, ymm, predicate"_v, 0x66, kOpcodeMap0F3A, 0x1e, true,  0,  true, false);
    compare(FormKCmpF32,  FormVWideKCmpF32,  "vcmpps k, xmm, xmm, predicate"_v, "vcmpps k, ymm, ymm, predicate"_v, 0x00, kOpcodeMap0F, 0xc2, false, 0, true, true);
    compare(FormKCmpF64,  FormVWideKCmpF64,  "vcmppd k, xmm, xmm, predicate"_v, "vcmppd k, ymm, ymm, predicate"_v, 0x66, kOpcodeMap0F, 0xc2, true,  0, true, true);

    {
        static const MachineFormId kCompares[36] = {
            FormKCmpEq8, FormVWideKCmpEq8, FormKCmpEq16, FormVWideKCmpEq16,
            FormKCmpEq32, FormVWideKCmpEq32, FormKCmpEq64, FormVWideKCmpEq64,
            FormKCmpGt8, FormVWideKCmpGt8, FormKCmpGt16, FormVWideKCmpGt16,
            FormKCmpGt32, FormVWideKCmpGt32, FormKCmpGt64, FormVWideKCmpGt64,
            FormKCmp8, FormVWideKCmp8, FormKCmp16, FormVWideKCmp16,
            FormKCmp32, FormVWideKCmp32, FormKCmp64, FormVWideKCmp64,
            FormKCmpU8, FormVWideKCmpU8, FormKCmpU16, FormVWideKCmpU16,
            FormKCmpU32, FormVWideKCmpU32, FormKCmpU64, FormVWideKCmpU64,
            FormKCmpF32, FormVWideKCmpF32, FormKCmpF64, FormVWideKCmpF64,
        };
        static const StringView kNames[36] = {
            "vpcmpeqb k, xmm, [address]"_v, "vpcmpeqb k, ymm, [address]"_v,
            "vpcmpeqw k, xmm, [address]"_v, "vpcmpeqw k, ymm, [address]"_v,
            "vpcmpeqd k, xmm, [address]"_v, "vpcmpeqd k, ymm, [address]"_v,
            "vpcmpeqq k, xmm, [address]"_v, "vpcmpeqq k, ymm, [address]"_v,
            "vpcmpgtb k, xmm, [address]"_v, "vpcmpgtb k, ymm, [address]"_v,
            "vpcmpgtw k, xmm, [address]"_v, "vpcmpgtw k, ymm, [address]"_v,
            "vpcmpgtd k, xmm, [address]"_v, "vpcmpgtd k, ymm, [address]"_v,
            "vpcmpgtq k, xmm, [address]"_v, "vpcmpgtq k, ymm, [address]"_v,
            "vpcmpb k, xmm, [address], predicate"_v, "vpcmpb k, ymm, [address], predicate"_v,
            "vpcmpw k, xmm, [address], predicate"_v, "vpcmpw k, ymm, [address], predicate"_v,
            "vpcmpd k, xmm, [address], predicate"_v, "vpcmpd k, ymm, [address], predicate"_v,
            "vpcmpq k, xmm, [address], predicate"_v, "vpcmpq k, ymm, [address], predicate"_v,
            "vpcmpub k, xmm, [address], predicate"_v, "vpcmpub k, ymm, [address], predicate"_v,
            "vpcmpuw k, xmm, [address], predicate"_v, "vpcmpuw k, ymm, [address], predicate"_v,
            "vpcmpud k, xmm, [address], predicate"_v, "vpcmpud k, ymm, [address], predicate"_v,
            "vpcmpuq k, xmm, [address], predicate"_v, "vpcmpuq k, ymm, [address], predicate"_v,
            "vcmpps k, xmm, [address], predicate"_v, "vcmpps k, ymm, [address], predicate"_v,
            "vcmppd k, xmm, [address], predicate"_v, "vcmppd k, ymm, [address], predicate"_v,
        };

        for(Size i = 0; i < 36; i++) {
            memoryTwin(MachineFormId(FormKCmpEq8Mem + i), kCompares[i], kNames[i]);
        }
    }

    /*
     * Two masks combined - `kandd`, `kord`, `kxord`, `kandnd`, VEX.L1.66.0F.W1 41/45/47/42.
     *
     * VEX rather than EVEX: the operations on mask registers themselves arrived with a VEX spelling,
     * and a mask register is never one only EVEX can name. `L` set is part of what the instruction is
     * rather than a width. W1 is the 32-bit register, which `Fixed64` is how this table says a VEX.W
     * that is not a width in the prefix.
     *
     * `kandnd k1, k2, k3` is `~k2 & k3`, so the complemented operand is `vvvv` - the first one, which
     * is `X86MaskAnd`'s order when the mask is the arm dropped.
     */
    auto maskBitwise = [&](MachineFormId id, MachineOpcodeId opcode, StringView formName, U8 op, bool quad = false) {
        auto regClass = quad ? ClassMask64 : ClassMask32;
        auto& form = add(id, opcode, formName);
        form.uses.push(anyReg(regClass));
        form.uses.push(anyReg(regClass));
        form.defs.push(def(regClass));
        form.requiredFeatures = kFeatureAvx512f | kFeatureAvx512bw;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = op,
            .escape = 0x0f, .prefix = U8(quad ? 0x00 : 0x66),
            .regField = defRef(0), .rmField = useRef(1),
            .width = OperationWidth::Fixed64,
            .prefixEncoding = PrefixEncoding::Vex,
            .opcodeMap = kOpcodeMap0F,
            .vectorLength = 1,
        };
        form.encoding.vvvvField = useRef(0);
    };

    maskBitwise(FormKAnd,    OpVAnd,    "kandd k, k, k"_v,  0x41);
    maskBitwise(FormKOr,     OpVOr,     "kord k, k, k"_v,   0x45);
    maskBitwise(FormKXor,    OpVXor,    "kxord k, k, k"_v,  0x47);
    maskBitwise(FormKAndNot, OpVAndNot, "kandnd k, k, k"_v, 0x42);

    // The complement, which is `knotd` and the clearing of whatever it set above the lanes - see
    // PseudoKind::MaskNot. Its own register in and out, and nothing scratched.
    {
        auto& form = add(FormKNot, OpVNot, "knotd; kshiftld; kshiftrd (complement)"_v);
        form.uses.push(anyReg(ClassMask32));
        form.defs.push(def(ClassMask32));
        form.requiredFeatures = kFeatureAvx512f | kFeatureAvx512bw;
        form.encoding = EncodingDescriptor { .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::MaskNot };
    }

    // A mask copied, which is `kmovd k, k` - VEX.L0.66.0F.W1 90 - and nothing where the two are one
    // register already, on `FormVMove`'s terms.
    {
        auto& form = add(FormKMove, OpMove, "kmovd k, k"_v);
        form.uses.push(anyReg(ClassMask32));
        form.defs.push(def(ClassMask32));
        form.requiredFeatures = kFeatureAvx512f | kFeatureAvx512bw;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = 0x90,
            .escape = 0x0f, .prefix = 0x66,
            .regField = defRef(0), .rmField = useRef(0),
            .width = OperationWidth::Fixed64,
            .prefixEncoding = PrefixEncoding::Vex,
            .omitWhenSame = true,
        };
    }

    /*
     * The movemask, which a mask register *is* - `kmovd r32, k`, VEX.L0.F2.0F.W0 93.
     *
     * One bit per lane whatever the lane width, where `pmovmskb` answered one per byte and a 16-bit
     * lane's consumers paid a shift to divide them back out. `maskBitsPerLane` in transform_reduce.cpp
     * is the other half of that and says one here.
     */
    {
        auto& form = add(FormKBits, OpVMaskBits, "kmovd r32, k"_v);
        form.uses.push(anyReg(ClassMask32));
        form.defs.push(def(ClassGpr32));
        form.requiredFeatures = kFeatureAvx512f | kFeatureAvx512bw;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = 0x93,
            .escape = 0x0f, .prefix = 0xf2,
            .regField = defRef(0), .rmField = useRef(0),
            .width = OperationWidth::Fixed32,
            .prefixEncoding = PrefixEncoding::Vex,
        };
    }

    /*
     * A mask read from memory and written to it, where it is the vector of all-ones and all-zeros lanes
     * - Design-Vector §2.4 makes that its representation at every level, so a program that stores one
     * reads the same bytes back at `avx2`. Through the declared scratch vector register and a crossing
     * each way; see PseudoKind::MaskLoad.
     */
    {
        auto& form = add(FormKLoad, OpLoad, "vmovdqu xmm11, [address]; vpmovd2m k, xmm11"_v);
        form.uses.push(address());
        form.defs.push(def(ClassMask32));
        form.clobbers.add(vectorReg(kVectorScratch));
        form.requiredFeatures = features(kFeatureAvx512bw | kFeatureAvx512dq);
        form.encoding = EncodingDescriptor { .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::MaskLoad };
    }

    {
        auto& form = add(FormKStore, OpStore, "vpmovm2d xmm11, k; vmovdqu [address], xmm11"_v);
        form.uses.push(address());
        form.uses.push(anyReg(ClassMask32));
        form.clobbers.add(vectorReg(kVectorScratch));
        form.requiredFeatures = features(kFeatureAvx512bw | kFeatureAvx512dq);
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::MaskStore,
            .width = OperationWidth::FromUse1,
        };
    }

    /*
     * The two crossings - `vpmovm2b`/`w`/`d`/`q` and `vpmovb2m`/`w2m`/`d2m`/`q2m`, EVEX.F3.0F38 28 and
     * 38, 29 and 39, W the lane's width halving. Into a vector each lane is all-ones where its bit is
     * set; out of one each bit is the lane's sign bit, which for a mask lane is the whole lane.
     */
    auto toVector = [&](MachineFormId id, MachineFormId wideId, StringView name128, StringView name256,
                        U8 op, bool w, FeatureSet extension) {
        auto& form = add(id, OpMaskToVector, name128);
        form.uses.push(anyReg(ClassMask32));
        form.defs.push(def(ClassXmm128));
        form.requiredFeatures = features(extension);
        form.encoding = evexRegRm(0xf3, kOpcodeMap0F38, op, w);

        widen(wideId, id, name256);
    };

    toVector(FormKToVector8,  FormVWideKToVector8,  "vpmovm2b xmm, k"_v, "vpmovm2b ymm, k"_v, 0x28, false, bw);
    toVector(FormKToVector16, FormVWideKToVector16, "vpmovm2w xmm, k"_v, "vpmovm2w ymm, k"_v, 0x28, true,  bw);
    toVector(FormKToVector32, FormVWideKToVector32, "vpmovm2d xmm, k"_v, "vpmovm2d ymm, k"_v, 0x38, false, kFeatureAvx512dq);
    toVector(FormKToVector64, FormVWideKToVector64, "vpmovm2q xmm, k"_v, "vpmovm2q ymm, k"_v, 0x38, true,  kFeatureAvx512dq);

    auto toMask = [&](MachineFormId id, MachineFormId wideId, StringView name128, StringView name256,
                      U8 op, bool w, FeatureSet extension) {
        auto& form = add(id, OpVectorToMask, name128);
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(def(ClassMask32));
        form.requiredFeatures = features(extension);
        form.encoding = evexRegRm(0xf3, kOpcodeMap0F38, op, w);

        widen(wideId, id, name256);
    };

    toMask(FormVectorToK8,  FormVWideVectorToK8,  "vpmovb2m k, xmm"_v, "vpmovb2m k, ymm"_v, 0x29, false, bw);
    toMask(FormVectorToK16, FormVWideVectorToK16, "vpmovw2m k, xmm"_v, "vpmovw2m k, ymm"_v, 0x29, true,  bw);
    toMask(FormVectorToK32, FormVWideVectorToK32, "vpmovd2m k, xmm"_v, "vpmovd2m k, ymm"_v, 0x39, false, kFeatureAvx512dq);
    toMask(FormVectorToK64, FormVWideVectorToK64, "vpmovq2m k, xmm"_v, "vpmovq2m k, ymm"_v, 0x39, true,  kFeatureAvx512dq);

    /*
     * The select - a masked move of the operand taken where the mask is set, over the one the tie has
     * already put in the destination. `vmovdqu8`/`16`/`32`/`64` and `vmovups`/`vmovupd`, whose element
     * is the lane, so a mask bit per lane is a mask bit per element.
     *
     * One instruction and no scratch, against `vpblendvb`, which read three vectors: what it costs is
     * the tie, which is a copy wherever the operand kept is still live afterwards.
     */
    auto moves = [&](U8 lane, U8& prefix, U8& opcode, bool& w) {
        switch(lane) {
            case 0: prefix = 0xf2; opcode = 0x6f; w = false; break; // vmovdqu8
            case 1: prefix = 0xf2; opcode = 0x6f; w = true;  break; // vmovdqu16
            case 2: prefix = 0xf3; opcode = 0x6f; w = false; break; // vmovdqu32
            case 3: prefix = 0xf3; opcode = 0x6f; w = true;  break; // vmovdqu64
            case 4: prefix = 0x00; opcode = 0x10; w = false; break; // vmovups
            default: prefix = 0x66; opcode = 0x10; w = true; break; // vmovupd
        }
    };

    auto select = [&](MachineFormId id, MachineFormId wideId, StringView name128, StringView name256, U8 lane) {
        U8 prefix, opcode; bool w;
        moves(lane, prefix, opcode, w);

        auto& form = add(id, OpVBlend, name128);
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(anyReg(ClassMask32));
        form.defs.push(tiedDef(1, ClassXmm128));
        form.requiredFeatures = features(lane < 2 ? bw : 0);
        form.encoding = evexRegRm(prefix, kOpcodeMap0F, opcode, w);
        form.encoding.maskField = useRef(2);

        widen(wideId, id, name256);
    };

    select(FormKSelect8,   FormVWideKSelect8,   "vmovdqu8 xmm {k}, xmm (select)"_v,  "vmovdqu8 ymm {k}, ymm (select)"_v, 0);
    select(FormKSelect16,  FormVWideKSelect16,  "vmovdqu16 xmm {k}, xmm (select)"_v, "vmovdqu16 ymm {k}, ymm (select)"_v, 1);
    select(FormKSelect32,  FormVWideKSelect32,  "vmovdqu32 xmm {k}, xmm (select)"_v, "vmovdqu32 ymm {k}, ymm (select)"_v, 2);
    select(FormKSelect64,  FormVWideKSelect64,  "vmovdqu64 xmm {k}, xmm (select)"_v, "vmovdqu64 ymm {k}, ymm (select)"_v, 3);
    select(FormKSelectF32, FormVWideKSelectF32, "vmovups xmm {k}, xmm (select)"_v,   "vmovups ymm {k}, ymm (select)"_v, 4);
    select(FormKSelectF64, FormVWideKSelectF64, "vmovupd xmm {k}, xmm (select)"_v,   "vmovupd ymm {k}, ymm (select)"_v, 5);

    /*
     * The masked vector - `X86MaskAnd`. Kept where the mask is set is the same masked move with the
     * lanes it masks out zeroed (`{z}`), and needs no tie at all. Dropped where it is set is the
     * value subtracted from itself under the mask - `vpsubb`/`w`/`d`/`q` at the lane's own element,
     * `vxorps`/`vxorpd` at a float one - which writes zero exactly there and keeps the rest.
     *
     * The operand order is `X86MaskAnd`'s: the value first where the mask is kept, the mask first
     * where it is dropped.
     */
    auto keep = [&](MachineFormId id, MachineFormId wideId, StringView name128, StringView name256, U8 lane) {
        U8 prefix, opcode; bool w;
        moves(lane, prefix, opcode, w);

        auto& form = add(id, OpVAnd, name128);
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(anyReg(ClassMask32));
        form.defs.push(def(ClassXmm128));
        form.requiredFeatures = features(lane < 2 ? bw : 0);
        form.encoding = evexRegRm(prefix, kOpcodeMap0F, opcode, w);
        form.encoding.maskField = useRef(1);
        form.encoding.zeroMasking = true;

        widen(wideId, id, name256);
    };

    keep(FormKKeep8,   FormVWideKKeep8,   "vmovdqu8 xmm {k}{z}, xmm"_v,  "vmovdqu8 ymm {k}{z}, ymm"_v, 0);
    keep(FormKKeep16,  FormVWideKKeep16,  "vmovdqu16 xmm {k}{z}, xmm"_v, "vmovdqu16 ymm {k}{z}, ymm"_v, 1);
    keep(FormKKeep32,  FormVWideKKeep32,  "vmovdqu32 xmm {k}{z}, xmm"_v, "vmovdqu32 ymm {k}{z}, ymm"_v, 2);
    keep(FormKKeep64,  FormVWideKKeep64,  "vmovdqu64 xmm {k}{z}, xmm"_v, "vmovdqu64 ymm {k}{z}, ymm"_v, 3);
    keep(FormKKeepF32, FormVWideKKeepF32, "vmovups xmm {k}{z}, xmm"_v,   "vmovups ymm {k}{z}, ymm"_v, 4);
    keep(FormKKeepF64, FormVWideKKeepF64, "vmovupd xmm {k}{z}, xmm"_v,   "vmovupd ymm {k}{z}, ymm"_v, 5);

    /*
     * And each of the twelve reading its value out of memory - a zero-masking *load*, which faults on
     * none of the lanes it masks out. That is §2.6's point: `select(live, loadVectorTail(chunk, i),
     * empty)` becomes one instruction whose correctness at the end of a buffer rests on the machine
     * rather than on the overread guarantee. `foldLoads` is what moves an instruction onto one, as it
     * does for every other memory twin here, and the displacement is scaled by the vector length.
     */
    {
        static const MachineFormId kKeep[12] = {
            FormKKeep8, FormVWideKKeep8, FormKKeep16, FormVWideKKeep16,
            FormKKeep32, FormVWideKKeep32, FormKKeep64, FormVWideKKeep64,
            FormKKeepF32, FormVWideKKeepF32, FormKKeepF64, FormVWideKKeepF64,
        };
        static const StringView kNames[12] = {
            "vmovdqu8 xmm {k}{z}, [address]"_v,  "vmovdqu8 ymm {k}{z}, [address]"_v,
            "vmovdqu16 xmm {k}{z}, [address]"_v, "vmovdqu16 ymm {k}{z}, [address]"_v,
            "vmovdqu32 xmm {k}{z}, [address]"_v, "vmovdqu32 ymm {k}{z}, [address]"_v,
            "vmovdqu64 xmm {k}{z}, [address]"_v, "vmovdqu64 ymm {k}{z}, [address]"_v,
            "vmovups xmm {k}{z}, [address]"_v,   "vmovups ymm {k}{z}, [address]"_v,
            "vmovupd xmm {k}{z}, [address]"_v,   "vmovupd ymm {k}{z}, [address]"_v,
        };

        for(Size i = 0; i < 12; i++) {
            forms[kKeep[i]].uses[0] = regOrMem(MemoryAccessKind::Read, forms[kKeep[i]].uses[0].regClass);
            forms[kKeep[i]].encoding.evexTuple = EvexTuple::Vector;
        }

        for(Size i = 0; i < 12; i++) {
            memoryTwin(MachineFormId(FormKKeep8Mem + i), kKeep[i], kNames[i]);
        }
    }

    auto drop = [&](MachineFormId id, MachineFormId wideId, StringView name128, StringView name256, U8 lane) {
        static const struct { U8 prefix, opcode; bool w; } kDrop[6] = {
            { 0x66, 0xf8, false }, { 0x66, 0xf9, false }, { 0x66, 0xfa, false }, { 0x66, 0xfb, true },
            { 0x00, 0x57, false }, { 0x66, 0x57, true },
        };

        auto& form = add(id, OpVAndNot, name128);
        form.uses.push(anyReg(ClassMask32));
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(tiedDef(1, ClassXmm128));
        form.requiredFeatures = features(lane < 2 ? bw : lane >= 4 ? kFeatureAvx512dq : 0);
        form.encoding = evexRegRm(kDrop[lane].prefix, kOpcodeMap0F, kDrop[lane].opcode, kDrop[lane].w);
        form.encoding.rmField = useRef(1);
        form.encoding.vvvvField = useRef(1);
        form.encoding.maskField = useRef(0);

        widen(wideId, id, name256);
    };

    drop(FormKDrop8,   FormVWideKDrop8,   "vpsubb xmm {k}, xmm, xmm (drop)"_v, "vpsubb ymm {k}, ymm, ymm (drop)"_v, 0);
    drop(FormKDrop16,  FormVWideKDrop16,  "vpsubw xmm {k}, xmm, xmm (drop)"_v, "vpsubw ymm {k}, ymm, ymm (drop)"_v, 1);
    drop(FormKDrop32,  FormVWideKDrop32,  "vpsubd xmm {k}, xmm, xmm (drop)"_v, "vpsubd ymm {k}, ymm, ymm (drop)"_v, 2);
    drop(FormKDrop64,  FormVWideKDrop64,  "vpsubq xmm {k}, xmm, xmm (drop)"_v, "vpsubq ymm {k}, ymm, ymm (drop)"_v, 3);
    drop(FormKDropF32, FormVWideKDropF32, "vxorps xmm {k}, xmm, xmm (drop)"_v, "vxorps ymm {k}, ymm, ymm (drop)"_v, 4);
    drop(FormKDropF64, FormVWideKDropF64, "vxorpd xmm {k}, xmm, xmm (drop)"_v, "vxorpd ymm {k}, ymm, ymm (drop)"_v, 5);

    /*
     * `vptestmb`/`w`/`d`/`q` - EVEX.66.0F38 26 and 27 - and `vptestnm*`, the same opcodes under F3: the
     * lanes of `lhs & rhs` that are not zero, and the ones that are. So `v .== zero()` is `vptestnmd k,
     * v, v`, with no zero register and no comparison, and `(a and b) .!= zero()` is one instruction
     * where it was three. Each has a masked twin, as the comparisons do - see registerMaskedForms.
     */
    auto test = [&](MachineFormId id, MachineFormId wideId, StringView name128, StringView name256,
                    U8 prefix, U8 op, bool w, FeatureSet extension) {
        auto& form = add(id, OpVCmp, name128);
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(def(ClassMask32));
        form.requiredFeatures = features(extension);
        form.encoding = evexRegRm(prefix, kOpcodeMap0F38, op, w);
        form.encoding.rmField = useRef(1);
        form.encoding.vvvvField = useRef(0);

        widen(wideId, id, name256);
    };

    test(FormKTest8,     FormVWideKTest8,     "vptestmb k, xmm, xmm"_v,  "vptestmb k, ymm, ymm"_v,  0x66, 0x26, false, bw);
    test(FormKTest16,    FormVWideKTest16,    "vptestmw k, xmm, xmm"_v,  "vptestmw k, ymm, ymm"_v,  0x66, 0x26, true,  bw);
    test(FormKTest32,    FormVWideKTest32,    "vptestmd k, xmm, xmm"_v,  "vptestmd k, ymm, ymm"_v,  0x66, 0x27, false, 0);
    test(FormKTest64,    FormVWideKTest64,    "vptestmq k, xmm, xmm"_v,  "vptestmq k, ymm, ymm"_v,  0x66, 0x27, true,  0);
    test(FormKTestNot8,  FormVWideKTestNot8,  "vptestnmb k, xmm, xmm"_v, "vptestnmb k, ymm, ymm"_v, 0xf3, 0x26, false, bw);
    test(FormKTestNot16, FormVWideKTestNot16, "vptestnmw k, xmm, xmm"_v, "vptestnmw k, ymm, ymm"_v, 0xf3, 0x26, true,  bw);
    test(FormKTestNot32, FormVWideKTestNot32, "vptestnmd k, xmm, xmm"_v, "vptestnmd k, ymm, ymm"_v, 0xf3, 0x27, false, 0);
    test(FormKTestNot64, FormVWideKTestNot64, "vptestnmq k, xmm, xmm"_v, "vptestnmq k, ymm, ymm"_v, 0xf3, 0x27, true,  0);

    /*
     * `kortestd k, k` - VEX.L0.66.0F.W1 98 - which sets ZF where the register is zero: `any` and `none`
     * without the `kmovd` and the `test` in between (§2.1). A comparison in every sense the flags
     * folding cares about, so these are `OpCmp` forms, and the constant zero they replace is folded
     * into the instruction.
     */
    auto zeroTest = [&](MachineFormId id, StringView formName, bool materialize) {
        auto& form = add(id, OpCmp, formName);
        form.uses.push(anyReg(ClassMask32));
        form.uses.push(immediate(ImmediateWidth::Imm8));
        form.defs.push(materialize ? def(ClassGpr32) : noDef());
        form.flagsEffect = FlagsEffect::Def;
        form.requiredFeatures = kFeatureAvx512f | kFeatureAvx512bw;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = 0x98,
            .escape = 0x0f, .prefix = 0x66,
            .regField = useRef(0), .rmField = useRef(0),
            .width = OperationWidth::Fixed64,
            .prefixEncoding = PrefixEncoding::Vex,
        };
        form.encoding.materializeFlags = materialize;
    };

    zeroTest(FormKZeroTest, "kortestd k, k"_v, false);
    zeroTest(FormKZeroTestSet, "kortestd k, k; setcc r"_v, true);

    // `kmovd k, r32` - VEX.L0.F2.0F.W0 92 - a mask register written from a general register's bits.
    auto fromBits = [&](MachineFormId id, StringView formName, bool quad) {
        auto& form = add(id, OpVectorToMask, formName);
        form.uses.push(anyReg(quad ? ClassGpr64 : ClassGpr32));
        form.defs.push(def(quad ? ClassMask64 : ClassMask32));
        form.requiredFeatures = kFeatureAvx512f | kFeatureAvx512bw;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = 0x92,
            .escape = 0x0f, .prefix = 0xf2,
            .regField = defRef(0), .rmField = useRef(0),
            .width = quad ? OperationWidth::Fixed64 : OperationWidth::Fixed32,
            .prefixEncoding = PrefixEncoding::Vex,
        };
    };

    fromBits(FormKFromBits, "kmovd k, r32"_v, false);

    /*
     * ---------------------------------------------------------------------------------------------
     * Sixty-four lanes - Analysis-Avx512.md §1.2's stage 2.
     *
     * A byte vector at 512 bits is the one mask with more lanes than `kmovd` moves, so what reads or
     * writes a whole mask register as a number has a `q` row beside its `d` one: the bitwise four, the
     * copy, the movemask both ways and `kortest`. The `d` spelling of each would drop the upper half
     * silently. Every other row reads a `k` register at the width its own opcode does - a byte
     * comparison at 512 bits writes all sixty-four bits whatever its operand class says - and the
     * complement and the two transfers through memory are pseudos that read the lane count.
     *
     * Where a `d` row writes the 66 prefix a `q` one writes none: `kandq` is VEX.L1.0F.W1 41, `kmovq`
     * VEX.L0.0F.W1 90, `kortestq` VEX.L0.0F.W1 98, and the general-register moves F2 at W1.
     * ---------------------------------------------------------------------------------------------
     */
    maskBitwise(FormKAndQ,    OpVAnd,    "kandq k, k, k"_v,  0x41, true);
    maskBitwise(FormKOrQ,     OpVOr,     "korq k, k, k"_v,   0x45, true);
    maskBitwise(FormKXorQ,    OpVXor,    "kxorq k, k, k"_v,  0x47, true);
    maskBitwise(FormKAndNotQ, OpVAndNot, "kandnq k, k, k"_v, 0x42, true);

    {
        auto& form = add(FormKMoveQ, OpMove, "kmovq k, k"_v);
        form.uses.push(anyReg(ClassMask64));
        form.defs.push(def(ClassMask64));
        form.requiredFeatures = kFeatureAvx512f | kFeatureAvx512bw;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = 0x90,
            .escape = 0x0f,
            .regField = defRef(0), .rmField = useRef(0),
            .width = OperationWidth::Fixed64,
            .prefixEncoding = PrefixEncoding::Vex,
            .omitWhenSame = true,
        };
    }

    {
        auto& form = add(FormKBitsQ, OpVMaskBits, "kmovq r64, k"_v);
        form.uses.push(anyReg(ClassMask64));
        form.defs.push(def(ClassGpr64));
        form.requiredFeatures = kFeatureAvx512f | kFeatureAvx512bw;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = 0x93,
            .escape = 0x0f, .prefix = 0xf2,
            .regField = defRef(0), .rmField = useRef(0),
            .width = OperationWidth::Fixed64,
            .prefixEncoding = PrefixEncoding::Vex,
        };
    }

    auto zeroTestQ = [&](MachineFormId id, StringView formName, bool materialize) {
        auto& form = add(id, OpCmp, formName);
        form.uses.push(anyReg(ClassMask64));
        form.uses.push(immediate(ImmediateWidth::Imm8));
        form.defs.push(materialize ? def(ClassGpr32) : noDef());
        form.flagsEffect = FlagsEffect::Def;
        form.requiredFeatures = kFeatureAvx512f | kFeatureAvx512bw;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = 0x98,
            .escape = 0x0f,
            .regField = useRef(0), .rmField = useRef(0),
            .width = OperationWidth::Fixed64,
            .prefixEncoding = PrefixEncoding::Vex,
        };
        form.encoding.materializeFlags = materialize;
    };

    zeroTestQ(FormKZeroTestQ, "kortestq k, k"_v, false);
    zeroTestQ(FormKZeroTestSetQ, "kortestq k, k; setcc r"_v, true);

    fromBits(FormKFromBitsQ, "kmovq k, r64"_v, true);
}

/*
 * The bitwise and lane-movement rows - Analysis-Avx512.md §2.3 and §2.5, steps 5 and 6 of its order of
 * work. Registered after the mask rows and with nothing derived from them: none is masked, none has a
 * VEX spelling, and each is reached only through an instruction a pass at `avx512` builds.
 */
void MachineFormBuilder::registerEvexShuffleForms() {
    auto features = [](FeatureSet extension) {
        return kFeatureAvx512f | kFeatureAvx512vl | extension;
    };

    auto evex = [](U8 map, U8 opcode, bool w) {
        auto e = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = opcode,
            .escape = 0x0f, .prefix = 0x66,
            .regField = defRef(0), .rmField = useRef(0),
            .width = OperationWidth::FromResult,
            .widthInPrefix = true,
            .prefixEncoding = PrefixEncoding::Evex,
            .opcodeMap = map,
        };

        e.evexW = w;
        return e;
    };

    // The 256-bit twin, as `registerEvexForms` derives it.
    auto widen = [&](MachineFormId id, MachineFormId narrowId, StringView formName) {
        auto twin = forms[narrowId];

        for(auto& c: twin.uses) if(c.regClass == ClassXmm128) c.regClass = ClassYmm256;
        for(auto& c: twin.defs) if(c.regClass == ClassXmm128) c.regClass = ClassYmm256;

        twin.encoding.vectorLength = 1;
        twin.id = id;
        twin.name = formName;
        twin.wide = 0;
        twin.wideOf = narrowId;

        auto& form = add(id, twin.opcode, formName);
        form = twin;
        forms[narrowId].wide = id;
    };

    /*
     * `vpternlogd a, b, c, table` - EVEX.66.0F3A.W0 25 /r ib, F.
     *
     * Destructive like the legacy bitwise rows: the answer is written over `a`, which is ModRM.reg; `b`
     * is `vvvv` and `c` the r/m operand, which is the one a pooled constant folds into. The element is
     * a doubleword for the broadcast and nothing else - a bitwise operation has no lane - so a table
     * over any integer vector is this row.
     */
    {
        auto& form = add(FormVTernaryLogic, OpVTernaryLogic, "vpternlogd xmm, xmm, xmm/m, table"_v);
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(regOrMem(MemoryAccessKind::Read, ClassXmm128));
        form.defs.push(tiedDef(0, ClassXmm128));
        form.requiredFeatures = features(0);
        form.encoding = evex(kOpcodeMap0F3A, 0x25, false);
        form.encoding.vvvvField = useRef(1);
        form.encoding.rmField = useRef(2);
        form.encoding.patternImmediate = true;
        form.encoding.evexTuple = EvexTuple::Vector;
        form.encoding.evexBroadcast = true;
    }
    widen(FormVWideTernaryLogic, FormVTernaryLogic, "vpternlogd ymm, ymm, ymm/m, table"_v);

    memoryTwin(FormVTernaryLogicMem, FormVTernaryLogic, "vpternlogd xmm, xmm, [address], table"_v);
    memoryTwin(FormVWideTernaryLogicMem, FormVWideTernaryLogic, "vpternlogd ymm, ymm, [address], table"_v);

    /*
     * `valignd`/`valignq dst, high, low, from` - EVEX.66.0F3A 03 /r ib, W the lane, F.
     *
     * The two sources placed end to end with `low` below, and `lanes` of them read starting at lane
     * `from`: `Core.alignLanes` in one instruction at every lane count, where `vpalignr` at 256 bits
     * shifts each 128-bit half on its own. The operand order is `FormVAlign`'s - the first source is the
     * high half - and the immediate counts lanes rather than bytes. A rotation is the same instruction
     * with one vector as both halves, which is the two rows after it: one reading the first operand and
     * one the second, on `FormVShuffle32Second`'s terms.
     */
    auto align = [&](MachineFormId id, StringView formName, bool quad, I32 source) {
        auto& form = add(id, OpVShuffle, formName);
        form.uses.push(anyReg(ClassYmm256));
        if(source != 0) form.uses.push(anyReg(ClassYmm256));
        form.defs.push(def(ClassYmm256));
        form.requiredFeatures = features(0);
        form.encoding = evex(kOpcodeMap0F3A, 0x03, quad);
        form.encoding.vectorLength = 1;
        form.encoding.vvvvField = useRef(U8(source < 0 ? 0 : source));
        form.encoding.rmField = useRef(U8(source < 0 ? 1 : source));
        form.encoding.patternImmediate = true;
    };

    align(FormVWideAlign32, "valignd ymm, ymm, ymm, from"_v, false, -1);
    align(FormVWideAlign64, "valignq ymm, ymm, ymm, from"_v, true, -1);
    align(FormVWideAlignOne32, "valignd ymm, ymm, ymm, from (rotation)"_v, false, 0);
    align(FormVWideAlignOne64, "valignq ymm, ymm, ymm, from (rotation)"_v, true, 0);
    align(FormVWideAlignSecond32, "valignd ymm, ymm, ymm, from (rotation of the second source)"_v, false, 1);
    align(FormVWideAlignSecond64, "valignq ymm, ymm, ymm, from (rotation of the second source)"_v, true, 1);

    /*
     * `vpbroadcast{b,w,d,q} xmm, r32/r64` - EVEX.66.0F38 7A, 7B and 7C, W the general register's width
     * at the quadword row and zero at the rest. AVX512BW for the byte and word rows.
     *
     * A splat of a scalar that is in a *general* register, which every tier below writes as `movd`
     * and then a broadcast of the vector register it landed in - see PseudoKind::VecBroadcast. This is
     * the whole of it in one instruction, and it is a form rather than a pseudo for that reason.
     */
    auto gprBroadcast = [&](MachineFormId id, MachineFormId wideId, StringView name128, StringView name256,
                            U8 opcode, bool w, RegisterClassId source, FeatureSet extension) {
        auto& form = add(id, OpVBroadcast, name128);
        form.uses.push(anyReg(source));
        form.defs.push(def(ClassXmm128));
        form.requiredFeatures = features(extension);
        form.encoding = evex(kOpcodeMap0F38, opcode, w);

        widen(wideId, id, name256);
    };

    gprBroadcast(FormVBroadcastGpr8,  FormVWideBroadcastGpr8,  "vpbroadcastb xmm, r32"_v, "vpbroadcastb ymm, r32"_v, 0x7a, false, ClassGpr32, kFeatureAvx512bw);
    gprBroadcast(FormVBroadcastGpr16, FormVWideBroadcastGpr16, "vpbroadcastw xmm, r32"_v, "vpbroadcastw ymm, r32"_v, 0x7b, false, ClassGpr32, kFeatureAvx512bw);
    gprBroadcast(FormVBroadcastGpr32, FormVWideBroadcastGpr32, "vpbroadcastd xmm, r32"_v, "vpbroadcastd ymm, r32"_v, 0x7c, false, ClassGpr32, 0);
    gprBroadcast(FormVBroadcastGpr64, FormVWideBroadcastGpr64, "vpbroadcastq xmm, r64"_v, "vpbroadcastq ymm, r64"_v, 0x7c, true,  ClassGpr64, 0);

    /*
     * `vpermi2b`/`w`/`d`/`q`/`ps`/`pd indices, left, right` - EVEX.66.0F38 75, 76, 77 with W the lane.
     * VBMI for the byte row, BW for the word row, F for the rest.
     *
     * The indices are ModRM.reg and the destination, `left` is `vvvv` and `right` the r/m operand. An
     * index names a lane of `left` where the bit above the lane number is clear and of `right` where it
     * is set, which is `VecShuffle`'s numbering outright - see `lowerTwoSourcePermutes`.
     */
    auto permute = [&](MachineFormId id, MachineFormId wideId, StringView name128, StringView name256,
                       U8 opcode, bool w, FeatureSet extension) {
        auto& form = add(id, OpVPermute2, name128);
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(tiedDef(0, ClassXmm128));
        form.requiredFeatures = features(extension);
        form.encoding = evex(kOpcodeMap0F38, opcode, w);
        form.encoding.vvvvField = useRef(1);
        form.encoding.rmField = useRef(2);

        widen(wideId, id, name256);
    };

    permute(FormVPermute2x8,  FormVWidePermute2x8,  "vpermi2b xmm, xmm, xmm"_v,  "vpermi2b ymm, ymm, ymm"_v,  0x75, false, kFeatureAvx512vbmi);
    permute(FormVPermute2x16, FormVWidePermute2x16, "vpermi2w xmm, xmm, xmm"_v,  "vpermi2w ymm, ymm, ymm"_v,  0x75, true,  kFeatureAvx512bw);
    permute(FormVPermute2x32, FormVWidePermute2x32, "vpermi2d xmm, xmm, xmm"_v,  "vpermi2d ymm, ymm, ymm"_v,  0x76, false, 0);
    permute(FormVPermute2x64, FormVWidePermute2x64, "vpermi2q xmm, xmm, xmm"_v,  "vpermi2q ymm, ymm, ymm"_v,  0x76, true,  0);
    permute(FormVPermute2F32, FormVWidePermute2F32, "vpermi2ps xmm, xmm, xmm"_v, "vpermi2ps ymm, ymm, ymm"_v, 0x77, false, 0);
    permute(FormVPermute2F64, FormVWidePermute2F64, "vpermi2pd xmm, xmm, xmm"_v, "vpermi2pd ymm, ymm, ymm"_v, 0x77, true,  0);

    /*
     * `vpcompress*` and `vpexpand*` - the lanes a mask register sets, packed to the bottom or spread
     * out to them, with every other lane zeroed (`{z}`). EVEX.66.0F38: 63 and 62 for the byte and word
     * rows (VBMI2, W the lane), 8B and 89 for the doubleword and quadword ones, 8A and 88 for the float
     * pair.
     *
     * Register to register, and the *compress* is the one that reads backwards: it is a store-shaped
     * instruction, so the source is ModRM.reg and the destination the r/m operand - the shape `vpmovwb`
     * above has. The memory destination is the form Analysis-Avx512.md §3.1 declines on purpose: the
     * Zen 4 guide has it microcoded, and `compress` promises nothing about the lanes past its count so
     * that the whole register may be stored instead.
     */
    auto lanesMove = [&](MachineFormId id, MachineFormId wideId, MachineOpcodeId opcode, StringView name128,
                         StringView name256, U8 op, bool w, FeatureSet extension) {
        auto& form = add(id, opcode, name128);
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(anyReg(ClassMask32));
        form.defs.push(def(ClassXmm128));
        form.requiredFeatures = features(extension);
        form.encoding = evex(kOpcodeMap0F38, op, w);
        form.encoding.maskField = useRef(1);
        form.encoding.zeroMasking = true;

        if(opcode == OpVCompress) {
            form.encoding.regField = useRef(0);
            form.encoding.rmField = defRef(0);
        }

        widen(wideId, id, name256);
    };

    auto vbmi2 = kFeatureAvx512vbmi2;

    lanesMove(FormVCompress8,   FormVWideCompress8,   OpVCompress, "vpcompressb xmm {k}{z}, xmm"_v, "vpcompressb ymm {k}{z}, ymm"_v, 0x63, false, vbmi2);
    lanesMove(FormVCompress16,  FormVWideCompress16,  OpVCompress, "vpcompressw xmm {k}{z}, xmm"_v, "vpcompressw ymm {k}{z}, ymm"_v, 0x63, true,  vbmi2);
    lanesMove(FormVCompress32,  FormVWideCompress32,  OpVCompress, "vpcompressd xmm {k}{z}, xmm"_v, "vpcompressd ymm {k}{z}, ymm"_v, 0x8b, false, 0);
    lanesMove(FormVCompress64,  FormVWideCompress64,  OpVCompress, "vpcompressq xmm {k}{z}, xmm"_v, "vpcompressq ymm {k}{z}, ymm"_v, 0x8b, true,  0);
    lanesMove(FormVCompressF32, FormVWideCompressF32, OpVCompress, "vcompressps xmm {k}{z}, xmm"_v, "vcompressps ymm {k}{z}, ymm"_v, 0x8a, false, 0);
    lanesMove(FormVCompressF64, FormVWideCompressF64, OpVCompress, "vcompresspd xmm {k}{z}, xmm"_v, "vcompresspd ymm {k}{z}, ymm"_v, 0x8a, true,  0);

    lanesMove(FormVExpand8,   FormVWideExpand8,   OpVExpand, "vpexpandb xmm {k}{z}, xmm"_v, "vpexpandb ymm {k}{z}, ymm"_v, 0x62, false, vbmi2);
    lanesMove(FormVExpand16,  FormVWideExpand16,  OpVExpand, "vpexpandw xmm {k}{z}, xmm"_v, "vpexpandw ymm {k}{z}, ymm"_v, 0x62, true,  vbmi2);
    lanesMove(FormVExpand32,  FormVWideExpand32,  OpVExpand, "vpexpandd xmm {k}{z}, xmm"_v, "vpexpandd ymm {k}{z}, ymm"_v, 0x89, false, 0);
    lanesMove(FormVExpand64,  FormVWideExpand64,  OpVExpand, "vpexpandq xmm {k}{z}, xmm"_v, "vpexpandq ymm {k}{z}, ymm"_v, 0x89, true,  0);
    lanesMove(FormVExpandF32, FormVWideExpandF32, OpVExpand, "vexpandps xmm {k}{z}, xmm"_v, "vexpandps ymm {k}{z}, ymm"_v, 0x88, false, 0);
    lanesMove(FormVExpandF64, FormVWideExpandF64, OpVExpand, "vexpandpd xmm {k}{z}, xmm"_v, "vexpandpd ymm {k}{z}, ymm"_v, 0x88, true,  0);

    /*
     * A vector read and written under a mask register - `vmovdqu8`/`16`/`32`/`64`, `vmovups`,
     * `vmovupd` with `{k}` - which is `loadUpTo` and `storeUpTo` at `avx512` (LowerInst::X86MaskedLoad).
     * The load zeroes the lanes it leaves (`{z}`); the store merges, which to memory means it leaves
     * them as they were, and EVEX has no zeroing store to choose instead. Neither faults on a lane the
     * mask clears, which is the whole of the reason to have them.
     *
     * The same six rows as the masked select's (EVEX.F2.0F 6F/7F for the byte and word lanes, F3 for the
     * doubleword and quadword ones, 0F 10/11 and 66.0F 10/11 for the floats), shaped as a load and a
     * store: an address in r/m, scaled as a whole vector for a one-byte displacement.
     */
    static const struct { U8 prefix, load, store; bool w; } kMoves[6] = {
        { 0xf2, 0x6f, 0x7f, false }, { 0xf2, 0x6f, 0x7f, true }, { 0xf3, 0x6f, 0x7f, false },
        { 0xf3, 0x6f, 0x7f, true },  { 0x00, 0x10, 0x11, false }, { 0x66, 0x10, 0x11, true },
    };

    auto transfer = [&](U8 lane, U8 opcode, OperandRef reg) {
        auto e = EncodingDescriptor {
            .family = EncodingFamily::LoadStore,
            .opcode = opcode,
            .escape = 0x0f, .prefix = kMoves[lane].prefix,
            .regField = reg, .rmField = useRef(0),
            .width = OperationWidth::Fixed32,
            .widthInPrefix = true,
            .prefixEncoding = PrefixEncoding::Evex,
            .opcodeMap = kOpcodeMap0F,
        };

        e.evexW = kMoves[lane].w;
        e.evexTuple = EvexTuple::Vector;
        e.maskField = useRef(1);
        return e;
    };

    auto maskedLoad = [&](MachineFormId id, MachineFormId wideId, StringView name128, StringView name256, U8 lane) {
        auto& form = add(id, OpVMaskedLoad, name128);
        form.uses.push(address());
        form.uses.push(anyReg(ClassMask32));
        form.defs.push(def(ClassXmm128));
        form.requiredFeatures = features(lane < 2 ? kFeatureAvx512bw : 0);
        form.encoding = transfer(lane, kMoves[lane].load, defRef(0));
        form.encoding.zeroMasking = true;

        widen(wideId, id, name256);
    };

    auto maskedStore = [&](MachineFormId id, MachineFormId wideId, StringView name128, StringView name256, U8 lane) {
        auto& form = add(id, OpVMaskedStore, name128);
        form.uses.push(address());
        form.uses.push(anyReg(ClassMask32));
        form.uses.push(anyReg(ClassXmm128));
        form.requiredFeatures = features(lane < 2 ? kFeatureAvx512bw : 0);
        form.encoding = transfer(lane, kMoves[lane].store, useRef(2));

        widen(wideId, id, name256);
    };

    maskedLoad(FormVMaskedLoad8,   FormVWideMaskedLoad8,   "vmovdqu8 xmm {k}{z}, [address] (upto)"_v,  "vmovdqu8 ymm {k}{z}, [address] (upto)"_v,  0);
    maskedLoad(FormVMaskedLoad16,  FormVWideMaskedLoad16,  "vmovdqu16 xmm {k}{z}, [address] (upto)"_v, "vmovdqu16 ymm {k}{z}, [address] (upto)"_v, 1);
    maskedLoad(FormVMaskedLoad32,  FormVWideMaskedLoad32,  "vmovdqu32 xmm {k}{z}, [address] (upto)"_v, "vmovdqu32 ymm {k}{z}, [address] (upto)"_v, 2);
    maskedLoad(FormVMaskedLoad64,  FormVWideMaskedLoad64,  "vmovdqu64 xmm {k}{z}, [address] (upto)"_v, "vmovdqu64 ymm {k}{z}, [address] (upto)"_v, 3);
    maskedLoad(FormVMaskedLoadF32, FormVWideMaskedLoadF32, "vmovups xmm {k}{z}, [address] (upto)"_v,   "vmovups ymm {k}{z}, [address] (upto)"_v,   4);
    maskedLoad(FormVMaskedLoadF64, FormVWideMaskedLoadF64, "vmovupd xmm {k}{z}, [address] (upto)"_v,   "vmovupd ymm {k}{z}, [address] (upto)"_v,   5);

    maskedStore(FormVMaskedStore8,   FormVWideMaskedStore8,   "vmovdqu8 [address] {k}, xmm"_v,  "vmovdqu8 [address] {k}, ymm"_v,  0);
    maskedStore(FormVMaskedStore16,  FormVWideMaskedStore16,  "vmovdqu16 [address] {k}, xmm"_v, "vmovdqu16 [address] {k}, ymm"_v, 1);
    maskedStore(FormVMaskedStore32,  FormVWideMaskedStore32,  "vmovdqu32 [address] {k}, xmm"_v, "vmovdqu32 [address] {k}, ymm"_v, 2);
    maskedStore(FormVMaskedStore64,  FormVWideMaskedStore64,  "vmovdqu64 [address] {k}, xmm"_v, "vmovdqu64 [address] {k}, ymm"_v, 3);
    maskedStore(FormVMaskedStoreF32, FormVWideMaskedStoreF32, "vmovups [address] {k}, xmm"_v,   "vmovups [address] {k}, ymm"_v,   4);
    maskedStore(FormVMaskedStoreF64, FormVWideMaskedStoreF64, "vmovupd [address] {k}, xmm"_v,   "vmovupd [address] {k}, ymm"_v,   5);

    /*
     * And the `avx2` pair, `vpmaskmovd`/`q`: VEX.66.0F38 8C to load and 8E to store, the mask a vector
     * whose lanes' sign bits choose - which a whole-lane mask's are. Here beside the EVEX rows because
     * they are the same two operations; they state AVX2 and nothing of AVX-512, and have no EVEX
     * spelling, so what reaches them is never an upper register. Only four- and eight-byte lanes have
     * the instruction, a float lane taking the integer row of its width.
     *
     * The element width is VEX.W, which the load/store family writes from the operation's width rather
     * than from the prefix - hence a fixed width on each row instead of `widthInPrefix`.
     */
    auto maskMove = [&](MachineFormId id, MachineFormId wideId, MachineOpcodeId opcode, StringView name128,
                        StringView name256, bool store, bool quad) {
        auto& form = add(id, opcode, name128);
        form.uses.push(address());
        form.uses.push(anyReg(ClassXmm128));
        if(store) form.uses.push(anyReg(ClassXmm128));
        else form.defs.push(def(ClassXmm128));
        form.requiredFeatures = kFeatureAvx | kFeatureAvx2;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::LoadStore,
            .opcode = U8(store ? 0x8e : 0x8c),
            .escape = 0x0f, .prefix = 0x66,
            .regField = store ? useRef(2) : defRef(0), .rmField = useRef(0),
            .width = quad ? OperationWidth::Fixed64 : OperationWidth::Fixed32,
            .prefixEncoding = PrefixEncoding::Vex,
            .opcodeMap = kOpcodeMap0F38,
        };
        form.encoding.vvvvField = useRef(1);

        widen(wideId, id, name256);
    };

    maskMove(FormVMaskMoveLoad32,  FormVWideMaskMoveLoad32,  OpVMaskedLoad,  "vpmaskmovd xmm, xmm, [address]"_v, "vpmaskmovd ymm, ymm, [address]"_v, false, false);
    maskMove(FormVMaskMoveLoad64,  FormVWideMaskMoveLoad64,  OpVMaskedLoad,  "vpmaskmovq xmm, xmm, [address]"_v, "vpmaskmovq ymm, ymm, [address]"_v, false, true);
    maskMove(FormVMaskMoveStore32, FormVWideMaskMoveStore32, OpVMaskedStore, "vpmaskmovd [address], xmm, xmm"_v, "vpmaskmovd [address], ymm, ymm"_v, true,  false);
    maskMove(FormVMaskMoveStore64, FormVWideMaskMoveStore64, OpVMaskedStore, "vpmaskmovq [address], xmm, xmm"_v, "vpmaskmovq [address], ymm, ymm"_v, true,  true);

    /*
     * The halves of a 512-bit register, and a register made of two - Analysis-Avx512.md §1.2's stage
     * 2. `splitFullVectors` writes an operation that has no 512-bit form as two 256-bit ones between
     * these, which are three width-changing shuffles `packedShuffleChoice` reads by their sources'
     * width.
     *
     * The low half is the register itself read at 256 bits, so it is a copy and nothing where the
     * allocator lands both ends in one register; the high one is `vextracti64x4 ymm, zmm, 1`
     * (EVEX.512.66.0F3A.W1 3B /r ib), and the join `vinserti64x4 zmm, zmm, ymm, 1` (3A), whose first
     * operand is the low half read as the 512-bit register it sits in - a 256-bit value's register
     * holds zeros above it, every write of one being VEX or EVEX, and the insert replaces them.
     */
    {
        auto& form = add(FormVFullLow, OpVShuffle, "vmovaps ymm, ymm (low half of zmm)"_v);
        form.uses.push(anyReg(ClassZmm512));
        form.defs.push(def(ClassYmm256));
        form.requiredFeatures = kFeatureAvx512f;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = 0x28,
            .escape = 0x0f,
            .regField = defRef(0), .rmField = useRef(0),
            .widthInPrefix = true,
            .prefixEncoding = PrefixEncoding::Vex,
            .vectorLength = 1,
            .omitWhenSame = true,
        };
    }

    {
        auto& form = add(FormVFullHigh, OpVShuffle, "vextracti64x4 ymm, zmm, 1"_v);
        form.uses.push(anyReg(ClassZmm512));
        form.defs.push(def(ClassYmm256));
        form.requiredFeatures = kFeatureAvx512f;
        form.encoding = evex(kOpcodeMap0F3A, 0x3b, true);
        form.encoding.regField = useRef(0);
        form.encoding.rmField = defRef(0);
        form.encoding.vectorLength = 2;
        form.encoding.patternImmediate = true;
    }

    {
        auto& form = add(FormVFullJoin, OpVShuffle, "vinserti64x4 zmm, zmm, ymm, 1"_v);
        form.uses.push(anyReg(ClassYmm256));
        form.uses.push(anyReg(ClassYmm256));
        form.defs.push(def(ClassZmm512));
        form.requiredFeatures = kFeatureAvx512f;
        form.encoding = evex(kOpcodeMap0F3A, 0x3a, true);
        form.encoding.vvvvField = useRef(0);
        form.encoding.rmField = useRef(1);
        form.encoding.vectorLength = 2;
        form.encoding.patternImmediate = true;
    }

    /*
     * The lane-count conversions at 512 bits, each one instruction - where the 256-bit tier builds the
     * same thing out of two in-lane interleaves and an exchange of halves (`expandLaneCountConversions`),
     * because AVX2 has no widening move into a `ymm` that reads a whole `ymm`.
     *
     * Widening is `vpmovzxbw`/`wd`/`dq` and `vpmovsx*` - EVEX.512.66.0F38 30/33/35 and 20/23/25, the
     * quadword pair at W0 - with the 512-bit destination in ModRM.reg and the 256-bit source in r/m.
     * Narrowing is the truncating `vpmovwb`/`dw`/`qd` of `FormVNarrow16To8` one width up: the source in
     * ModRM.reg, the destination r/m.
     */
    auto extend = [&](MachineFormId id, StringView formName, U8 op) {
        auto& form = add(id, OpCast, formName);
        form.uses.push(anyReg(ClassYmm256));
        form.defs.push(def(ClassZmm512));
        form.requiredFeatures = kFeatureAvx512f | kFeatureAvx512bw;
        form.encoding = evex(kOpcodeMap0F38, op, false);
        form.encoding.vectorLength = 2;
    };

    extend(FormVFullZeroExtend8,  "vpmovzxbw zmm, ymm"_v, 0x30);
    extend(FormVFullZeroExtend16, "vpmovzxwd zmm, ymm"_v, 0x33);
    extend(FormVFullZeroExtend32, "vpmovzxdq zmm, ymm"_v, 0x35);
    extend(FormVFullSignExtend8,  "vpmovsxbw zmm, ymm"_v, 0x20);
    extend(FormVFullSignExtend16, "vpmovsxwd zmm, ymm"_v, 0x23);
    extend(FormVFullSignExtend32, "vpmovsxdq zmm, ymm"_v, 0x25);

    auto narrowFull = [&](MachineFormId id, StringView formName, U8 op) {
        auto& form = add(id, OpCast, formName);
        form.uses.push(anyReg(ClassZmm512));
        form.defs.push(def(ClassYmm256));
        form.requiredFeatures = kFeatureAvx512f | kFeatureAvx512bw;
        form.encoding = evex(kOpcodeMap0F38, op, false);
        form.encoding.prefix = 0xf3;
        form.encoding.regField = useRef(0);
        form.encoding.rmField = defRef(0);
        form.encoding.width = OperationWidth::FromUse0;
        form.encoding.vectorLength = 2;
    };

    narrowFull(FormVFullNarrow16To8,  "vpmovwb ymm, zmm"_v, 0x30);
    narrowFull(FormVFullNarrow32To16, "vpmovdw ymm, zmm"_v, 0x33);
    narrowFull(FormVFullNarrow64To32, "vpmovqd ymm, zmm"_v, 0x35);
}

/*
 * The masked twin of every arithmetic form a mask register can mask - see MachineForm::masked.
 *
 * Swept rather than listed, on the VEX tier's argument: the twins are every form of a shape, and a list
 * would be the one place one was forgotten. The shape is the three-operand register form whose
 * destination is ModRM.reg, whose first operand is `vvvv` and whose second is r/m - which is every
 * VEX and EVEX packed binary operation here - at an operation whose element is its lane, so that a
 * mask bit per lane is a mask bit per element. That is add, subtract, multiply, divide, minimum and
 * maximum; the bitwise three are not among them, their one row serving every lane width at a 32-bit
 * element.
 *
 * The twin takes the mask as a third operand and writes its result over the first, which is what
 * leaves the first operand's lanes where the mask is clear. The comparisons into a mask register are
 * swept too, and their twin writes a fresh mask: EVEX masks a comparison by `and`ing its answer.
 */
void MachineFormBuilder::registerMaskedForms() {
    auto maskable = [&](const MachineForm& form) {
        switch(form.opcode) {
            case OpVAdd: case OpVSub: case OpVMul: case OpVDiv: case OpVMin: case OpVMax:
                break;

            // A comparison into a mask register, whose mask is `and`ed with its answer rather than
            // choosing between it and the destination - see LowerInst::X86MaskCompare.
            case OpVCmp:
                if(form.defs.size() != 1 || form.defs[0].regClass != ClassMask32) return false;
                break;

            default:
                return false;
        }

        auto& e = form.encoding;
        if(e.family != EncodingFamily::RegRm || e.prefixEncoding == PrefixEncoding::Legacy) return false;
        if(form.memorySourceOf || form.uses.size() != 2 || form.defs.size() != 1 || form.tiedResult() >= 0) return false;
        if(e.vvvvField != useRef(0) || e.rmField != useRef(1) || e.regField != defRef(0)) return false;
        if(!e.maskField.isNone()) return false;

        return e.prefixEncoding == PrefixEncoding::Evex
            || evexSpellingOf(e.prefix, e.opcodeMap, e.opcode, e.extension).exists;
    };

    auto described = forms.size();
    Size count = 0;
    Size nameBytes = 0;
    auto suffix = " {k}"_v;

    for(Size i = 0; i < described; i++) {
        if(!maskable(forms[i])) continue;
        count++;
        nameBytes += forms[i].name.length + suffix.length;
    }

    forms.reserve(U32(forms.size() + count));
    derivedNames.reserve(U32(derivedNames.size() + nameBytes));

    for(Size i = 0; i < described; i++) {
        if(!maskable(forms[i])) continue;

        auto source = forms[i];
        auto at = derivedNames.size();
        for(Size k = 0; k < source.name.length; k++) derivedNames.push(source.name.ptr[k]);
        for(Size k = 0; k < suffix.length; k++) derivedNames.push(suffix.ptr[k]);
        auto name = StringView { &derivedNames[at], source.name.length + suffix.length };

        auto id = MachineFormId(forms.size());
        auto& form = add(id, source.opcode, name);
        form = source;
        form.id = id;
        form.name = name;
        form.uses.push(anyReg(ClassMask32));
        form.encoding.maskField = useRef(2);

        // An operation writes its answer over the operand it keeps where the mask is clear; a
        // comparison writes a fresh mask, clear wherever the mask it was given is.
        if(source.opcode != OpVCmp) form.defs[0] = tiedDef(0, source.defs[0].regClass);

        // BW on every twin, which is the byte and word rows' requirement and more than the dword,
        // quadword and float ones need. The strictest of the set, for `hasEvexRow`'s reason: the
        // eleven bits move together today, and asking for too much is the direction that cannot
        // produce an instruction the machine will not decode.
        form.requiredFeatures |= kFeatureAvx512f | kFeatureAvx512vl | kFeatureAvx512bw;
        form.alternative = 0;
        form.alternativeOf = 0;
        form.memorySource = 0;
        form.wide = 0;
        form.wideOf = 0;
        form.masked = 0;

        forms[i].masked = id;
    }
}

/*
 * Answered from one row of the group, which has to be **the one requiring the most**: a caller asking
 * this is about to let selection reach any row in the group, so an answer taken from a weaker member
 * would admit an instruction the build cannot encode. `vpmovwb` stands for the narrowings and
 * `vpermi2w` and `vpbroadcastw` for their families for that reason - each is the BW row beside rows
 * needing only F. Every bit moves together while `avx512` is one level (see x64FeaturesFor), so this
 * is the rule the day one of them splits rather than a difference today.
 */
bool hasEvexRow(EvexRow row) {
    auto representative = [&]() -> MachineFormId {
        switch(row) {
            case EvexRow::QuadwordAbs:      return FormVAbs64;
            case EvexRow::QuadwordSar:      return FormVSar64Imm;
            case EvexRow::QuadwordMinMax:   return FormVMinI64;
            case EvexRow::QuadwordMul:      return FormVMul64;
            case EvexRow::WordShiftPerLane: return FormVShlVar16;
            case EvexRow::Rotate:           return FormVRol32Imm;
            case EvexRow::WordRotate:       return FormVRol16Imm;
            case EvexRow::QuadwordToDouble: return FormVCastI64ToF64;
            case EvexRow::PopcountNarrow:   return FormVPopcount8;
            case EvexRow::PopcountWide:     return FormVPopcount32;
            case EvexRow::LeadingZeros:     return FormVLeadingZeros32;
            case EvexRow::Narrowing:        return FormVNarrow16To8;
            case EvexRow::UnsignedToFloat:  return FormCastUToF64;
            case EvexRow::GprBroadcast:     return FormVBroadcastGpr16;
            case EvexRow::TernaryLogic:     return FormVTernaryLogic;
            case EvexRow::Align:            return FormVWideAlign32;
            case EvexRow::Permute2:         return FormVPermute2x16;
            case EvexRow::BytePermute2:     return FormVPermute2x8;
            case EvexRow::Compress:         return FormVCompress8;
            case EvexRow::MaskedMove:       return FormVMaskedLoad8;
        }

        return FormNop;
    }();

    // A function encoded without a vector prefix has no EVEX either - see `legacyVectorEncodings`.
    if(legacyVectorEncodings()) return false;
    return (machineTarget().form(representative).requiredFeatures & ~targetFeatures()) == 0;
}
