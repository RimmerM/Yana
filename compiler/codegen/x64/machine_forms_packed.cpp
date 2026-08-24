#include "machine_internal.h"

/*
 * The packed set, at 128 bits.
 *
 * One row per operation and lane width, and the source every other vector tier is derived from: the
 * 256-bit forms and the VEX twins in machine_forms_tiers.cpp are both built out of these rather than
 * written beside them, so that there is one statement of what `paddd` is.
 */

void MachineFormBuilder::registerPackedForms() {
/*
 * The packed set, at 128 bits.
 *
 * Every one of these is `op xmm, xmm/m` with the destination doubling as the first source, which
 * is the same two-address shape the scalar float arithmetic has and is described the same way.
 * What differs is only which bytes: an integer operation carries the 66 prefix, `addps` carries
 * none, and `addpd` carries 66 as well - so a lane type is a prefix and an opcode byte, and the
 * table below is that pair per lane.
 *
 * **These are the legacy spellings, and they are what a target without AVX selects.** Two tiers
 * are derived from each one: the 256-bit form below (`wideTwin`), which is this descriptor with
 * `vectorLength` set and its operands a class wider; and the 128-bit VEX form the sweep at the
 * end of this constructor builds, which is this descriptor with the prefix changed and the tie
 * gone. A target with AVX takes the second in place of every row here, so nothing below is
 * emitted by a build that has the extension - see validateMachineForms, which says so as a rule
 * rather than as a hope.
 */
    /*
     * A packed operation's operands and result are the whole register, which is ClassXmm128 -
     * the class a 128-bit vector and a mask over one both occupy (classForType).
     *
     * The right-hand side may be read out of a frame slot, and that is the first memory operand
     * in this table with an *alignment* requirement: a legacy-encoded packed operation faults on
     * a memory operand that is not 16-byte aligned, where every operand of eight bytes or fewer
     * has no such rule. A Slot128 is what makes it hold - a slot of that class raises the frame's
     * alignment, `computeFrameLayout` realigns the prologue for it, and the one case where that
     * cannot happen (a run-time allocation in the same function) is already reported as an
     * unsupported frame rather than emitted.
     *
     * What that same rule refuses is a memory *twin*: an address the program computed is one
     * nothing here has promised anything about, so `memoryTwin` is deliberately not called for
     * any of these and `tryFoldLoad` therefore leaves a vector load alone.
     *
     * The VEX forms are where that opens up - a VEX-encoded packed operation has no alignment
     * requirement at all - and the sweep at the end of this constructor builds that twin on the
     * VEX form alone, which is what makes the fold appear with the extension and not without it.
     * There is no feature test in `tryFoldLoad` for it: that pass asks the form `selectForm`
     * would choose, and here that is this row, which has no memory source to move onto.
     */
    auto packed = [&](MachineFormId id, MachineOpcodeId opcode, StringView formName, U8 prefix, U8 op,
                      U8 map = kOpcodeMap0F, FeatureSet features = 0) {
        auto& form = add(id, opcode, formName);
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(regOrMem(MemoryAccessKind::Read, ClassXmm128));
        form.defs.push(tiedDef(0, ClassXmm128));
        form.requiredFeatures = features;
        form.encoding = sseRegRm(prefix, op, useRef(0), useRef(1), OperationWidth::FromResult);

        // The map the opcode is in, which for most of this table is the two-byte one the default
        // names. The minimum and maximum are where that stops being true of a whole family: half
        // of their rows are SSE4.1 and so live in the three-byte 0F38 map, the other half are
        // SSE2 two-byte opcodes, and the two are the same instruction at a different lane.
        form.encoding.opcodeMap = map;
    };

    /*
     * `palignr xmm1, xmm2/m128, imm8` - SSSE3, and therefore inside the v2 floor.
     *
     * One row for every lane width, which is what makes it worth having: the instruction shifts the
     * *concatenation of the two registers* right by a number of bytes, so what a lane is is entirely
     * the immediate's business. `packedShuffleChoice` is what recognizes a window pattern and turns
     * the lane offset into that byte.
     *
     * Destructive two-address like every other row here, and the operand order is the machine's:
     * ModRM.reg is the *high* half of the concatenation and is also the destination.
     */
    {
        auto& form = add(FormVAlign, OpVShuffle, "palignr xmm, xmm/m, imm8"_v);
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(regOrMem(MemoryAccessKind::Read, ClassXmm128));
        form.defs.push(tiedDef(0, ClassXmm128));
        form.encoding = sseRegRm(0x66, 0x0f, useRef(0), useRef(1), OperationWidth::FromResult);
        form.encoding.opcodeMap = kOpcodeMap0F3A;
        form.encoding.patternImmediate = true;
    }

    /*
     * `pshufb xmm1, xmm2/m128` - `66 0F 38 00 /r`, SSSE3 - the general byte permutation.
     *
     * The one shuffle on this machine whose pattern is not part of the instruction. Result byte `i`
     * is source byte `ctrl[i] & 15`, or zero where `ctrl[i]`'s top bit is set - so the pattern is a
     * *value*, with a `.rodata` entry, a live range and a register of its own. `lowerByteLaneShuffles`
     * is what turns a `VecShuffle` into one, for the reason `lowerWideLanePermutes` exists at the
     * wide tier: a form cannot create an operand.
     *
     * **No feature bit.** SSSE3 is inside x86-64-v2 and this backend's floor is v2 - see the note on
     * kFeatureBaseline, which is why a row here asks only which of the levels *above* the floor a
     * target is. The comment in `narrowShuffleChoice` that called this SSSE3-and-unpooled predates
     * both the floor being named and `poolVectorConstants` opening the pool to vectors.
     *
     * **The operand order is `LowerInstX86Permute`'s and not the machine's**, which is the one place
     * this family departs from the rule the wide rows follow. That kind states indices first, and
     * `vpermd` happens to agree because its indices are `vvvv`; `pshufb` does not, because its
     * control is the r/m operand and the vector it permutes is both ModRM.reg and the destination.
     * Reordering the *instruction* for one row would make its two producers disagree about what
     * `indices` means, so the row is what bends: use 0 is the control and use 1 is the vector, the
     * destination is tied to use 1, and the encoding names them the machine's way round.
     *
     * The control stays `regOrMem` so that the pooled load folds into the addressing mode where the
     * encoding allows it - one instruction under VEX, and at the baseline the load stands, a legacy
     * packed operand having to be aligned.
     */
    {
        auto& form = add(FormVByteShuffle, OpVPermute, "pshufb xmm, xmm/m"_v);
        form.uses.push(regOrMem(MemoryAccessKind::Read, ClassXmm128));
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(tiedDef(1, ClassXmm128));
        form.encoding = sseRegRm(0x66, 0x00, useRef(1), useRef(0), OperationWidth::FromResult);
        form.encoding.opcodeMap = kOpcodeMap0F38;
    }


    // The integer widths of one operation, in lane order, and the two float ones. `0` for a lane
    // type the machine has no instruction for, which is a form this table does not build.
    packed(FormVAdd8,   OpVAdd, "paddb xmm, xmm/m"_v, 0x66, 0xfc);
    packed(FormVAdd16,  OpVAdd, "paddw xmm, xmm/m"_v, 0x66, 0xfd);
    packed(FormVAdd32,  OpVAdd, "paddd xmm, xmm/m"_v, 0x66, 0xfe);
    packed(FormVAdd64,  OpVAdd, "paddq xmm, xmm/m"_v, 0x66, 0xd4);
    packed(FormVAddF32, OpVAdd, "addps xmm, xmm/m"_v, 0x00, 0x58);
    packed(FormVAddF64, OpVAdd, "addpd xmm, xmm/m"_v, 0x66, 0x58);

    packed(FormVSub8,   OpVSub, "psubb xmm, xmm/m"_v, 0x66, 0xf8);
    packed(FormVSub16,  OpVSub, "psubw xmm, xmm/m"_v, 0x66, 0xf9);
    packed(FormVSub32,  OpVSub, "psubd xmm, xmm/m"_v, 0x66, 0xfa);
    packed(FormVSub64,  OpVSub, "psubq xmm, xmm/m"_v, 0x66, 0xfb);
    packed(FormVSubF32, OpVSub, "subps xmm, xmm/m"_v, 0x00, 0x5c);
    packed(FormVSubF64, OpVSub, "subpd xmm, xmm/m"_v, 0x66, 0x5c);

    // `pmullw` is the only packed integer multiply in the two-byte map: the 32-bit one is
    // `pmulld`, which is SSE4.1 and three bytes, and there is no byte or quadword multiply at
    // any feature level.
    packed(FormVMul16,  OpVMul, "pmullw xmm, xmm/m"_v, 0x66, 0xd5);

    // `pmulld`, which is SSE4.1 and is the ordinary two-address shape in the three-byte 0F38
    // map. The `packed` helper writes a two-byte opcode, so this one is spelled out.
    {
        auto& form = add(FormVMul32, OpVMul, "pmulld xmm, xmm/m"_v);
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(regOrMem(MemoryAccessKind::Read, ClassXmm128));
        form.defs.push(tiedDef(0, ClassXmm128));
        form.requiredFeatures = kFeatureBaseline;
        form.encoding = sseRegRm(0x66, 0x40, useRef(0), useRef(1), OperationWidth::FromResult);
        form.encoding.opcodeMap = kOpcodeMap0F38;
    }

    /*
     * And the same product without it, which is where SSE2 is at its least regular.
     *
     * `pmuludq` is the only 32-bit lane multiply the baseline has and it multiplies the *even*
     * lanes alone, widening each into a quadword. So the vector's odd lanes have to be brought
     * down into even positions, multiplied separately, and the two sets of low halves
     * interleaved back - which is two `pmuludq`s and five shuffles, and needs two scratch
     * registers because both halves are alive at once.
     *
     * Two clobbers rather than one, and they are the highest two registers for the reason
     * `FormVSelect` gives: placement reaches for those last, so a function that fits in the file
     * pays nothing for them.
     */
    {
        auto& form = add(FormVMul32Sse2, OpVMul, "pmuludq (32-bit lane product)"_v);
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(tiedDef(0, ClassXmm128));
        form.clobbers.add(vectorReg(14));
        form.clobbers.add(vectorReg(15));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::VecMul32,
        };
    }

    packed(FormVMulF32, OpVMul, "mulps xmm, xmm/m"_v, 0x00, 0x59);
    packed(FormVMulF64, OpVMul, "mulpd xmm, xmm/m"_v, 0x66, 0x59);

    /*
     * The top half of the product, which is the one arithmetic row where the signedness of the
     * operands changes the bits - the low half being the same either way, which is why `Mul` and
     * `IMul` share every row above.
     *
     * SSE2 and a 16-bit lane alone. There is no packed multiply of a byte or a quadword at all,
     * and the 32-bit lane's `pmulld` keeps the low half only: the widening `pmuludq` is where a
     * 32-bit lane's high half lives, and reaching it is the seven-instruction shuffle
     * `FormVMul32Sse2` is, run for the other half of the answer. So the family is these two.
     */
    packed(FormVMulHi16,  OpVMulHi,  "pmulhuw xmm, xmm/m"_v, 0x66, 0xe4);
    packed(FormVIMulHi16, OpVIMulHi, "pmulhw xmm, xmm/m"_v,  0x66, 0xe5);

    // The widening even-lane pair. `pmuludq` is a two-byte SSE2 opcode and `pmuldq` is SSE4.1 in
    // the three-byte 0F38 map, which is the same split the minimum and maximum have and for the
    // same reason: the unsigned one is old and the signed one arrived with the rest of SSE4.1.
    packed(FormVMulWide32,  OpVMulWide,  "pmuludq xmm, xmm/m"_v, 0x66, 0xf4);
    packed(FormVIMulWide32, OpVIMulWide, "pmuldq xmm, xmm/m"_v,  0x66, 0x28, kOpcodeMap0F38,
           kFeatureBaseline);

    // No packed integer divide exists on any x86. An integer vector division expands lane by
    // lane, per Design-Vector §3.1, which is a transform this backend does not have yet.
    packed(FormVDivF32, OpVDiv, "divps xmm, xmm/m"_v, 0x00, 0x5e);
    packed(FormVDivF64, OpVDiv, "divpd xmm, xmm/m"_v, 0x66, 0x5e);

    // The bitwise operations do not have a lane width, so `pand` serves an `i8x16`, an `i64x2`
    // and a mask alike. `pandn` computes `~lhs & rhs`, which is what the name does not say and
    // what a select expansion depends on.
    packed(FormVAnd,    OpVAnd,    "pand xmm, xmm/m"_v,  0x66, 0xdb);
    packed(FormVOr,     OpVOr,     "por xmm, xmm/m"_v,   0x66, 0xeb);
    packed(FormVXor,    OpVXor,    "pxor xmm, xmm/m"_v,  0x66, 0xef);
    packed(FormVAndNot, OpVAndNot, "pandn xmm, xmm/m"_v, 0x66, 0xdf);

    // The float domain's `and`, which the absolute value is: `v & 0x7fffffff` per lane clears
    // the sign bit and leaves a NaN, an infinity and a zero of either sign exactly where they
    // were. One instruction against the comparison, the subtraction and the blend it replaces.
    packed(FormVAndF32, OpVAnd, "andps xmm, xmm/m"_v, 0x00, 0x54);
    packed(FormVAndF64, OpVAnd, "andpd xmm, xmm/m"_v, 0x66, 0x54);

    // And the complement of the same, which the dropped arm of a masked float vector is.
    packed(FormVAndNotF32, OpVAndNot, "andnps xmm, xmm/m"_v, 0x00, 0x55);
    packed(FormVAndNotF64, OpVAndNot, "andnpd xmm, xmm/m"_v, 0x66, 0x55);

    /*
     * Shifts by a constant count every lane shares.
     *
     * The opcode byte is shared between the three shift directions and ModRM.reg carries the
     * extension that says which, exactly as the scalar group-2 shifts do. The register-count
     * rows are below, and are a different opcode rather than this one with an operand swapped.
     */
    auto shiftImm = [&](MachineFormId id, MachineOpcodeId opcode, StringView formName, U8 op, U8 extension) {
        auto& form = add(id, opcode, formName);
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(immediate(ImmediateWidth::Imm8));
        form.defs.push(tiedDef(0, ClassXmm128));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RmExtImm,
            .opcode = op,
            .escape = 0x0f, .prefix = 0x66, .extension = extension,
            .rmField = useRef(0), .immField = useRef(1),
            .width = OperationWidth::FromResult,
            .widthInPrefix = true,
        };
    };

    shiftImm(FormVShl16Imm, OpVShl, "psllw xmm, imm8"_v, 0x71, 6);
    shiftImm(FormVShl32Imm, OpVShl, "pslld xmm, imm8"_v, 0x72, 6);
    shiftImm(FormVShl64Imm, OpVShl, "psllq xmm, imm8"_v, 0x73, 6);
    shiftImm(FormVShr16Imm, OpVShr, "psrlw xmm, imm8"_v, 0x71, 2);
    shiftImm(FormVShr32Imm, OpVShr, "psrld xmm, imm8"_v, 0x72, 2);
    shiftImm(FormVShr64Imm, OpVShr, "psrlq xmm, imm8"_v, 0x73, 2);

    // There is no arithmetic shift of a quadword before AVX-512, and none of a byte at any
    // level, which is why this pair is two rows shorter than the two above it.
    shiftImm(FormVSar16Imm, OpVSar, "psraw xmm, imm8"_v, 0x71, 4);
    shiftImm(FormVSar32Imm, OpVSar, "psrad xmm, imm8"_v, 0x72, 4);

    /*
     * And the same three by a count that is not a constant, which used to be the one shift shape
     * this backend refused.
     *
     * The machine has it: `psllw xmm, xmm/m128` reads the **low quadword** of its second operand
     * as one count that every lane shares - not one count per lane, which is AVX2's `vpsllvd` and
     * a different instruction. So what was missing was never the instruction, it was the
     * transfer: the IR's shared count is a scalar in a *general* register, and getting it into
     * the low quadword of a vector one with everything above it clear is a `movd`/`movq`, whose
     * destination has to be a register that is neither operand.
     *
     * Hence a pseudo and a clobber, on exactly `FormVSelect`'s terms. And one row per direction
     * rather than per lane width: the three widths are `F1`/`F2`/`F3` off one base, which the
     * expansion reads off the instruction's own type, and nothing about the width changes an
     * operand, a tie or a clobber - which is all a form states.
     *
     * `movd` zero-extends, so a count arrives as an unsigned 64-bit number and a count at or
     * above the lane width answers zero (all sign bits, for `psra`). That is the same rule the
     * immediate rows already follow, `pslld xmm, 0xff` being zero and not a mask of the count.
     */
    auto shiftReg = [&](MachineFormId id, MachineOpcodeId opcode, StringView formName) {
        auto& form = add(id, opcode, formName);
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(anyReg());
        form.defs.push(tiedDef(0, ClassXmm128));
        form.clobbers.add(vectorReg(15));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::VecShiftCount,
        };
    };

    shiftReg(FormVShlReg, OpVShl, "movd xmm15, r; psllw/d/q xmm, xmm15"_v);
    shiftReg(FormVShrReg, OpVShr, "movd xmm15, r; psrlw/d/q xmm, xmm15"_v);
    shiftReg(FormVSarReg, OpVSar, "movd xmm15, r; psraw/d xmm, xmm15"_v);

    /*
     * A count *per lane*, which is AVX2's own family and has no spelling at all below it.
     *
     * Three-operand and non-destructive like everything VEX writes, and the operand order is the
     * IR's without rearrangement: `vpsllvd xmm1, xmm2, xmm3` shifts `xmm2` by `xmm3`, so the
     * value being shifted is `vvvv` and the counts are the r/m operand.
     *
     * **The lane width is `VEX.W` rather than an opcode byte**, which is the one thing about this
     * family that differs from every other packed row here: `vpsllvd` and `vpsllvq` are one
     * opcode and one bit apart. `Fixed32`/`Fixed64` is how that bit is stated, on the same terms
     * `pextrd`/`pextrq` state theirs - and `widthInPrefix` is deliberately left false, since here
     * the width *is* what the prefix bit means.
     *
     * Register operands only, as `vpermd` declares: each of these has a memory form, and a
     * memory twin would be derived from a row that has none to derive from.
     */
    auto shiftVar = [&](MachineFormId id, MachineOpcodeId opcode, StringView formName, U8 op,
                        bool wide) {
        auto& form = add(id, opcode, formName);
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(def(ClassXmm128));
        form.requiredFeatures = kFeatureAvx | kFeatureAvx2;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = op,
            .escape = 0x0f, .prefix = 0x66,
            .regField = defRef(0), .rmField = useRef(1),
            .width = wide ? OperationWidth::Fixed64 : OperationWidth::Fixed32,
            .prefixEncoding = PrefixEncoding::Vex,
            .opcodeMap = kOpcodeMap0F38,
        };
        form.encoding.vvvvField = useRef(0);
    };

    shiftVar(FormVShlVar32, OpVShl, "vpsllvd xmm, xmm, xmm"_v, 0x47, false);
    shiftVar(FormVShlVar64, OpVShl, "vpsllvq xmm, xmm, xmm"_v, 0x47, true);
    shiftVar(FormVShrVar32, OpVShr, "vpsrlvd xmm, xmm, xmm"_v, 0x45, false);
    shiftVar(FormVShrVar64, OpVShr, "vpsrlvq xmm, xmm, xmm"_v, 0x45, true);
    shiftVar(FormVSarVar32, OpVSar, "vpsravd xmm, xmm, xmm"_v, 0x46, false);

    /*
     * Comparison into a mask.
     *
     * The integer comparisons are two relations - equal and signed greater - and everything else
     * is reached by swapping the operands or inverting the result, which is what
     * `selectFormForTarget` does rather than this table. There is no unsigned packed compare at
     * all; the quadword pair needs SSE4.1 and SSE4.2 respectively, which is the floor.
     *
     * The float ones are one instruction carrying a predicate as an immediate, which is a
     * *selected* value rather than an operand of the IR: the condition comes from the
     * comparison's own `LowerCmp` and the encoder writes it. That is what `immediateBytes`
     * without an `immField` means here - a byte the encoding always writes and no operand
     * supplies - and `emitPackedCompare` is what supplies it.
     */
    packed(FormVCmpEq8,  OpVCmp, "pcmpeqb xmm, xmm/m"_v, 0x66, 0x74);
    packed(FormVCmpEq16, OpVCmp, "pcmpeqw xmm, xmm/m"_v, 0x66, 0x75);
    packed(FormVCmpEq32, OpVCmp, "pcmpeqd xmm, xmm/m"_v, 0x66, 0x76);
    packed(FormVCmpEq64, OpVCmp, "pcmpeqq xmm, xmm/m"_v, 0x66, 0x29, kOpcodeMap0F38, kFeatureBaseline);
    packed(FormVCmpGt8,  OpVCmp, "pcmpgtb xmm, xmm/m"_v, 0x66, 0x64);
    packed(FormVCmpGt16, OpVCmp, "pcmpgtw xmm, xmm/m"_v, 0x66, 0x65);
    packed(FormVCmpGt32, OpVCmp, "pcmpgtd xmm, xmm/m"_v, 0x66, 0x66);
    packed(FormVCmpGt64, OpVCmp, "pcmpgtq xmm, xmm/m"_v, 0x66, 0x37, kOpcodeMap0F38, kFeatureBaseline);

    auto floatCompare = [&](MachineFormId id, StringView formName, U8 prefix) {
        auto& form = add(id, OpVCmp, formName);
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(regOrMem(MemoryAccessKind::Read, ClassXmm128));
        form.defs.push(tiedDef(0, ClassXmm128));
        form.encoding = sseRegRm(prefix, 0xc2, useRef(0), useRef(1), OperationWidth::FromResult);
        form.encoding.conditionImmediate = true;
    };

    floatCompare(FormVCmpF32, "cmpps xmm, xmm/m, predicate"_v, 0x00);
    floatCompare(FormVCmpF64, "cmppd xmm, xmm/m, predicate"_v, 0x66);

    /*
     * The lane-wise minimum and maximum.
     *
     * The one packed family with a signedness as well as a lane width, and the machine's own
     * shape: `pminsw` and `pmaxub` are the two SSE2 rows, and SSE4.1 filled in the other four
     * integer pairs in the three-byte 0F38 map. Which of the pair a program reaches is the
     * *comparison* `selectPackedMinMax` recognized, not the lane type - an `i32x4` compared
     * unsigned takes `pminud` and one compared signed takes `pminsd`.
     *
     * `minps`/`maxps` are `0F 5D` and `0F 5F` with the mandatory prefix deciding the lane width,
     * exactly as the arithmetic above them is. Their NaN and signed-zero behaviour is what makes
     * the operand order load-bearing - see LowerInst::X86MinMax - and it is also what makes them
     * an exact replacement for `select(a < b, a, b)`: both answer the second operand when the
     * comparison is false, which is what an unordered comparison is.
     *
     * There is no quadword row. `pminsq` is AVX-512, so a 64-bit lane keeps the comparison and
     * the blend, which is what `selectPackedMinMax` declines to rewrite.
     */
    packed(FormVMinI8,  OpVMin, "pminsb xmm, xmm/m"_v, 0x66, 0x38, kOpcodeMap0F38, kFeatureBaseline);
    packed(FormVMinU8,  OpVMin, "pminub xmm, xmm/m"_v, 0x66, 0xda);
    packed(FormVMinI16, OpVMin, "pminsw xmm, xmm/m"_v, 0x66, 0xea);
    packed(FormVMinU16, OpVMin, "pminuw xmm, xmm/m"_v, 0x66, 0x3a, kOpcodeMap0F38, kFeatureBaseline);
    packed(FormVMinI32, OpVMin, "pminsd xmm, xmm/m"_v, 0x66, 0x39, kOpcodeMap0F38, kFeatureBaseline);
    packed(FormVMinU32, OpVMin, "pminud xmm, xmm/m"_v, 0x66, 0x3b, kOpcodeMap0F38, kFeatureBaseline);
    packed(FormVMinF32, OpVMin, "minps xmm, xmm/m"_v,  0x00, 0x5d);
    packed(FormVMinF64, OpVMin, "minpd xmm, xmm/m"_v,  0x66, 0x5d);

    packed(FormVMaxI8,  OpVMax, "pmaxsb xmm, xmm/m"_v, 0x66, 0x3c, kOpcodeMap0F38, kFeatureBaseline);
    packed(FormVMaxU8,  OpVMax, "pmaxub xmm, xmm/m"_v, 0x66, 0xde);
    packed(FormVMaxI16, OpVMax, "pmaxsw xmm, xmm/m"_v, 0x66, 0xee);
    packed(FormVMaxU16, OpVMax, "pmaxuw xmm, xmm/m"_v, 0x66, 0x3e, kOpcodeMap0F38, kFeatureBaseline);
    packed(FormVMaxI32, OpVMax, "pmaxsd xmm, xmm/m"_v, 0x66, 0x3d, kOpcodeMap0F38, kFeatureBaseline);
    packed(FormVMaxU32, OpVMax, "pmaxud xmm, xmm/m"_v, 0x66, 0x3f, kOpcodeMap0F38, kFeatureBaseline);
    packed(FormVMaxF32, OpVMax, "maxps xmm, xmm/m"_v,  0x00, 0x5f);
    packed(FormVMaxF64, OpVMax, "maxpd xmm, xmm/m"_v,  0x66, 0x5f);

    /*
     * The three signed relations that are the complement of one the machine has.
     *
     * `pcmpXX dst, rhs` and then the mask inverted, which needs an all-ones vector - and the way
     * to make one without a constant pool is to compare a register with itself, whatever it
     * holds. So this is the base comparison plus two instructions and one scratch register,
     * declared as a clobber for the reason `FormVSelect` above declares one.
     *
     * One form for every lane width. What the widths differ in is the opcode byte, which the
     * pseudo reads off the instruction's own type; the operands, the tie and the clobber are the
     * same for all of them, and those are what the allocator reads.
     */
    {
        auto& form = add(FormVCmpInverted, OpVCmp, "pcmpXX; pcmpeqd; pxor (inverted)"_v);
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(tiedDef(0, ClassXmm128));
        form.clobbers.add(vectorReg(15));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::VecCompareInverted,
        };
    }

    // And the complement on its own, which is the same two trailing instructions with nothing in
    // front of them. `not` over a mask is what reaches this - Design-Vector §3.2 makes it the
    // bitwise complement rather than `Bool`'s `xor 1`, because a mask lane is all-ones or
    // all-zeros and complementing one lands back inside the type.
    {
        auto& form = add(FormVNot, OpVNot, "pcmpeqd; pxor (complement)"_v);
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(tiedDef(0, ClassXmm128));
        form.clobbers.add(vectorReg(15));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::VecNot,
        };
    }

    /*
     * Lanes rearranged within one vector.
     *
     * `pshufd xmm, xmm/m128, imm8` is the one shuffle in this tier that is a true three-operand
     * instruction: the destination is written whole from the source, so there is no `tiedDef`
     * and no copy for the allocator to insert. That is worth more here than anywhere else in
     * this table, because a shuffle is what a splat, a lane extract and every level of a
     * reduction tree are built out of - a tie on it would put a copy in each of them.
     *
     * No memory operand. `pshufd` reads m128 quite happily, but a legacy-encoded packed read is
     * the one in this table with a 16-byte alignment requirement (see `packed` above), and the
     * *reason* the arithmetic can take a frame slot is that a Slot128 is aligned by
     * construction. A shuffle's source is as often a value the allocator left in a register, so
     * the memory form buys a fold this backend cannot yet perform and costs the same refusal
     * `memoryTwin` is deliberately not called for.
     */
    {
        auto& form = add(FormVShuffle32, OpVShuffle, "pshufd xmm, xmm, pattern"_v);
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(def(ClassXmm128));
        form.encoding = sseRegRm(0x66, 0x70, defRef(0), useRef(0), OperationWidth::FromResult);
        form.encoding.patternImmediate = true;
    }

    /*
     * The same instruction reading the *other* operand.
     *
     * A pattern naming only the second source is a one-source shuffle of a vector this
     * instruction happens to hold second, and nothing but which operand it reads distinguishes
     * it. So it is a form rather than an exchange in a pass: exchanging the operands would move
     * a use between two values and leave the pattern needing rewriting to match, where naming
     * the other operand in the encoding costs one row.
     *
     * Both operands are `anyReg` here where the form above lists one. The first is not read and
     * needs no register of its own, but it is not implicit either - the value is live and some
     * other instruction is why - so leaving it off the list would make it unconstrained rather
     * than absent, which is the same thing for placement and clearer for reading.
     */
    {
        auto& form = add(FormVShuffle32Second, OpVShuffle, "pshufd xmm, xmm, pattern (second source)"_v);
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(def(ClassXmm128));
        form.encoding = sseRegRm(0x66, 0x70, defRef(0), useRef(1), OperationWidth::FromResult);
        form.encoding.patternImmediate = true;
    }

    /*
     * Lanes taken from two vectors.
     *
     * `shufps` and `shufpd` are the general pair and are two-address: the destination is the
     * first source, its own lanes are the low half of the result, and the second source supplies
     * the high half. The control byte is two bits per result lane for `shufps` and one for
     * `shufpd`, which is exactly `pshufd`'s byte with the sides split.
     *
     * `shufps` on an integer vector is a domain crossing and is emitted anyway. What it would
     * cost is a forwarding penalty on some parts; what the alternative costs is a second family
     * of rows saying the same thing, and there is no integer `shufdq` to say it with.
     */
    auto shuffle2 = [&](MachineFormId id, StringView formName, U8 prefix) {
        auto& form = add(id, OpVShuffle, formName);
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(tiedDef(0, ClassXmm128));
        form.encoding = sseRegRm(prefix, 0xc6, useRef(0), useRef(1), OperationWidth::FromResult);
        form.encoding.patternImmediate = true;
    };

    shuffle2(FormVShuffle2F32, "shufps xmm, xmm, pattern"_v, 0x00);
    shuffle2(FormVShuffle2F64, "shufpd xmm, xmm, pattern"_v, 0x66);

    /*
     * The interleaves, which are the one two-source pattern the pair above cannot state: a lane
     * of each side alternating, where `shufps` takes a run from each.
     *
     * One form per lane width and per half, which is twelve rows and no choices - the machine
     * has an instruction for every one of them, including the two widths that have no shuffle at
     * all otherwise. That is worth stating: an 8- or 16-bit lane is refused everywhere else in
     * this table and is complete here, because interleaving needs no pattern byte and so needs
     * none of what `pshufb` would have supplied.
     */
    auto unpack = [&](MachineFormId id, StringView formName, U8 prefix, U8 opcode) {
        auto& form = add(id, OpVShuffle, formName);
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(tiedDef(0, ClassXmm128));
        form.encoding = sseRegRm(prefix, opcode, useRef(0), useRef(1), OperationWidth::FromResult);
    };

    unpack(FormVUnpackLow8,   "punpcklbw xmm, xmm"_v,  0x66, 0x60);
    unpack(FormVUnpackLow16,  "punpcklwd xmm, xmm"_v,  0x66, 0x61);
    unpack(FormVUnpackLow32,  "punpckldq xmm, xmm"_v,  0x66, 0x62);
    unpack(FormVUnpackLow64,  "punpcklqdq xmm, xmm"_v, 0x66, 0x6c);
    unpack(FormVUnpackLowF32, "unpcklps xmm, xmm"_v,   0x00, 0x14);
    unpack(FormVUnpackLowF64, "unpcklpd xmm, xmm"_v,   0x66, 0x14);

    unpack(FormVUnpackHigh8,   "punpckhbw xmm, xmm"_v,  0x66, 0x68);
    unpack(FormVUnpackHigh16,  "punpckhwd xmm, xmm"_v,  0x66, 0x69);
    unpack(FormVUnpackHigh32,  "punpckhdq xmm, xmm"_v,  0x66, 0x6a);
    unpack(FormVUnpackHigh64,  "punpckhqdq xmm, xmm"_v, 0x66, 0x6d);
    unpack(FormVUnpackHighF32, "unpckhps xmm, xmm"_v,   0x00, 0x15);
    unpack(FormVUnpackHighF64, "unpckhpd xmm, xmm"_v,   0x66, 0x15);

    /*
     * Every lane the same scalar.
     *
     * A pseudo, and the operand's *class* is what the four forms differ in - which is the whole
     * of why it is one: a float lane's scalar is already in a vector register and needs the
     * shuffle alone, an integer lane's is in a general one and needs `movd` or `movq` first.
     * Two instructions or one, decided by the bank, from one IR instruction.
     *
     * No clobber and no scratch, unlike the float-immediate pseudo it otherwise resembles: the
     * bank crossing writes the destination directly, so there is no third register in it.
     */
    auto broadcast = [&](MachineFormId id, StringView formName, RegisterClassId source,
                         FeatureSet features = 0, bool scratch = false) {
        auto& form = add(id, OpVBroadcast, formName);
        form.uses.push(anyReg(source));
        form.defs.push(def(ClassXmm128));
        form.requiredFeatures = features;

        // The byte broadcast's baseline route alone - `pshufb` shuffles by a *vector* of indices
        // and the indices wanted are zeros, which have to be somewhere. xmm15 for the reason
        // `FormVSelect` gives: it is the last register placement reaches for.
        if(scratch) form.clobbers.add(vectorReg(15));

        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::VecBroadcast,
        };
    };

    // The two narrow lanes, whose AVX2 row is one instruction after the crossing and whose
    // baseline row is the shuffle sequence that stands in for it. The byte's baseline route
    // needs a register of zeros to shuffle against, which is what the clobber is for; the word's
    // is two shuffles of the value itself and needs nothing.
    broadcast(FormVBroadcast8, "movd xmm, r32; vpbroadcastb xmm, xmm"_v, ClassGpr32,
              kFeatureAvx | kFeatureAvx2);
    broadcast(FormVBroadcast8Sse, "movd xmm, r32; pxor; pshufb (byte broadcast)"_v, ClassGpr32,
              0, true);
    broadcast(FormVBroadcast16, "movd xmm, r32; vpbroadcastw xmm, xmm"_v, ClassGpr32,
              kFeatureAvx | kFeatureAvx2);
    broadcast(FormVBroadcast16Sse, "movd xmm, r32; pshuflw; pshufd (word broadcast)"_v, ClassGpr32);

    broadcast(FormVBroadcast32,  "movd xmm, r32; pshufd xmm, xmm, 0"_v,  ClassGpr32);
    broadcast(FormVBroadcast64,  "movq xmm, r64; pshufd xmm, xmm, 0x44"_v, ClassGpr64);
    broadcast(FormVBroadcastF32, "pshufd xmm, xmm, 0"_v,    ClassFloat32);
    broadcast(FormVBroadcastF64, "pshufd xmm, xmm, 0x44"_v, ClassFloat64);

    /*
     * The two constants that need no pool - §5.7.
     *
     * One form each rather than a lane column, because neither pattern depends on the lane
     * width: all-zeros is all-zeros at every width and `pcmpeqd` answers all-ones for a byte
     * lane and a mask alike, which is what `emitAllOnes` already says. The operand is `folded()`
     * - the opcode *is* the value, so nothing about the scalar is encoded and it occupies no
     * location.
     *
     * A wide twin cannot be derived here, `wideTwin` rebuilding a form from a narrow one whose
     * operands move class: these have no register operand to move. So the 256-bit pair is
     * written out, which is two lines and states the one thing that differs - the class of the
     * destination.
     */
    auto constant = [&](MachineFormId id, StringView formName, PseudoKind kind, RegisterClassId cls,
                        U32 features) {
        auto& form = add(id, OpVBroadcast, formName);
        form.uses.push(folded());
        form.defs.push(def(cls));
        form.requiredFeatures = features;
        form.encoding = EncodingDescriptor { .family = EncodingFamily::Pseudo, .pseudo = kind };
    };

    constant(FormVZero, "pxor xmm, xmm (zero)"_v, PseudoKind::VecZero, ClassXmm128, 0);
    constant(FormVOnes, "pcmpeqd xmm, xmm (all ones)"_v, PseudoKind::VecOnes, ClassXmm128, 0);

    // The wide pair requires the extension that defines the *prefix* as well as the one that
    // defines the instruction - §5.4's rule, which `validateMachineForms` enforces and which
    // `kFeatureAvx2` alone passed every test in the default build without.
    constant(FormVWideZero, "vpxor ymm, ymm, ymm (zero)"_v, PseudoKind::VecZero, ClassYmm256,
             kFeatureAvx | kFeatureAvx2);
    constant(FormVWideOnes, "vpcmpeqd ymm, ymm, ymm (all ones)"_v, PseudoKind::VecOnes, ClassYmm256,
             kFeatureAvx | kFeatureAvx2);


    /*
     * One lane out of a vector.
     *
     * The integer forms put the *vector* in ModRM.reg and the general register in r/m, which is
     * the opposite way round from most of this table and is what `pextr` encodes: the
     * destination is the r/m operand. `regField` naming a use and `rmField` naming a def is
     * exactly that, and it is the reason these are written out rather than built by `sseRegRm`.
     *
     * `pextrd`/`pextrq` are three-byte opcodes in the 0F3A map, which is the first legacy
     * encoding here to use one - `writePrefix` reads `map` under a legacy prefix for it.
     */
    auto extractInt = [&](MachineFormId id, StringView formName, RegisterClassId cls, bool wide,
                          U8 opcode = 0x16) {
        auto& form = add(id, OpVExtract, formName);
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(def(cls));
        form.requiredFeatures = kFeatureBaseline;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = opcode,
            .escape = 0x0f, .prefix = 0x66,
            .regField = useRef(0), .rmField = defRef(0),
            .width = wide ? OperationWidth::Fixed64 : OperationWidth::Fixed32,
            .opcodeMap = kOpcodeMap0F3A,
        };
        form.encoding.patternImmediate = true;
    };

    /*
     * The narrow pair, which is the same shape with the opcode one and two lower in the 0F3A
     * map: `pextrb` is `14` and `pextrw` is `15` where `pextrd`/`pextrq` are `16`. Both write a
     * 32-bit register and zero the bits above the lane, which is why the width is `Fixed32` for
     * a lane that is eight or sixteen bits wide.
     *
     * `pextrw` has an older two-byte encoding as well (`66 0F C5`), which this deliberately does
     * not use: it is register-destination only, where the 0F3A row has a memory form the other
     * three extracts share, and both are SSE4.1-or-below so there is no feature to gain.
     *
     * The order of these four `add` calls is the order of their rows in the enum, which
     * `validateMachineForms` asserts - see the check at the top of this function.
     */
    extractInt(FormVExtract8,  "pextrb r32, xmm, lane"_v, ClassGpr32, false, 0x14);
    extractInt(FormVExtract16, "pextrw r32, xmm, lane"_v, ClassGpr32, false, 0x15);
    extractInt(FormVExtract32, "pextrd r32, xmm, lane"_v, ClassGpr32, false);
    extractInt(FormVExtract64, "pextrq r64, xmm, lane"_v, ClassGpr64, true);

    /*
     * The baseline's lane zero, which is the same direction with a two-byte opcode and no index:
     * `movd`/`movq` move the low element and have no way to name another. Every other index at
     * this feature level is refused - see unsupportedVectorReason - because reaching one needs a
     * shuffle into a vector register that is neither operand, and a form cannot declare a
     * temporary (validateMachineForms).
     */
    auto extractZero = [&](MachineFormId id, StringView formName, RegisterClassId cls, bool wide) {
        auto& form = add(id, OpVExtract, formName);
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(def(cls));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = 0x7e,
            .escape = 0x0f, .prefix = 0x66,
            .regField = useRef(0), .rmField = defRef(0),
            .width = wide ? OperationWidth::Fixed64 : OperationWidth::Fixed32,
        };
    };

    extractZero(FormVExtract32Zero, "movd r32, xmm"_v, ClassGpr32, false);
    extractZero(FormVExtract64Zero, "movq r64, xmm"_v, ClassGpr64, true);

    /*
     * A mask's bytes, as bits of a general register - `pmovmskb`, `66 0F D7 /r`, SSE2.
     *
     * The ordinary extract direction, and the same shape `movd r32, xmm` above has: the general
     * register is what it writes so it sits in ModRM.reg, and the vector it reads is r/m. Fixed
     * at 32 bits on both sides of the tier - sixteen bits are set at 128 and thirty-two at 256,
     * and neither needs REX.W to hold.
     *
     * There is no memory form: `pmovmskb` takes a register operand only, which is why this row
     * declares no `memorySource` twin where the other packed rows do.
     */
    {
        auto& form = add(FormVMaskBits, OpVMaskBits, "pmovmskb r32, xmm"_v);
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(def(ClassGpr32));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = 0xd7,
            .escape = 0x0f, .prefix = 0x66,
            .regField = defRef(0), .rmField = useRef(0),
            .width = OperationWidth::Fixed32,
        };
    }

    /*
     * The same movemask at one bit per lane - `movmskps`, `0F 50 /r`, and `movmskpd`, `66 0F 50
     * /r`. Both SSE2, so neither costs a feature.
     *
     * They read the *sign bit* of each 32- or 64-bit element, which for a mask is the lane: a
     * mask lane is all-ones or all-zeros by construction, so its sign bit is its truth value.
     * What that buys is the shift underneath every consumer. `pmovmskb` answers a bit per byte,
     * so a four-byte lane contributed four equal bits and `count` divided them back out with a
     * `shr $0x2` and `firstSet` with the same shift - one instruction per consumer, in the loop,
     * for a repacking nobody asked for. Here the bitmap the consumers want *is* the answer.
     *
     * A float-domain instruction over an integer vector is deliberate and is what LLVM's own
     * selection does: a movemask is a read, so the domain crossing costs a forwarding delay at
     * worst and there is no `pmovmskd` to cross back to.
     *
     * The 16-bit lane keeps `pmovmskb` and its shift, there being no `movmskw`; the 8-bit lane
     * keeps it because a bit per byte is already a bit per lane.
     */
    {
        auto& form = add(FormVMaskBitsF32, OpVMaskBits, "movmskps r32, xmm"_v);
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(def(ClassGpr32));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = 0x50,
            .escape = 0x0f, .prefix = 0x00,
            .regField = defRef(0), .rmField = useRef(0),
            .width = OperationWidth::Fixed32,
        };
    }

    {
        auto& form = add(FormVMaskBitsF64, OpVMaskBits, "movmskpd r32, xmm"_v);
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(def(ClassGpr32));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = 0x50,
            .escape = 0x0f, .prefix = 0x66,
            .regField = defRef(0), .rmField = useRef(0),
            .width = OperationWidth::Fixed32,
        };
    }

    // A float lane never leaves the vector bank, so the extract is the shuffle that brings the
    // wanted lane down to lane zero - no feature, no bank crossing, and the same instruction the
    // broadcast uses with the index generalized.
    auto extractFloat = [&](MachineFormId id, StringView formName, RegisterClassId cls) {
        auto& form = add(id, OpVExtract, formName);
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(def(cls));
        form.encoding = sseRegRm(0x66, 0x70, defRef(0), useRef(0), OperationWidth::FromUse0);
        form.encoding.patternImmediate = true;
    };

    extractFloat(FormVExtractF32, "pshufd xmm, xmm, lane"_v, ClassFloat32);
    extractFloat(FormVExtractF64, "pshufd xmm, xmm, lane"_v, ClassFloat64);

    /*
     * And lane *zero* of a float vector, which is the register it is already in.
     *
     * A scalar float lives in the low lane, so reading lane zero out of a vector moves no bits
     * at all - the shuffle above spends four bytes rearranging a register into itself. What it
     * is instead is the vector bank's copy, and `omitWhenSame` is what makes that free: the
     * allocator's `copyHint` gives a result the register its source is vacating, so the copy is
     * usually between one register and itself and emits nothing.
     *
     * That is the whole of "the low scalar coalesces into the return register" - a reduction
     * ending in `horizontalSum` hands its answer to a `ret`, the return register is where the
     * convention wants it, and nothing now stands between the two claiming to be a shuffle.
     *
     * `movaps` rather than `movss`, and the same argument `FormVBitcast` makes one row up: the
     * two are the same length, `movss` merges into its destination where this does not care,
     * and a lane read is by construction a value about to be read as something other than what
     * produced it - so neither forwarding domain is the right guess and the shorter dependency
     * is the better default.
     */
    auto extractFloatLow = [&](MachineFormId id, StringView formName, RegisterClassId cls) {
        auto& form = add(id, OpVExtract, formName);
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(def(cls));
        form.encoding = sseRegRm(0x00, 0x28, defRef(0), useRef(0), OperationWidth::FromResult);
        form.encoding.omitWhenSame = true;
    };

    extractFloatLow(FormVExtractF32Low, "movaps xmm, xmm"_v, ClassFloat32);
    extractFloatLow(FormVExtractF64Low, "movaps xmm, xmm"_v, ClassFloat64);

    /*
     * One lane into a vector.
     *
     * The direction is the ordinary one and the opposite of the extract's: `pinsr` puts the
     * *vector* in ModRM.reg and the scalar in r/m, because the vector is what it writes. So these
     * are `sseRegRm`'s shape with a trailing index byte, and the only reason they are written out
     * is REX.W - `pinsrq` states its width there, where every packed form above states it in the
     * mandatory prefix, so `widthInPrefix` is off and the width is a real one.
     *
     * Two-address at every width. `tiedDef(0)` is the vector the insert reads and writes, and a
     * vector still live after one gets the copy the allocator inserts for the tie. There is no
     * three-operand spelling before VEX and nothing here can avoid it.
     */
    auto insertInt = [&](MachineFormId id, StringView formName, RegisterClassId cls, U8 opcode,
                         U8 map, OperationWidth width, FeatureSet features)
    {
        auto& form = add(id, OpVInsert, formName);
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(anyReg(cls));
        form.defs.push(tiedDef(0, ClassXmm128));
        form.requiredFeatures = features;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = opcode,
            .escape = 0x0f, .prefix = 0x66,
            .regField = useRef(0), .rmField = useRef(1),
            .width = width,
            .opcodeMap = map,
        };
        form.encoding.patternImmediate = true;
    };

    // `pinsrw` is two bytes and needs nothing; the other three are SSE4.1 in the 0F3A map. The
    // scalar is a 32-bit register for every lane but the quadword, which is what `scalarFormOf`
    // already says an 8- or 16-bit lane arrives in.
    insertInt(FormVInsert8, "pinsrb xmm, r32, lane"_v, ClassGpr32, 0x20, kOpcodeMap0F3A,
              OperationWidth::Fixed32, kFeatureBaseline);
    insertInt(FormVInsert16, "pinsrw xmm, r32, lane"_v, ClassGpr32, 0xc4, kOpcodeMap0F,
              OperationWidth::Fixed32, kFeatureBaseline);
    insertInt(FormVInsert32, "pinsrd xmm, r32, lane"_v, ClassGpr32, 0x22, kOpcodeMap0F3A,
              OperationWidth::Fixed32, kFeatureBaseline);
    insertInt(FormVInsert64, "pinsrq xmm, r64, lane"_v, ClassGpr64, 0x22, kOpcodeMap0F3A,
              OperationWidth::Fixed64, kFeatureBaseline);

    /*
     * A float lane, which never crosses a bank and so is a different instruction at each width.
     *
     * `insertps` is the general one and is SSE4.1: its byte is three fields rather than an index -
     * which lane of the source, which lane of the destination, and which lanes to zero - and
     * `packedTrailingByte` is where the second of those becomes `lane << 4`.
     *
     * At the baseline there are exactly two lanes that have an instruction, and a two-lane vector
     * is made of both of them: `movsd xmm, xmm` merges the low quadword and leaves the high one,
     * and `unpcklpd` takes the low quadword of each. So a `f64x2` is complete without SSE4.1 and
     * an `f32x4` reaches lane zero only, by the same `movss` that is already a scalar copy here.
     */
    auto insertFloat = [&](MachineFormId id, StringView formName, RegisterClassId cls, U8 prefix,
                           U8 opcode, U8 map, bool pattern, FeatureSet features)
    {
        auto& form = add(id, OpVInsert, formName);
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(anyReg(cls));
        form.defs.push(tiedDef(0, ClassXmm128));
        form.requiredFeatures = features;
        form.encoding = sseRegRm(prefix, opcode, useRef(0), useRef(1), OperationWidth::FromResult);
        form.encoding.opcodeMap = map;
        form.encoding.patternImmediate = pattern;
    };

    insertFloat(FormVInsertF32, "insertps xmm, xmm, lane"_v, ClassFloat32, 0x66, 0x21,
                kOpcodeMap0F3A, true, kFeatureBaseline);
    insertFloat(FormVInsertF32Low, "movss xmm, xmm"_v, ClassFloat32, 0xf3, 0x10,
                kOpcodeMap0F, false, kFeatureBaseline);
    insertFloat(FormVInsertF64Low, "movsd xmm, xmm"_v, ClassFloat64, 0xf2, 0x10,
                kOpcodeMap0F, false, kFeatureBaseline);
    insertFloat(FormVInsertF64High, "unpcklpd xmm, xmm"_v, ClassFloat64, 0x66, 0x14,
                kOpcodeMap0F, false, kFeatureBaseline);

    /*
     * A lane-wise select.
     *
     * `(mask & a) | (~mask & b)`, which is three instructions and one value more than there are
     * registers to hold it: the destination is `a`, the mask has to survive being read twice,
     * and `~mask & b` has to be computed somewhere that is neither. So the form declares a
     * clobber and the expansion uses it as scratch - the arrangement `FormImmFloat32` already
     * has one bank over, and for the same reason: a clobber keeps a live value out of one
     * register at this one instruction, where `MachineForm::temporaries` would hold one back
     * from the whole function.
     *
     * xmm15 rather than xmm0, which is what a later `blendvps` form would want: the highest
     * register is the last one placement reaches for, so a function that fits in the file loses
     * nothing to this.
     *
     * The operand order is the instruction's - `lhs, rhs, cmp` - so use 0 is the value taken
     * where the mask is set, and the tie is on it because `pand` writes what it reads.
     */
    {
        auto& form = add(FormVSelect, OpVBlend, "pand; pandn; por (lanewise select)"_v);
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(tiedDef(0, ClassXmm128));
        form.clobbers.add(vectorReg(15));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::VecSelect,
        };
    }

    /*
     * The same select as `pblendvb`, which is SSE4.1 and therefore inside the floor §38 fixed.
     *
     * `PBLENDVB xmm1, xmm2/m128, <XMM0>` keeps its destination's byte where the mask byte's sign
     * bit is clear and takes the source's where it is set - so two instructions, `movaps xmm0,
     * mask` and the blend, replace the four the sequence above needs. Nine bytes against
     * seventeen at the widest, and one dependency chain shorter.
     *
     * **The tie is on the other operand**, and that is why this is a row of its own rather than
     * an `alternative` of the one above. `pand` writes the value taken where the mask is *set*,
     * so that sequence's destination is `whenTrue`; `pblendvb` preserves what it already holds
     * where the mask is clear, so this one's destination is `whenFalse`. An alternative is a
     * swap `selectForm` may perform after allocation, and the allocator has to have been told
     * which operand it is writing over before it places anything.
     *
     * **The cost is xmm0**, which is not the register a scratch should be: it is the first one
     * placement reaches for, where `FormVSelect`'s xmm15 is the last. §34.3 declined the row for
     * that reason and did not measure it. Measured, the trade is one-sided: a clobber holds a
     * register back at *one instruction*, and two instructions are removed at every select in a
     * language whose vector library selects in every masked tail. A function under enough
     * pressure to want xmm0 across a select is a function spilling anyway.
     *
     * Legacy only. Under any VEX build `emitVecSelect` writes `vpblendvb`, which takes the mask
     * as an ordinary third operand and needs neither the row nor the move.
     */
    {
        auto& form = add(FormVSelectBlend, OpVBlend, "movaps xmm0, mask; pblendvb xmm, xmm (lanewise select)"_v);
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(anyReg(ClassXmm128));
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(tiedDef(1, ClassXmm128));
        form.clobbers.add(vectorReg(0));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::VecSelectBlend,
        };
    }

    /*
     * A vector read as a vector of another lane shape.
     *
     * The same bits in the same register, so this is the vector bank's copy with `omitWhenSame` -
     * the identical arrangement `FormBitcastF32` has one class over, and for the identical
     * reason: what makes it a form rather than nothing is that the allocator has usually put
     * source and destination in one register and then it emits no bytes at all.
     *
     * `movaps` and not `movdqa`, which are the same length and differ in the forwarding domain -
     * and a bitcast is by construction a value about to be read as something other than what
     * produced it, so neither domain is the right guess.
     */
    {
        auto& form = add(FormVBitcast, OpBitcast, "movaps xmm, xmm"_v);
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(def(ClassXmm128));
        form.encoding = sseRegRm(0x00, 0x28, defRef(0), useRef(0), OperationWidth::FromResult);
        form.encoding.omitWhenSame = true;
    }

    // A vector copied. `FormVBitcast` above is the identical instruction, and this is a separate
    // row because a form belongs to one opcode: what the two say about the machine is the same
    // thing, and what they say about the IR is not.
    {
        auto& form = add(FormVMove, OpMove, "movaps xmm, xmm"_v);
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(def(ClassXmm128));
        form.encoding = sseRegRm(0x00, 0x28, defRef(0), useRef(0), OperationWidth::FromResult);
        form.encoding.omitWhenSame = true;
    }

    /*
     * A vector negated, which is two different operations wearing one name.
     *
     * An integer lane is subtracted from zero, and the zero has to be somewhere - so the
     * expansion is `pxor scratch, scratch; psub scratch, v; movaps dst, scratch`. Doing it in
     * the destination instead would be one instruction shorter and wrong: the allocator is
     * entitled to give the destination the source's own register, and `pxor dst, dst` would then
     * clear the value about to be read.
     *
     * A float lane is its sign bit toggled, and the mask of sign bits is buildable without a
     * constant pool for the same reason the complement is: all ones shifted left by 31 (or 63)
     * is exactly one sign bit per lane. That is the vector answer to what `FloatNeg` does one
     * bank over by toggling a bit in r11.
     */
    auto negate = [&](MachineFormId id, StringView formName) {
        auto& form = add(id, OpVNeg, formName);
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(def(ClassXmm128));
        form.clobbers.add(vectorReg(15));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::VecNegate,
        };
    };

    negate(FormVNeg8,   "pxor; psubb (negate)"_v);
    negate(FormVNeg16,  "pxor; psubw (negate)"_v);
    negate(FormVNeg32,  "pxor; psubd (negate)"_v);
    negate(FormVNeg64,  "pxor; psubq (negate)"_v);
    negate(FormVNegF32, "pcmpeqd; pslld; xorps (negate)"_v);
    negate(FormVNegF64, "pcmpeqd; psllq; xorpd (negate)"_v);

    /*
     * The magnitude of an integer lane, which is an ordinary form and not a pseudo: `pabsb`,
     * `pabsw` and `pabsd` are SSSE3 - inside the v2 floor - and are non-destructive, writing a
     * destination that need not be either operand.
     *
     * Three rows and not four. There is no `pabsq` outside AVX-512, so a 64-bit lane keeps the
     * comparison and the select the magnitude was written as; `unsupportedVectorReason` states
     * that from the other side, since the comparison it falls back on is itself missing at that
     * width below SSE4.2.
     */
    auto absolute = [&](MachineFormId id, StringView formName, U8 opcode) {
        auto& form = add(id, OpVAbs, formName);
        form.uses.push(regOrMem(MemoryAccessKind::Read, ClassXmm128));
        form.defs.push(def(ClassXmm128));
        form.requiredFeatures = kFeatureBaseline;
        form.encoding = sseRegRm(0x66, opcode, defRef(0), useRef(0), OperationWidth::FromResult);
        form.encoding.opcodeMap = kOpcodeMap0F38;
    };

    absolute(FormVAbs8,  "pabsb xmm, xmm/m"_v,  0x1c);
    absolute(FormVAbs16, "pabsw xmm, xmm/m"_v,  0x1d);
    absolute(FormVAbs32, "pabsd xmm, xmm/m"_v,  0x1e);

    /*
     * A packed conversion between the two lane kinds.
     *
     * `cvtdq2ps` and `cvttps2dq` convert four lanes at a time and are the only pair at this
     * register width: every other lane-count-preserving conversion changes the lane's width as
     * well as its kind, and four 64-bit lanes are two registers.
     *
     * Truncating rather than rounding, which is what a conversion means here. What it does *not*
     * do is saturate - an out-of-range lane becomes the integer indefinite value - and that is
     * `expandPackedConversions`' job rather than this form's, exactly as the scalar direction is
     * `expandFloatToSigned`'s rather than `cvttsd2si`'s.
     */
    auto packedConvert = [&](MachineFormId id, StringView formName, U8 prefix, U8 opcode) {
        auto& form = add(id, OpCast, formName);
        form.uses.push(anyReg(ClassXmm128));
        form.defs.push(def(ClassXmm128));
        form.encoding = sseRegRm(prefix, opcode, defRef(0), useRef(0), OperationWidth::FromResult);
    };

    packedConvert(FormVCastIToF32, "cvtdq2ps xmm, xmm"_v, 0x00, 0x5b);
    packedConvert(FormVCastFToI32, "cvttps2dq xmm, xmm"_v, 0xf3, 0x5b);

    /*
     * The square root, at four widths and one opcode.
     *
     * `0F 51` with the mandatory prefix deciding which of the four: none for `sqrtps`, 66 for
     * `sqrtpd`, F3 for `sqrtss`, F2 for `sqrtsd`. Non-destructive at every one of them - the
     * destination is ModRM.reg and need not be the source - which is the shape `pshufd` has and
     * almost nothing else in this table does.
     *
     * The scalar forms take a `ClassFloat32`/`ClassFloat64` operand and the packed ones take a
     * `ClassXmm128`, which is the whole of the difference between them here: the same instruction
     * writes one lane or four, and the register class is what says which.
     */
    auto squareRoot = [&](MachineFormId id, StringView formName, U8 prefix, RegisterClassId cls) {
        auto& form = add(id, OpSqrt, formName);
        form.uses.push(anyReg(cls));
        form.defs.push(def(cls));
        form.encoding = sseRegRm(prefix, 0x51, defRef(0), useRef(0), OperationWidth::FromResult);

        // The scalar pair writes one lane and leaves the rest, so its VEX spelling names where
        // the rest comes from; the packed pair writes the whole register and requires the field
        // that would say so to be unused. One opcode, two answers - see mergesIntoDestination.
        form.encoding.mergesIntoDestination = cls == ClassFloat32 || cls == ClassFloat64;
    };

    squareRoot(FormSqrt32,   "sqrtss xmm, xmm"_v, 0xf3, ClassFloat32);
    squareRoot(FormSqrt64,   "sqrtsd xmm, xmm"_v, 0xf2, ClassFloat64);
    squareRoot(FormVSqrtF32, "sqrtps xmm, xmm"_v, 0x00, ClassXmm128);
    squareRoot(FormVSqrtF64, "sqrtpd xmm, xmm"_v, 0x66, ClassXmm128);

    /*
     * The directed roundings, at four widths and four opcodes that differ in one nibble.
     *
     * `66 0F 3A 08..0B` - `roundps`, `roundpd`, `roundss`, `roundsd` in that order - each with
     * the 66 prefix and each in the `0F3A` map, which is the one three-byte map whose members
     * all carry a trailing immediate. That immediate is the rounding mode and is supplied by
     * `packedTrailingByte` from the IR kind, which is why three IR kinds share one form per
     * width instead of there being twelve of them.
     *
     * SSE4.1, which is inside the v2 floor this backend already required before the level enum
     * existed - so `kFeatureBaseline`, exactly as `pcmpeqq` above is.
     *
     * `mergesIntoDestination` for the scalar pair on `sqrtss`'s grounds: `roundss` writes one
     * lane and leaves the rest, so its VEX spelling has to name where the rest comes from.
     */
    auto rounding = [&](MachineFormId id, StringView formName, U8 prefix, U8 opcode, RegisterClassId cls) {
        auto& form = add(id, OpRound, formName);
        form.uses.push(anyReg(cls));
        form.defs.push(def(cls));
        form.encoding = sseRegRm(prefix, opcode, defRef(0), useRef(0), OperationWidth::FromResult);
        form.encoding.opcodeMap = kOpcodeMap0F3A;
        form.encoding.patternImmediate = true;
        form.encoding.mergesIntoDestination = cls == ClassFloat32 || cls == ClassFloat64;
    };

    rounding(FormRound32,   "roundss xmm, xmm, mode"_v, 0x66, 0x0a, ClassFloat32);
    rounding(FormRound64,   "roundsd xmm, xmm, mode"_v, 0x66, 0x0b, ClassFloat64);
    rounding(FormVRoundF32, "roundps xmm, xmm, mode"_v, 0x66, 0x08, ClassXmm128);
    rounding(FormVRoundF64, "roundpd xmm, xmm, mode"_v, 0x66, 0x09, ClassXmm128);

    /*
     * The fused multiply-add.
     *
     * `vfmadd213` computes `dst = src1 * dst + src2` - the digits name which operand is which -
     * so with the destination tied to the first operand it is exactly `a * b + c`: the tie makes
     * `dst` be `a`, VEX.vvvv names `b`, and ModRM.rm names `c`.
     *
     * VEX-encoded and three-byte, in the 0F38 map, and the *only* forms here that state
     * `vvvvField` by hand rather than having `prefixedTwin` derive one - there being no legacy
     * form for it to be derived from. `evexWideElement` follows `widthInPrefix` for the reason
     * §3.5.4 records: a double-precision FMA is `W1` and getting it wrong is a reserved encoding
     * rather than the single-precision instruction.
     */
    auto fusedMultiplyAdd = [&](MachineFormId id, StringView formName, bool wide, RegisterClassId cls) {
        auto& form = add(id, OpFma, formName);
        form.uses.push(anyReg(cls));
        form.uses.push(anyReg(cls));
        form.uses.push(anyReg(cls));
        form.defs.push(tiedDef(0, cls));

        // Both, and `validateMachineForms` is what says so: a form written with a vector prefix
        // has to require the extension that *defines the prefix*, whatever else it also needs.
        // FMA3 implies AVX on every part that has it and `x64FeaturesFor` will not claim one
        // without the other - but a form's features are what selection reads, and a rule the
        // settings happen to maintain is not the same as one the form states.
        form.requiredFeatures = kFeatureAvx | kFeatureFma3;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRm,

            // A9 is the scalar pair and A8 the packed one, which is the one place the two rows
            // differ in more than a register class.
            .opcode = cls == ClassXmm128 ? U8(0xa8) : U8(0xa9),
            .escape = 0x0f, .prefix = 0x66,
            .regField = defRef(0), .rmField = useRef(2),
            .width = wide ? OperationWidth::Fixed64 : OperationWidth::Fixed32,
            .widthInPrefix = false,
            .prefixEncoding = PrefixEncoding::Vex,
            .opcodeMap = kOpcodeMap0F38,
        };

        form.encoding.vvvvField = useRef(1);
    };

    fusedMultiplyAdd(FormFma32,   "vfmadd213ss xmm, xmm, xmm"_v, false, ClassFloat32);
    fusedMultiplyAdd(FormFma64,   "vfmadd213sd xmm, xmm, xmm"_v, true,  ClassFloat64);
    fusedMultiplyAdd(FormVFmaF32, "vfmadd213ps xmm, xmm, xmm"_v, false, ClassXmm128);
    fusedMultiplyAdd(FormVFmaF64, "vfmadd213pd xmm, xmm, xmm"_v, true,  ClassXmm128);

    /*
     * A whole vector between a register and memory.
     *
     * Two spellings of one operation: `movups` is in the float domain and `movdqu` in the
     * integer one, and on every part since Nehalem the only difference is a forwarding penalty
     * for a value produced in one domain and consumed in the other. Selecting by the lane type
     * is free and is what avoids it.
     *
     * Unaligned in both cases. The aligned encodings *fault* on a misaligned address rather than
     * running slowly, and nothing here has said anything about what a program's pointer is
     * aligned to - Design-Vector §6 caps a vector's own alignment at 16 and `@align(n)` is what
     * raises it, neither of which is a promise about an arbitrary address.
     */
    auto vectorLoad = [&](MachineFormId id, StringView formName, U8 opcode, U8 prefix) {
        auto& form = add(id, OpLoad, formName);
        form.uses.push(address());
        form.defs.push(def(ClassXmm128));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::LoadStore,
            .opcode = opcode, .escape = 0x0f, .prefix = prefix,
            .regField = defRef(0),
            .widthInPrefix = true,
        };
    };

    vectorLoad(FormVLoad, "movups xmm, [address]"_v, 0x10, 0x00);
    vectorLoad(FormVLoadInt, "movdqu xmm, [address]"_v, 0x6f, 0xf3);

    auto vectorStore = [&](MachineFormId id, StringView formName, U8 opcode, U8 prefix) {
        auto& form = add(id, OpStore, formName);
        form.uses.push(address());
        form.uses.push(anyReg(ClassXmm128));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::LoadStore,
            .opcode = opcode, .escape = 0x0f, .prefix = prefix,
            .regField = useRef(1),
            .width = OperationWidth::FromUse1,
            .widthInPrefix = true,
        };
    };

    vectorStore(FormVStore, "movups [address], xmm"_v, 0x11, 0x00);
    vectorStore(FormVStoreInt, "movdqu [address], xmm"_v, 0x7f, 0xf3);

    /*
     * `vzeroupper`.
     *
     * No operands, no result, and nothing the allocator has to be told: what it does is to the
     * upper halves of registers no value of this function is living in by the time it runs. It
     * is VEX-encoded - C5 F8 77 - which is what makes it the one form here that states a feature.
     */
    auto& zero = add(FormVZeroUpper, OpVZeroUpper, "vzeroupper"_v);
    zero.requiredFeatures = kFeatureAvx;
    zero.encoding = EncodingDescriptor {
        .family = EncodingFamily::Opcode,
        .opcode = 0x77,
        // No operand and no result, so the width is stated rather than derived - and it is what
        // the encoding is: `C5 F8 77` is VEX.128.0F.WIG, which is W0 as this writer emits it.
        .width = OperationWidth::Fixed32,
        .prefixEncoding = PrefixEncoding::Vex,
    };

}
