#pragma once

#include "machine.h"
#include "x64_util.h"

/*
 * The AMD64 form table and the selection over it, and what each file of it answers.
 *
 * Everything the backend used to know about an instruction in several places is stated once in the
 * table. This header is what the files that build it and the files that read it agree on: the form
 * ids that name its entries, the builder that fills them, and the handful of answers one of these
 * files needs from another. Read this list before hunting for a form or a rule.
 *
 *   machine.cpp                 the spine: the opcode names, and the constructor that calls the five
 *                               registrations below in the order the form ids declare their forms.
 *   machine_forms_scalar.cpp    everything a general register or a scalar float does - the moves and
 *                               casts, the conversions and bitcasts across the two banks, the ALU,
 *                               the shifts, the comparisons, and the scalar VEX/EVEX twins.
 *   machine_forms_packed.cpp    the packed set at 128 bits: one row per operation and lane width,
 *                               which is what every wider and every prefixed tier is derived from.
 *   machine_forms_tiers.cpp     those two derived tiers - the 256-bit forms, and the sweep that
 *                               gives every legacy vector form a VEX twin.
 *   machine_forms_memory.cpp    the rest of the machine: select, alloca, load and store, the
 *                               in-place memory updates, the block operations, calls, terminators.
 *   machine_validate.cpp        what a form is required to be, checked over the whole table once at
 *                               startup - including the temporary reserve each bank has to hold.
 *   machine_select.cpp          which opcode an instruction is, and which of that opcode's forms it
 *                               takes on this target.
 *   machine_vector.cpp          the same question for a packed instruction, and - the same set of
 *                               answers read the other way round - what this backend refuses to do
 *                               to a vector at all.
 *
 * Reading order within the table: the form ids below name its entries, the builder fills them, and
 * `selectForm` in machine_select.cpp says which one an instruction takes.
 *
 * Two conventions keep the table short. A form's `uses` array only has to cover the operands that
 * have something to say: an operand beyond its length is an unconstrained register of its own class,
 * which is what most operands of most instructions are. And a form whose operand count is not fixed
 * - a call has as many as it has arguments - sets `conventionOperands` and takes its constraints
 * from the calling convention instead.
 */

enum: MachineFormId {
    FormNop,
    FormArg,
    FormPhi,
    FormAddress,

    FormImmMov,
    FormImmZero,
    FormImmImplicit,
    FormImmFloat32,
    FormImmFloat64,
    FormGlobalAddress,
    FormGlobalImplicit,
    FormFunctionAddress,
    FormFunctionImplicit,

    FormMove,
    FormMoveF32,
    FormMoveF64,
    FormCastMov,
    FormCastCopy,
    FormCastSext,
    FormSext8, FormSext16, FormSext32,
    FormCastImm,
    FormCastZero,
    FormBitcast,
    FormBitcastImm,
    FormBitcastZero,

    FormCastIToF32, FormCastIToF64,
    FormCastF32ToI, FormCastF64ToI,
    FormCastF32ToF64, FormCastF64ToF32,

    FormBitcastF32ToI, FormBitcastF64ToI,
    FormBitcastIToF32, FormBitcastIToF64,
    FormBitcastF32,    FormBitcastF64,

    FormNeg,
    FormNot,
    FormBswap,

    FormAddReg, FormAddImm, FormAddMem,
    FormSubReg, FormSubImm, FormSubMem,
    FormAndReg, FormAndImm, FormAndMem,
    FormOrReg,  FormOrImm,  FormOrMem,
    FormXorReg, FormXorImm, FormXorMem,

    FormAddInc, FormAddDec,
    FormSubInc, FormSubDec,

    // The BMI1 pair-replacements. One form each, all four needing the feature - see their
    // registration in machine_forms_scalar.cpp.
    FormAndNot,
    FormLowBitClear, FormLowBitIsolate, FormLowBitMask,
    FormAndNotMem,
    FormLowBitClearMem, FormLowBitIsolateMem, FormLowBitMaskMem,

    FormMul,
    FormDiv,
    FormIDiv,
    FormRem,
    FormIRem,
    FormMulHi,
    FormIMulHi,

    // The BMI2 spelling of the unsigned high product - see the note beside its registration. An
    // alternative of FormMulHi rather than a row of its own, so nothing selects it by name.
    FormMulx,

    FormIMulReg,
    FormIMulMem,
    FormIMulImm,

    FormShlImm, FormShlOne, FormShlCl,
    FormShrImm, FormShrOne, FormShrCl,
    FormSarImm, FormSarOne, FormSarCl,
    FormRolImm, FormRolOne, FormRolCl,
    FormRorImm, FormRorOne, FormRorCl,

    // The BMI2 shifts, which are alternatives of the three `cl` forms above and of the immediate
    // rotate - the four of the fifteen that have a VEX spelling. See the note beside them.
    FormShlx, FormShrx, FormSarx, FormRorx,

    FormCmpReg,
    FormCmpRegSet,
    FormCmpMem,
    FormCmpMemSet,
    FormCmpImm,
    FormCmpImmSet,
    FormCmpNone,

    FormFAdd32, FormFAdd64,
    FormFSub32, FormFSub64,
    FormFMul32, FormFMul64,
    FormFDiv32, FormFDiv64,
    FormFNeg32, FormFNeg64,

    FormFAdd32Mem, FormFAdd64Mem,
    FormFSub32Mem, FormFSub64Mem,
    FormFMul32Mem, FormFMul64Mem,
    FormFDiv32Mem, FormFDiv64Mem,

    FormFCmp32, FormFCmp32Set,
    FormFCmp64, FormFCmp64Set,
    FormFCmp32Mem, FormFCmp32MemSet,
    FormFCmp64Mem, FormFCmp64MemSet,

    /*
     * The same operations written with a VEX prefix, selected in place of the ones above wherever
     * the target has AVX - see MachineForm::alternative.
     *
     * They are listed here rather than beside each original because they are derived from them:
     * every one is built by `vexTwin` out of the form above it, so the only thing stated twice is a
     * name. The arithmetic ones are three-operand and so drop the tie; the comparisons and the
     * transfers have nothing to drop and differ from their originals in the prefix alone.
     */
    FormFAdd32Vex, FormFAdd64Vex, FormFAdd32VexMem, FormFAdd64VexMem,
    FormFAdd32Evex, FormFAdd64Evex, FormFAdd32EvexMem, FormFAdd64EvexMem,
    FormFSub32Vex, FormFSub64Vex, FormFSub32VexMem, FormFSub64VexMem,
    FormFSub32Evex, FormFSub64Evex, FormFSub32EvexMem, FormFSub64EvexMem,
    FormFMul32Vex, FormFMul64Vex, FormFMul32VexMem, FormFMul64VexMem,
    FormFMul32Evex, FormFMul64Evex, FormFMul32EvexMem, FormFMul64EvexMem,
    FormFDiv32Vex, FormFDiv64Vex, FormFDiv32VexMem, FormFDiv64VexMem,
    FormFDiv32Evex, FormFDiv64Evex, FormFDiv32EvexMem, FormFDiv64EvexMem,

    FormFNeg32Vex, FormFNeg64Vex,

    FormFCmp32Vex, FormFCmp32VexSet,
    FormFCmp64Vex, FormFCmp64VexSet,
    FormFCmp32VexMem, FormFCmp32VexMemSet,
    FormFCmp64VexMem, FormFCmp64VexMemSet,

    /*
     * The packed set, at 128 bits.
     *
     * One form per (operation, lane type), which is the shape the machine has: `paddb`, `paddw`,
     * `paddd` and `paddq` differ in one opcode byte and in nothing else, and `addps` differs from
     * `addpd` in a mandatory prefix. So they are built by a table below rather than written out, and
     * the ids here exist only so that the id is the index.
     *
     * `V128` in the name is the register width rather than the lane count: these are the xmm forms,
     * and the ymm and zmm ones are the VEX and EVEX tiers this does not build yet - see the note on
     * `packedArith`.
     *
     * The gaps are the machine's own. There is no packed byte or quadword multiply, no arithmetic
     * shift of a quadword, and no packed shift of a byte at all; the lane types that have no
     * instruction have no form, and `selectFormForTarget` refuses one rather than reaching for a
     * neighbouring width.
     */
    FormVAdd8, FormVAdd16, FormVAdd32, FormVAdd64, FormVAddF32, FormVAddF64,
    FormVSub8, FormVSub16, FormVSub32, FormVSub64, FormVSubF32, FormVSubF64,
    // The 32-bit lane is two rows because the machine changed its mind about it: `pmulld` is one
    // instruction and is SSE4.1, and below that the same product is four multiplies' worth of
    // shuffling around `pmuludq`, which multiplies the even lanes only. See PseudoKind::VecMul32.
    FormVMul16, FormVMul32, FormVMul32Sse2, FormVMulF32, FormVMulF64,
    // The *high* half of a product, which the machine has at a 16-bit lane and at no other: `pmulhw`
    // and `pmulhuw` are one row each because the top half of a product is the one place a
    // multiplication's signedness is visible in its bits. There is no `pmulhd` or `pmulhb` at any
    // feature level, so this pair is the whole family.
    FormVMulHi16, FormVIMulHi16,

    // The widening multiply of every *other* 32-bit lane into a 64-bit one, which is x86's only
    // 32x32 -> 64 packed product and the thing every wider integer multiply here is built out of -
    // see LowerInst::X86MulWide. `pmuludq` is SSE2 and `pmuldq` is SSE4.1, so the floor claims both.
    FormVMulWide32, FormVIMulWide32,
    FormVDivF32, FormVDivF64,

    // The bitwise three, at one form each: the machine has one packed `and` and it does not care
    // what the lanes are. A mask uses the same forms, which is what a mask *is* without AVX-512.
    FormVAnd, FormVOr, FormVXor, FormVAndNot,

    // And the `and` in the *float* domain, which is the one bitwise row here that a lane type
    // selects: `andps`/`andpd` do to a float vector exactly what `pand` does, and differ in the
    // forwarding domain the result is read from. What reaches them is the absolute value - see
    // `expandVectorAbs` - and the masked vector, `X86MaskAnd`; the complemented pair beside them is
    // that second reader's other arm, and is why `andn` has a float row where `or` and `xor` do not.
    FormVAndF32, FormVAndF64,
    FormVAndNotF32, FormVAndNotF64,

    // Shifts by a constant count every lane shares. AVX2's one count *per lane* - `vpsllvd` and its
    // siblings - is still absent, and is the one shift shape `unsupportedVectorReason` refuses; the
    // register-count rows below are the other spelling of a shared count and not that.
    FormVShl16Imm, FormVShl32Imm, FormVShl64Imm,
    FormVShr16Imm, FormVShr32Imm, FormVShr64Imm,
    FormVSar16Imm, FormVSar32Imm,

    // And the same three by a count held in a register, which the machine *does* have: `psllw xmm,
    // xmm/m128` reads the low quadword of its second operand as one count every lane shares. One row
    // per direction and not per width - the three widths differ in the opcode byte alone, which the
    // expansion reads off the instruction's type, and what a form states is what the allocator
    // reads. The count arrives in a general register and the instruction wants it in a vector one,
    // which is the transfer that makes these pseudos.
    FormVShlReg, FormVShrReg, FormVSarReg,

    // And a count *per lane*, which is AVX2's own family and exists at no feature level below it:
    // `vpsllvd`/`vpsllvq`, `vpsrlvd`/`vpsrlvq` and `vpsravd`. There is no `vpsravq` before AVX-512,
    // which is why the arithmetic direction has one row where the other two have two -
    // `expandQuadwordSar` builds the missing one out of `vpsrlvq` and a bias.
    FormVShlVar32, FormVShlVar64,
    FormVShrVar32, FormVShrVar64,
    FormVSarVar32,

    // Comparison into a mask. The integer ones test one relation each and the rest are reached by
    // swapping the operands; the float ones carry the relation as an immediate predicate, which is
    // why there are two of them and eight of the others.
    //
    // The quadword pair is the one that used to be missing and is not the machine's gap: `pcmpeqq`
    // is SSE4.1 and `pcmpgtq` is SSE4.2, both of which the v2 floor claims. They live in the
    // three-byte 0F38 map where the three narrower widths are two-byte opcodes, which is the only
    // thing about them the table has to say.
    FormVCmpEq8, FormVCmpEq16, FormVCmpEq32, FormVCmpEq64,
    FormVCmpGt8, FormVCmpGt16, FormVCmpGt32, FormVCmpGt64,
    FormVCmpF32, FormVCmpF64,
    /*
     * The lane-wise minimum and maximum, which is the one packed family with a column per
     * *signedness* as well as per lane width: `pminsb` and `pminub` are two instructions where
     * `paddb` is one for both readings.
     *
     * The quadword is missing at both signednesses and is the machine's gap rather than this
     * table's - there is no `pminsq` before AVX-512 - so a 64-bit minimum keeps the comparison and
     * the select it was written as. The float pair carries no signedness: `minps` is the only
     * ordering floats have.
     */
    FormVMinI8, FormVMinU8, FormVMinI16, FormVMinU16, FormVMinI32, FormVMinU32,
    FormVMinF32, FormVMinF64,
    FormVMaxI8, FormVMaxU8, FormVMaxI16, FormVMaxU16, FormVMaxI32, FormVMaxU32,
    FormVMaxF32, FormVMaxF64,


    // The three signed relations the machine has only the complement of - see
    // packedCompareIsInverted. One form for every lane width, because what differs between them is
    // the opcode byte the pseudo picks and not anything the allocator reads.
    FormVCmpInverted,

    // A mask or a vector complemented, which is an exclusive-or against an all-ones vector - and all
    // ones is a register compared with itself. Two instructions and a scratch, so a pseudo.
    FormVNot,

    // Lanes rearranged within one vector. `pshufd` is the only one of the machine's shuffles that is
    // non-destructive - it reads its source through r/m and writes a destination that need not be
    // either operand - which is why it is the one this tier reaches for first: every other spelling
    // needs the tie the three-operand form does not have. The `Second` twin is the same instruction
    // reading the shuffle's *other* operand, for a pattern that names only the second source.
    FormVShuffle32, FormVShuffle32Second,

    /*
     * Lanes taken from two vectors, which is the machine's other shuffle family and is two shapes.
     *
     * `shufps`/`shufpd` take two lanes (or one) from each side by a control byte, so they cover
     * every pattern whose *low* half comes from the first source and whose high half comes from the
     * second. `punpck`/`unpck` interleave - a lane of each, alternating - which is the pattern that
     * shape cannot state and the one a lane-count conversion is built out of.
     *
     * Both families are two-address, unlike `pshufd`, so a source still live afterwards gets the
     * copy the tie asks for. The machine has no three-operand spelling of either before VEX.
     */
    FormVShuffle2F32, FormVShuffle2F64,
    FormVUnpackLow8, FormVUnpackLow16, FormVUnpackLow32, FormVUnpackLow64,
    FormVUnpackLowF32, FormVUnpackLowF64,
    FormVUnpackHigh8, FormVUnpackHigh16, FormVUnpackHigh32, FormVUnpackHigh64,
    FormVUnpackHighF32, FormVUnpackHighF64,

    /*
     * Every lane the same scalar, at every lane width - the two narrow ones included.
     *
     * ~~An 8- or 16-bit lane arrives as an Int32 and would need the byte and word shuffles this tier
     * does not have.~~ It needs one of two sequences, and which one is a *feature* question rather
     * than a row: with AVX2 a narrow broadcast is `vpbroadcastb`/`vpbroadcastw`, one instruction
     * after the bank crossing; without it a byte is `pshufb` against a register of zeros and a word
     * is `pshuflw` and then `pshufd`. So the narrow pair is two rows each, exactly as `pmulld` and
     * its SSE2 stand-in are - and the byte's baseline row is the one that declares the scratch the
     * zeros live in.
     */
    FormVBroadcast8, FormVBroadcast8Sse, FormVBroadcast16, FormVBroadcast16Sse,
    FormVBroadcast32, FormVBroadcast64, FormVBroadcastF32, FormVBroadcastF64,

    // The two constants made out of nothing rather than loaded - see PseudoKind::VecZero. One form
    // each and no lane column: all-zeros is all-zeros at every lane width and so is all-ones, which
    // is the same fact `emitAllOnes` already states about `pcmpeqd` serving a byte lane and a mask.
    FormVZero, FormVOnes,
    FormVWideZero, FormVWideOnes,

    // One lane out of a vector. The two integer forms reach any index; the two below them are lane
    // zero, which `movd`/`movq` reach in two bytes fewer and no index byte. The float pair needs no
    // feature at all, the value staying in the bank it is already in - and its own lane zero is the
    // register itself, so the `Low` pair is the bank's copy with `omitWhenSame`.
    // The two narrow ones, which are not a convenience: `movd` and `pextrd` read *four bytes*, so a
    // byte or word lane read through them answers its 32-bit neighbourhood. Both zero-extend into
    // the destination, which is the whole answer for an unsigned lane and half of it for a signed
    // one - see `Value::VecLane` in resolve/lower_calc.cpp for the other half.
    FormVExtract8, FormVExtract16,
    FormVExtract32, FormVExtract64,
    FormVExtract32Zero, FormVExtract64Zero,
    // The movemask, at one row per number of bits a lane contributes. `pmovmskb` answers a bit per
    // *byte*, which is what a narrow lane wants and what a wide one then has to divide back out;
    // `movmskps` and `movmskpd` answer a bit per lane outright. See `maskBitsPerLane` in
    // transform_reduce.cpp, which is the other half of this choice and has to agree with it.
    FormVMaskBits, FormVMaskBitsF32, FormVMaskBitsF64,
    FormVExtractF32, FormVExtractF64,
    FormVExtractF32Low, FormVExtractF64Low,

    // One lane into a vector, which is a longer list than the extract's because the machine gives a
    // word its own instruction and gives nothing else one below SSE4.1. `pinsrw` is the baseline's
    // whole integer half, and a 32- or 64-bit lane is spent as the words it is made of - see
    // `lowerLaneInserts`. The float half is `insertps` at SSE4.1 and, at the baseline, the two
    // instructions that write exactly one half of a two-lane double vector.
    FormVInsert8, FormVInsert16, FormVInsert32, FormVInsert64,
    FormVInsertF32, FormVInsertF32Low,
    FormVInsertF64Low, FormVInsertF64High,

    // A lane-wise select, which the baseline has no single instruction for: three bitwise operations
    // through a scratch the form declares as a clobber. One form for every lane type and for a mask,
    // because none of the three cares what a lane is.
    //
    // `Blend` is `pblendvb`, the same operation in one instruction and a mask move - two rows rather
    // than an `alternative` chain, because they do not tie the same operand and the allocator has to
    // be told which before it runs. See selectSelectForm.
    FormVSelect, FormVSelectBlend,

    // A vector reinterpreted as another vector of the same width, which is the register itself and
    // therefore no instruction at all where the allocator has already put the two in one place.
    FormVBitcast,

    // A vector copied, and a vector negated. The copy is the same `movaps` the bitcast is and exists
    // for the same reason - two values the allocator may have put in two places - and differs from
    // it only in the opcode it is a form of. The negation is a pseudo at both lane kinds.
    FormVMove,
    FormVNeg8, FormVNeg16, FormVNeg32, FormVNeg64, FormVNegF32, FormVNegF64,

    // The magnitude of an integer lane, which SSSE3 gives at three widths and no feature level gives
    // at the fourth: there is no `pabsq` before AVX-512. A float lane has no row here at all - it is
    // an `and` against a pooled sign mask, which `expandVectorAbs` builds out of instructions this
    // table already has.
    FormVAbs8, FormVAbs16, FormVAbs32,

    // A packed conversion between the two lane kinds, which at this width is the 32-bit lane alone:
    // any other pair changes the register width as well as the lane's.
    FormVCastIToF32, FormVCastFToI32,

    /*
     * The square root and the fused multiply-add, at the scalar and the packed width of each lane
     * kind - four forms each, and one opcode each, because that is how the machine spells them.
     *
     * The FMA forms are VEX-only and so are the only forms in this table with no legacy tier under
     * them at all. What stands in for one is `expandFusedMultiplyAdd`, which is a *transform*: where
     * the target has no FMA3 the operation becomes the multiply and the add it always meant, at two
     * roundings rather than one, which Design-Vector §3.3 permits outright.
     */
    FormSqrt32, FormSqrt64, FormVSqrtF32, FormVSqrtF64,
    FormRound32, FormRound64, FormVRoundF32, FormVRoundF64,
    FormFma32, FormFma64, FormVFmaF32, FormVFmaF64,

    // A vector moved between a register and memory, at the two spellings the machine has for it.
    // Neither is aligned: an address the program computed is not one this backend has said anything
    // about, and the aligned forms fault rather than being slow.
    FormVLoad, FormVLoadInt,
    FormVStore, FormVStoreInt,

    FormVZeroUpper,

    /*
     * The 256-bit tier - see `MachineForm::wide`.
     *
     * One id per form above that a whole-register vector operation can reach, in the same order they
     * are built, because `add` requires the two to agree. They are derived rather than written:
     * every one of them is its narrow source with a VEX prefix, `L` set, its whole-register operands
     * moved up a class and - where the machine had no three-operand spelling before VEX - its tie
     * removed. So this block is a list of *which* operations widen and nothing about how.
     *
     * What is absent from it is as much of the answer as what is in it. There is no wide twin of
     * `FormVMul32Sse2` (AVX2 implies SSE4.1, so the `pmulld` route is always the one selected), none
     * of the four broadcasts or of the lane accesses (those are `vpbroadcast` and the 128-bit lane
     * halves, which are different instructions rather than the same one twice), and none of the
     * two-source shuffle families beyond the in-lane ones - which is the machine's own shape and is
     * where the wide tier is least like the narrow one. See `packedShuffleChoice`.
     */
    FormVWideAdd8, FormVWideAdd16, FormVWideAdd32, FormVWideAdd64, FormVWideAddF32, FormVWideAddF64,
    FormVWideSub8, FormVWideSub16, FormVWideSub32, FormVWideSub64, FormVWideSubF32, FormVWideSubF64,
    FormVWideMul16, FormVWideMul32, FormVWideMulF32, FormVWideMulF64,
    FormVWideMulHi16, FormVWideIMulHi16,
    FormVWideMulWide32, FormVWideIMulWide32,
    FormVWideDivF32, FormVWideDivF64,

    FormVWideAnd, FormVWideOr, FormVWideXor, FormVWideAndNot,
    FormVWideAndF32, FormVWideAndF64,
    FormVWideAndNotF32, FormVWideAndNotF64,

    FormVWideShlReg, FormVWideShrReg, FormVWideSarReg,
    FormVWideShlVar32, FormVWideShlVar64,
    FormVWideShrVar32, FormVWideShrVar64,
    FormVWideSarVar32,
    FormVWideShl16Imm, FormVWideShl32Imm, FormVWideShl64Imm,
    FormVWideShr16Imm, FormVWideShr32Imm, FormVWideShr64Imm,
    FormVWideSar16Imm, FormVWideSar32Imm,

    FormVWideCmpEq8, FormVWideCmpEq16, FormVWideCmpEq32, FormVWideCmpEq64,
    FormVWideCmpGt8, FormVWideCmpGt16, FormVWideCmpGt32, FormVWideCmpGt64,
    FormVWideCmpF32, FormVWideCmpF64,
    FormVWideMinI8, FormVWideMinU8, FormVWideMinI16, FormVWideMinU16,
    FormVWideMinI32, FormVWideMinU32, FormVWideMinF32, FormVWideMinF64,
    FormVWideMaxI8, FormVWideMaxU8, FormVWideMaxI16, FormVWideMaxU16,
    FormVWideMaxI32, FormVWideMaxU32, FormVWideMaxF32, FormVWideMaxF64,

    FormVWideCmpInverted,

    FormVWideNot, FormVWideSelect,
    FormVWideNeg8, FormVWideNeg16, FormVWideNeg32, FormVWideNeg64,
    FormVWideNegF32, FormVWideNegF64,
    FormVWideAbs8, FormVWideAbs16, FormVWideAbs32,

    FormVWideShuffle32, FormVWideShuffle32Second,
    FormVWideShuffle2F32, FormVWideShuffle2F64,
    FormVWideUnpackLow8, FormVWideUnpackLow16, FormVWideUnpackLow32, FormVWideUnpackLow64,
    FormVWideUnpackLowF32, FormVWideUnpackLowF64,
    FormVWideUnpackHigh8, FormVWideUnpackHigh16, FormVWideUnpackHigh32, FormVWideUnpackHigh64,
    FormVWideUnpackHighF32, FormVWideUnpackHighF64,

    FormVWideBitcast, FormVWideMove,
    FormVWideCastIToF32, FormVWideCastFToI32,
    FormVWideSqrtF32, FormVWideSqrtF64,
    FormVWideRoundF32, FormVWideRoundF64,
    FormVWideFmaF32, FormVWideFmaF64,
    FormVWideLoad, FormVWideLoadInt,
    FormVWideStore, FormVWideStoreInt,

    /*
     * And the four forms the wide tier has that the narrow one has no counterpart for.
     *
     * `vperm2f128` moves whole 128-bit halves between two sources, which is the *only* cross-half
     * rearrangement in this tier - every other shuffle AVX2 has works inside each half - and it is
     * what the top level of a reduction butterfly over eight lanes needs. `vextracti128` and
     * `vinserti128` are the same crossing read as a narrowing and a widening, which is how a lane
     * above the low half is reached and how a splat is built.
     *
     * `vpbroadcastd`/`vpbroadcastq` replace the shuffle a 128-bit splat takes: there is no in-lane
     * shuffle that reaches the upper half, so the broadcast has to be an instruction that crosses
     * and AVX2 is where one arrives.
     *
     * And `vpermd`/`vpermps`, which are the *general* crossing at a 32-bit lane and the reason those
     * two rows are not the whole story: `vperm2f128` moves halves entire, and any eight-lane pattern
     * that is neither in-lane nor a half exchange needs an instruction that reads one lane index per
     * result lane. Both take those indices out of a **vector register**, which is why these are the
     * one shuffle family here whose pattern is an operand rather than an immediate - see
     * LowerInst::X86Permute and `lowerWideLanePermutes`.
     */
    FormVPerm2, FormVPermute32, FormVPermuteF32, FormVExtract128, FormVInsert128,
    FormVWideBroadcast8, FormVWideBroadcast16,
    FormVWideBroadcast32, FormVWideBroadcast64,
    FormVWideBroadcastF32, FormVWideBroadcastF64,

    // A lane read out of or written into a 256-bit vector, which is the 128-bit access with the
    // wanted half brought down in front of it - see PseudoKind::VecWideLane. Four of each, by the
    // bank the scalar lives in and by its width, which is the same split the narrow forms have.
    FormVWideExtract32, FormVWideExtract64, FormVWideExtractF32, FormVWideExtractF64,
    FormVWideMaskBits, FormVWideMaskBitsF32, FormVWideMaskBitsF64,
    FormVWideInsert32, FormVWideInsert64, FormVWideInsertF32, FormVWideInsertF64,

    FormSelectFlags,
    FormSelectReg,
    FormSelectFloat32Flags, FormSelectFloat64Flags,
    FormSelectFloat32Reg,   FormSelectFloat64Reg,

    FormAllocaFixed,
    FormAllocaDynamic,

    FormLoad8, FormLoad8S,
    FormLoad16, FormLoad16S,
    FormLoad32, FormLoad32S,
    FormLoad64,
    FormLoadF32, FormLoadF64,

    FormStore8, FormStore16, FormStore32, FormStore64,
    FormStore8Imm, FormStore16Imm, FormStore32Imm, FormStore64Imm,
    FormStoreF32, FormStoreF64,

    // The VEX transfers, beside the forms they are derived from rather than with the arithmetic ones
    // above, because `add` requires the construction order to be the id order and these are built
    // where their originals are.
    FormLoadF32Vex, FormLoadF64Vex,
    FormStoreF32Vex, FormStoreF64Vex,

    // The byte-reversing accesses, at the two widths a reversal can reach them from - see the block
    // that builds them for why the machine's third width is not here.
    FormMovbeLoad32, FormMovbeLoad64,
    FormMovbeStore32, FormMovbeStore64,

    /*
     * The in-place memory updates, six per operation: one per access width, and the two immediate
     * shapes at the widths a group-1 immediate has - see `storeUpdate` below.
     *
     * Written out rather than generated because the ids are the construction order, and the block
     * that builds them reads this list back one operation at a time.
     */
    FormStoreAdd8, FormStoreAdd16, FormStoreAdd32, FormStoreAdd64,
    FormStoreAdd32Imm, FormStoreAdd64Imm,
    FormStoreSub8, FormStoreSub16, FormStoreSub32, FormStoreSub64,
    FormStoreSub32Imm, FormStoreSub64Imm,
    FormStoreAnd8, FormStoreAnd16, FormStoreAnd32, FormStoreAnd64,
    FormStoreAnd32Imm, FormStoreAnd64Imm,
    FormStoreOr8, FormStoreOr16, FormStoreOr32, FormStoreOr64,
    FormStoreOr32Imm, FormStoreOr64Imm,
    FormStoreXor8, FormStoreXor16, FormStoreXor32, FormStoreXor64,
    FormStoreXor32Imm, FormStoreXor64Imm,

    FormBlockCopyRep,
    FormBlockSetRep,

    FormCallDirect,
    FormCallIndirect,
    FormSyscall,
    FormPushArgReg,
    FormPushArgF32,
    FormPushArgF64,
    FormPushArgImm,

    FormLea,

    FormJmp,
    FormJccFlags,
    FormJccLive,
    FormJccReg,
    FormRet,
    FormNoReturn,

    kMachineFormCount,
};

/*
 * Encoding shorthands.
 *
 * Each one names the bytes and the operand-to-field mapping of one encoding shape. Emission walks
 * these; nothing below is a function the encoder has to know the name of.
 */

// `op reg, r/m`, with the two ModRM fields taken from the named operands. `alt` is the same
// operation with those fields swapped, which is what an operand left in a frame slot takes: a memory
// operand has to occupy the r/m field, so whichever operand needs a register moves into the reg one.
// Zero for an operation encoded in only one direction.
static EncodingDescriptor regRm(U8 opcode, OperandRef reg, OperandRef rm, U8 alt = 0) {
    return EncodingDescriptor {
        .family = EncodingFamily::RegRm,
        .opcode = opcode, .opcodeAlt = alt,
        .regField = reg, .rmField = rm,
    };
}

// `op r/m` with an opcode extension in the ModRM.reg field, for the one-operand shapes that have no
// second register: neg, not, mul, div, inc, and the shifts by one or by cl.
static EncodingDescriptor rmExt(U8 opcode, U8 extension, OperandRef rm) {
    return EncodingDescriptor {
        .family = EncodingFamily::RmExt,
        .opcode = opcode, .extension = extension,
        .rmField = rm,
    };
}

// `op r/m, imm`. `imm32` is the wider encoding of the same operation, or zero for one that carries
// an 8-bit immediate only.
static EncodingDescriptor rmExtImm(U8 imm8, U8 imm32, U8 extension, OperandRef rm, OperandRef imm) {
    return EncodingDescriptor {
        .family = EncodingFamily::RmExtImm,
        .opcode = imm8, .opcodeAlt = imm32, .extension = extension,
        .rmField = rm, .immField = imm,
    };
}

// `op xmm, xmm/m` - the SSE two-byte shape, where the mandatory prefix is the width and there is no
// direction bit: the destination is always the ModRM.reg field, so an operand left in the frame can
// only ever be the one this puts in r/m. `prefix` is 0xf3 for the single-precision form, 0xf2 for
// the double-precision one, 0x66 for a packed-double or integer one, and zero for packed single.
static EncodingDescriptor sseRegRm(U8 prefix, U8 opcode, OperandRef reg, OperandRef rm, OperationWidth width) {
    return EncodingDescriptor {
        .family = EncodingFamily::RegRm,
        .opcode = opcode,
        .escape = 0x0f, .prefix = prefix,
        .regField = reg, .rmField = rm,
        .width = width,
        .widthInPrefix = true,
    };
}

/*
 * The table under construction.
 *
 * One object with the registrations as members, rather than five free functions each handed a
 * `MachineTarget&`, because a row is almost never written on its own: most of this table is
 * *derived* - a memory-source twin of a register form, a VEX twin of a legacy one, a 256-bit twin of
 * a 128-bit one - and each derivation reaches the form it is derived from as well as the one it is
 * building. The five methods below are the whole of how a row enters the table, and a registration
 * may do nothing else to it.
 */
struct MachineFormBuilder {
    MachineTarget& target;
    Array<MachineForm>& forms;
    MachineOpcodeDesc* opcodes;
    Array<char>& derivedNames;

    explicit MachineFormBuilder(MachineTarget& t)
        : target(t), forms(t.forms), opcodes(t.opcodes), derivedNames(t.derivedNames) {}
    void name(MachineOpcodeId id, StringView text, bool flagsSelective = false) {
        opcodes[id].name = text;
        opcodes[id].flagsSelective = flagsSelective;
    }

    // Each form is pushed in the order the ids above declare it, so that the id is its index.
    MachineForm& add(MachineFormId id, MachineOpcodeId opcode, StringView formName) {
        assertTrue(forms.size() == id); // the form ids and the construction order have drifted apart

        forms.push(MachineForm {});

        auto& form = forms[forms.size() - 1];
        form.id = id;
        form.opcode = opcode;
        form.name = formName;
        return form;
    }

    /*
     * The memory-source twin of a form.
     *
     * Most of the AMD64 ALU can read one operand straight out of memory, and §5.5 already takes that
     * for a *frame slot*: the operand keeps its location and the encoder writes the slot into the
     * ModRM byte. What it does not take is a load the program actually wrote - `mov rax, [rdi]`
     * followed by `add rcx, rax` is two instructions where `add rcx, [rdi]` is one - because the
     * address is not a location, it is an addressing mode, and only an `address()` operand carries
     * one. So the memory source is a form of its own, and it is *derived* from the register form
     * rather than written beside it: the two are one operation, and stating the opcode, the flags
     * effect, the clobbers and the width twice is how they come to disagree.
     *
     * Exactly three things differ, and the twin is nothing but those three:
     *
     *  - the memory-capable operand becomes an `address()` - the same operand kind a load already
     *    has, which placement leaves alone and legalization already resolves into a MachineAddress;
     *  - every *other* operand that could have stayed in a frame slot becomes an ordinary register,
     *    since the single r/m field is now the address's;
     *  - the encoding becomes the LoadStore family - which is the one that writes a ModRM byte
     *    around `regs.address` - in whichever direction reaches the memory operand. The group-1
     *    shapes encode theirs in the ModRM.reg field of their register form, so those take the other
     *    direction, which is what `opcodeAlt` already is.
     *
     * foldLoads in transform_address.cpp is what moves an instruction onto one, and §5 of
     * test/bench/findings.md is the measurement.
     */
    MachineForm& memoryTwin(MachineFormId id, MachineFormId sourceId, StringView formName) {
        // A copy, taken before `add` below can move the array out from under a reference.
        auto twin = forms[sourceId];
        auto memory = twin.memoryUse();
        assertTrue(memory >= 0); // a memory-source twin of a form with no memory operand

        auto& e = twin.encoding;
        auto direct = !e.rmField.isNone() && !e.rmField.result && I32(e.rmField.index) == memory;

        // The other direction of the same operation, for the shapes whose register form puts the
        // memory operand in ModRM.reg: `add r/m, r` becomes `add r, r/m` and the two operands stay
        // exactly where they were.
        if(!direct) {
            assertTrue(e.opcodeAlt != 0 && !e.regField.isNone() && !e.regField.result
                && I32(e.regField.index) == memory); // a memory operand this encoding cannot address

            e.opcode = e.opcodeAlt;
            e.regField = e.rmField;
        }

        e.family = EncodingFamily::LoadStore;
        e.rmField = useRef(U8(memory));
        e.opcodeAlt = 0;

        for(Size i = 0; i < twin.uses.size(); i++) {
            auto& constraint = twin.uses[i];

            if(I32(i) == memory) constraint = address();
            else if(constraint.kind == OperandConstraintKind::RegisterOrMemory) constraint = anyReg(constraint.regClass);
        }

        twin.id = id;
        twin.name = formName;
        twin.memorySource = 0;
        twin.memorySourceOf = sourceId;

        auto& form = add(id, twin.opcode, formName);
        form = twin;

        forms[sourceId].memorySource = id;
        return form;
    }

    /*
     * The tie removed, and the destination put in the field the prefix that removes it names one in.
     *
     * Shared by both derivations below rather than written twice, because the two got it different:
     * where the destination ends up depends on which field the *legacy* encoding had it in, and a
     * copy of this that only ever saw one shape would be right about that shape and silent about
     * the other.
     *
     *  - `op reg, r/m` names the destination in ModRM.reg and doubles it as the first source. VEX
     *    keeps it there and gives `vvvv` the source it used to double as: `vaddps xmm1, xmm2, xmm3`.
     *  - `op r/m, imm8` has an *extension* in ModRM.reg and the destination in r/m, which is the
     *    shape of every packed shift. VEX cannot move the extension, so r/m keeps the operand and
     *    `vvvv` is the only field left to name the destination: `vpslld xmm1, xmm2, imm8` reads
     *    xmm2 and writes xmm1 where `pslld xmm1, imm8` did both to xmm1.
     *
     * Reading the first rule onto the second is what the wide shift forms were doing: `vvvv` named
     * the tied *source*, so the destination the allocator had been told was free was encoded
     * nowhere, and the shift wrote its answer into the register it read.
     *
     * Answers whether there was a tie to remove, so that a caller which requires one can say so.
     */
    static bool dropTie(MachineForm& twin) {
        auto& e = twin.encoding;

        // Idempotent, because the tiers are derived from each other: the EVEX form of an operation
        // is built out of its VEX form, which has already been through this.
        if(!e.vvvvField.isNone()) return true;

        auto tied = twin.tiedResult();
        if(tied < 0) return false;

        if(!e.regField.isNone()) {
            e.vvvvField = useRef(U8(tied));
            e.regField = defRef(0);
        } else {
            e.vvvvField = defRef(0);
        }

        twin.defs[0] = def(twin.defs[0].regClass);
        return true;
    }

    /*
     * The VEX form of a form.
     *
     * Derived from the legacy one for the reason `memoryTwin` above is derived from the register
     * one: they are one operation, and stating its opcode, its operands, its flags effect and its
     * width twice is how the two come to disagree. What the derivation changes is the prefix - the
     * mandatory byte and the escape become `pp` and a map, which the encoder reads out of the same
     * two descriptor fields either way - and, for a two-address operation, the tie.
     *
     * `threeOperand` is that second half, and it is the reason to prefer these forms at all rather
     * than merely a longer way to write the same bytes. `addsd xmm1, xmm2` puts its result in one of
     * its sources, so the allocator inserts a copy wherever the result has to go anywhere else;
     * `vaddsd xmm1, xmm2, xmm3` names the destination separately, so the tie goes and the copy with
     * it. The first source moves into VEX.vvvv, which is the field that exists to hold it.
     *
     * A form whose destination is not a register in the first place - a comparison, a store - passes
     * false and differs from its original in the prefix alone.
     */
    MachineForm& prefixedTwin(MachineFormId id, MachineFormId sourceId, StringView formName, bool threeOperand,
                              PrefixEncoding encoding, FeatureSet feature) {
        // A copy, taken before `add` below can move the array out from under a reference.
        auto twin = forms[sourceId];
        auto& e = twin.encoding;

        // Every scalar form this backend writes is in the two-byte opcode map, which is the map a
        // VEX prefix names as 1. A three-byte opcode would state its own map instead. A pseudo's
        // bytes are its own encoder's rather than the descriptor's, so it states neither and the
        // encoder is the one that has to agree - see emitFloatNeg.
        assertTrue(e.escape == 0x0f || e.family == EncodingFamily::Pseudo); // a VEX twin outside the 0F map
        e.prefixEncoding = encoding;

        // The map the source states, and not `kOpcodeMap0F` outright: the escape byte is 0x0f for
        // every form here, but a three-byte opcode says which of the two extended maps it is in with
        // this field alone (`pmulld` is 0F38), and rewriting it to 1 would encode a different
        // instruction that happens to exist. Every form written for the two-byte map already holds
        // the default, so this changes nothing for them.
        assertTrue(e.opcodeMap >= kOpcodeMap0F && e.opcodeMap <= kOpcodeMap0F3A); // a VEX twin of a form in no map

        /*
         * The tie is what is being removed, and where the destination lands when it goes is
         * `dropTie`'s question rather than this one's.
         */
        // The call stands outside the assertion rather than inside it: `assertTrue` does not evaluate
        // its argument in a build without asserts, so a derivation written as one would leave every
        // twin two-address in the build anybody ships and three-operand in the one the tests run.
        if(threeOperand) {
            auto wasTwoAddress = dropTie(twin);
            assertTrue(wasTwoAddress); // a three-operand twin of an operation that is not two-address
        }

        // And the operation that has no tie to remove but still names a third operand: the bits it
        // does not write come from the destination, which is where the legacy encoding left them.
        if(e.vvvvField.isNone() && e.mergesIntoDestination) e.vvvvField = defRef(0);

        twin.id = id;
        twin.name = formName;
        twin.requiredFeatures |= feature;

        // The links are rebuilt rather than inherited: this form's own memory twin is derived from
        // *it* and registered below, and its own alternative - the next tier up - is set when that
        // tier is built out of it.
        twin.memorySource = 0;
        twin.memorySourceOf = 0;
        twin.alternative = 0;
        twin.alternativeOf = sourceId;

        auto& form = add(id, twin.opcode, formName);
        form = twin;

        forms[sourceId].alternative = id;
        return form;
    }

    MachineForm& vexTwin(MachineFormId id, MachineFormId sourceId, StringView formName, bool threeOperand) {
        return prefixedTwin(id, sourceId, formName, threeOperand, PrefixEncoding::Vex, kFeatureAvx);
    }

    /*
     * And the EVEX tier, built out of the VEX one rather than out of the legacy form.
     *
     * Not because EVEX is better for a scalar operation - it is a byte longer than VEX and does
     * nothing extra here - but because it is what a target with thirty-two vector registers needs.
     * A value the allocator puts in xmm16 can only be named by an EVEX-encoded instruction, so every
     * form a scalar can reach has to have one before `vectorRegisterCountFor` can return more than
     * sixteen. Building them now, behind a feature nothing selects by default, is what makes the
     * EVEX writer something the test suite covers rather than something stage 5 discovers.
     */
    MachineForm& evexTwin(MachineFormId id, MachineFormId sourceId, StringView formName, bool threeOperand) {
        return prefixedTwin(id, sourceId, formName, threeOperand, PrefixEncoding::Evex, kFeatureAvx512f);
    }

    /*
     * The registrations, in the order the constructor runs them - which is the order the form ids
     * above declare their forms, since `add` asserts that a form's id is its index. A registration
     * moved is a build that stops at the first row of it.
     */
    void registerScalarForms();
    void registerPackedForms();
    void registerWideForms();
    void registerMemoryAndControlForms();
    void registerVexTier();
};

/*
 * machine_vector.cpp, read from machine_select.cpp.
 *
 * The packed selection is asked first and answers zero where the instruction is not a vector one, so
 * that the scalar switch below it never has to know that a vector exists.
 */
MachineFormId selectPackedForm(LowerBase base, LowerInst* inst);

// The form of a packed operation at the register width its type occupies - the one place the wide
// tier is reached from, and read from the scalar selection for the three operations that name a
// 128-bit form directly. There is no fallback in it: see the comment on the definition.
MachineFormId widthForm(MachineFormId narrow, LowerType type);
