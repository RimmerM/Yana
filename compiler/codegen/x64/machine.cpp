#include "machine.h"
#include "x64_util.h"

// For `nameForInst`, which is what a refusal names the instruction by - see checkVectorSupported.
#include "../../lower/lower_print.h"

/*
 * The AMD64 form table, and the selection that chooses between its entries.
 *
 * Everything the backend used to know about an instruction in several places is stated here once.
 * Reading order: the form ids below name the table's entries, the builder fills them, and selectForm
 * at the bottom says which one an instruction takes.
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

    FormAddReg, FormAddImm, FormAddMem,
    FormSubReg, FormSubImm, FormSubMem,
    FormAndReg, FormAndImm, FormAndMem,
    FormOrReg,  FormOrImm,  FormOrMem,
    FormXorReg, FormXorImm, FormXorMem,

    FormAddInc, FormAddDec,
    FormSubInc, FormSubDec,

    FormMul,
    FormDiv,
    FormIDiv,
    FormRem,
    FormIRem,
    FormMulHi,
    FormIMulHi,
    FormIMulReg,
    FormIMulMem,
    FormIMulImm,

    FormShlImm, FormShlOne, FormShlCl,
    FormShrImm, FormShrOne, FormShrCl,
    FormSarImm, FormSarOne, FormSarCl,

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
    FormVDivF32, FormVDivF64,

    // The bitwise three, at one form each: the machine has one packed `and` and it does not care
    // what the lanes are. A mask uses the same forms, which is what a mask *is* without AVX-512.
    FormVAnd, FormVOr, FormVXor, FormVAndNot,

    // And the `and` in the *float* domain, which is the one bitwise row here that a lane type
    // selects: `andps`/`andpd` do to a float vector exactly what `pand` does, and differ in the
    // forwarding domain the result is read from. What reaches them is the absolute value - see
    // `expandVectorAbs` - and nothing else does, which is why the other two operations have no
    // float row beside them.
    FormVAndF32, FormVAndF64,

    // Shifts by a constant count every lane shares. The machine also shifts by a count held in the
    // low quadword of a vector register, and AVX2 by one count per lane - neither is here, because
    // neither is what the IR's other spelling means: `shl %v, %count` with a register count is a
    // scalar in a general register, and getting it into a vector one is an expansion this backend
    // does not have yet. See selectPackedForm, which refuses it rather than choosing between them.
    FormVShl16Imm, FormVShl32Imm, FormVShl64Imm,
    FormVShr16Imm, FormVShr32Imm, FormVShr64Imm,
    FormVSar16Imm, FormVSar32Imm,

    // Comparison into a mask. The integer ones test one relation each and the rest are reached by
    // swapping the operands; the float ones carry the relation as an immediate predicate, which is
    // why there are two of them and eight of the others.
    FormVCmpEq8, FormVCmpEq16, FormVCmpEq32,
    FormVCmpGt8, FormVCmpGt16, FormVCmpGt32,
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

    // One lane out of a vector. The two integer forms are SSE4.1 and reach any index; the two below
    // them are the baseline's lane zero and nothing else. The float pair needs no feature at all,
    // the value staying in the bank it is already in.
    FormVExtract32, FormVExtract64,
    FormVExtract32Zero, FormVExtract64Zero,
    FormVMaskBits,
    FormVExtractF32, FormVExtractF64,

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
    FormVSelect,

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
    FormVWideDivF32, FormVWideDivF64,

    FormVWideAnd, FormVWideOr, FormVWideXor, FormVWideAndNot,
    FormVWideAndF32, FormVWideAndF64,

    FormVWideShl16Imm, FormVWideShl32Imm, FormVWideShl64Imm,
    FormVWideShr16Imm, FormVWideShr32Imm, FormVWideShr64Imm,
    FormVWideSar16Imm, FormVWideSar32Imm,

    FormVWideCmpEq8, FormVWideCmpEq16, FormVWideCmpEq32,
    FormVWideCmpGt8, FormVWideCmpGt16, FormVWideCmpGt32,
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
     */
    FormVPerm2, FormVExtract128, FormVInsert128,
    FormVWideBroadcast8, FormVWideBroadcast16,
    FormVWideBroadcast32, FormVWideBroadcast64,
    FormVWideBroadcastF32, FormVWideBroadcastF64,

    // A lane read out of or written into a 256-bit vector, which is the 128-bit access with the
    // wanted half brought down in front of it - see PseudoKind::VecWideLane. Four of each, by the
    // bank the scalar lives in and by its width, which is the same split the narrow forms have.
    FormVWideExtract32, FormVWideExtract64, FormVWideExtractF32, FormVWideExtractF64,
    FormVWideMaskBits,
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

    FormBlockCopyRep,
    FormBlockCopyUnrolled,
    FormBlockCopyUnrolledCount,
    FormBlockSetRep,
    FormBlockSetUnrolled,
    FormBlockSetUnrolledCount,

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
 * The table.
 */

MachineTarget::MachineTarget() {
    for(Size i = 0; i < kMachineOpcodeCount; i++) opcodes[i].id = MachineOpcodeId(i);

    auto name = [&](MachineOpcodeId id, StringView text, bool flagsSelective = false) {
        opcodes[id].name = text;
        opcodes[id].flagsSelective = flagsSelective;
    };

    name(OpNone, "none"_v);
    name(OpNop, "nop"_v);
    name(OpArg, "arg"_v);
    name(OpPhi, "phi"_v);

    // An immediate of zero is materialized with `xor r, r` rather than `mov r, 0` - two bytes
    // instead of five, at the cost of writing the flags, which the move does not. Which of the two
    // it is depends on the value alone and on nothing any peephole decides, which is what makes it
    // safe for the compare folding to ask this question while those passes are still running.
    name(OpImm, "imm"_v, true);

    name(OpGlobalAddress, "globaladdr"_v);
    name(OpFunctionAddress, "funaddr"_v);
    name(OpMove, "move"_v);

    // A cast whose source is an embedded constant is a materialization, and takes the same two forms
    // a materialization does: `xor r, r` for zero and `mov r, imm` for everything else. Which of the
    // two follows the constant's value alone, exactly as it does for OpImm above.
    name(OpCast, "cast"_v, true);

    // And a bitcast of one, for the same reason - and this is the pair that pays: `bitcast 0` is
    // what the lowering makes of every null pointer, where a cast of a constant is folded away
    // before it is ever built (foldCast in lower_builder.h) and only a hand-written .lower file
    // has one.
    //
    // Both are the first flags-selective opcodes whose answer moves the *wrong* way as the peepholes
    // run - the form that writes nothing is the one they start in. What makes that safe is the sweep
    // order rather than anything about these rows; see MachineOpcodeDesc::flagsSelective and §3.5.2
    // of the README.
    name(OpBitcast, "bitcast"_v, true);
    name(OpNeg, "neg"_v);
    name(OpNot, "not"_v);
    name(OpAdd, "add"_v);
    name(OpSub, "sub"_v);
    name(OpMul, "mul"_v);
    name(OpIMul, "imul"_v);
    name(OpDiv, "div"_v);
    name(OpIDiv, "idiv"_v);
    name(OpRem, "rem"_v);
    name(OpIRem, "irem"_v);
    name(OpMulHi, "mulhi"_v);
    name(OpIMulHi, "imulhi"_v);
    name(OpShl, "shl"_v);
    name(OpShr, "shr"_v);
    name(OpSar, "sar"_v);
    name(OpAnd, "and"_v);
    name(OpOr, "or"_v);
    name(OpXor, "xor"_v);
    // A comparison against zero whose answer the arithmetic above it already put in ZF emits
    // nothing and writes no flags, where every other form of this opcode writes them - so the two
    // do differ. Unlike the four selective opcodes above, which a *peephole* decides, this one is
    // decided by the compare folding itself, in the second sweep, after the last question anything
    // asks about a form's flags effect. See §3.5.2.2 of the README.
    name(OpCmp, "cmp"_v, true);

    // The packed set. None of them touches the flags at any lane type, which is what makes them the
    // one group here with nothing to declare: a comparison can be folded across a whole vector loop.
    name(OpVAdd, "vadd"_v);
    name(OpVSub, "vsub"_v);
    name(OpVMul, "vmul"_v);
    name(OpVDiv, "vdiv"_v);
    name(OpVAnd, "vand"_v);
    name(OpVOr, "vor"_v);
    name(OpVXor, "vxor"_v);
    name(OpVAndNot, "vandnot"_v);
    name(OpVShl, "vshl"_v);
    name(OpVShr, "vshr"_v);
    name(OpVSar, "vsar"_v);
    name(OpVCmp, "vcmp"_v);
    name(OpVAbs, "vabs"_v);
    name(OpVMin, "vmin"_v);
    name(OpVMax, "vmax"_v);
    name(OpVShuffle, "vshuffle"_v);
    name(OpVBroadcast, "vbroadcast"_v);
    name(OpVExtract, "vextract"_v);
    name(OpVMaskBits, "vmaskbits"_v);
    name(OpVInsert, "vinsert"_v);
    name(OpVBlend, "vblend"_v);
    name(OpVNot, "vnot"_v);
    name(OpVNeg, "vneg"_v);
    name(OpSqrt, "sqrt"_v);
    name(OpFma, "fma"_v);
    name(OpVZeroUpper, "vzeroupper"_v);

    name(OpFAdd, "fadd"_v);
    name(OpFSub, "fsub"_v);
    name(OpFMul, "fmul"_v);
    name(OpFDiv, "fdiv"_v);
    name(OpFNeg, "fneg"_v);
    name(OpFCmp, "fcmp"_v);

    // A select whose condition arrives in a register tests it first, and that test writes the flags;
    // one whose condition is already in the flags reads them and writes nothing.
    name(OpSelect, "select"_v, true);

    // A compile-time size is one `lea` and touches nothing; a run-time one rounds the size up and
    // moves the stack pointer, which writes the flags. Which of the two applies follows the count
    // being an embedded constant, so this is one of the opcodes whose selection a peephole moves -
    // see MachineOpcodeDesc::flagsSelective for why that is still safe.
    name(OpAlloca, "alloca"_v, true);

    name(OpLoad, "load"_v);
    name(OpStore, "store"_v);
    name(OpBlockCopy, "blockcopy"_v);
    name(OpBlockSet, "blockset"_v);
    name(OpCall, "call"_v);
    name(OpPushArg, "pusharg"_v);
    name(OpAddress, "address"_v);
    name(OpLea, "lea"_v);
    name(OpJmp, "jmp"_v);

    // As with the select above: a branch on a register tests it, a branch on the flags does not.
    name(OpJcc, "jcc"_v, true);

    name(OpRet, "ret"_v);

    // The end of a block control never leaves. Named like any other opcode so that the printers and
    // the verifiers have something to say about it, and encoding to nothing at all - see FormNoReturn.
    name(OpNoReturn, "noreturn"_v);

    // Each form is pushed in the order the ids above declare it, so that the id is its index.
    auto add = [&](MachineFormId id, MachineOpcodeId opcode, StringView formName) -> MachineForm& {
        assertTrue(forms.size() == id); // the form ids and the construction order have drifted apart

        forms.push(MachineForm {});

        auto& form = forms[forms.size() - 1];
        form.id = id;
        form.opcode = opcode;
        form.name = formName;
        return form;
    };

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
     * foldLoads in transform.cpp is what moves an instruction onto one, and §5 of
     * test/bench/findings.md is the measurement.
     */
    auto memoryTwin = [&](MachineFormId id, MachineFormId sourceId, StringView formName) -> MachineForm& {
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
    };

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
    auto dropTie = [](MachineForm& twin) {
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
    };

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
    auto prefixedTwin = [&](MachineFormId id, MachineFormId sourceId, StringView formName, bool threeOperand,
                            PrefixEncoding encoding, FeatureSet feature) -> MachineForm&
    {
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
    };

    auto vexTwin = [&](MachineFormId id, MachineFormId sourceId, StringView formName, bool threeOperand) -> MachineForm& {
        return prefixedTwin(id, sourceId, formName, threeOperand, PrefixEncoding::Vex, kFeatureAvx);
    };

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
    auto evexTwin = [&](MachineFormId id, MachineFormId sourceId, StringView formName, bool threeOperand) -> MachineForm& {
        return prefixedTwin(id, sourceId, formName, threeOperand, PrefixEncoding::Evex, kFeatureAvx512f);
    };

    /*
     * Instructions that emit nothing.
     */

    add(FormNop, OpNop, "nop"_v).encoding = EncodingDescriptor {
        .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::Nop,
    };

    add(FormArg, OpArg, "arg"_v);
    add(FormPhi, OpPhi, "phi"_v);

    // An addressing mode produces no code and no register of its own; its base and index are
    // ordinary register operands, read by whichever access folds it in.
    add(FormAddress, OpAddress, "address"_v).defs.push(noDef());

    /*
     * Constants and addresses.
     */

    {
        // The immediate is the value this instruction defines rather than an operand of it, which is
        // what `immField` naming a result says. Its byte width is chosen by the encoding: the
        // shortest of the three `mov` forms that reproduces the value exactly.
        auto& form = add(FormImmMov, OpImm, "mov r, imm"_v);
        form.defs.push(def());
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::MoveImm,
            .regField = defRef(0), .immField = defRef(0),
        };
    }

    {
        // `xor r, r`, which zeroes the whole register whatever the value's declared width. Two bytes
        // where `mov r, 0` is five, at the cost of the flags - which is the whole reason this is a
        // form of its own rather than an encoding detail.
        auto& form = add(FormImmZero, OpImm, "xor r, r"_v);
        form.defs.push(def());
        form.flagsEffect = FlagsEffect::Def;
        form.encoding = regRm(0x31, defRef(0), defRef(0));
        form.encoding.width = OperationWidth::Fixed32;
    }

    add(FormImmImplicit, OpImm, "imm (embedded)"_v).defs.push(noDef());

    // A floating-point constant, which no SSE encoding carries as an immediate and which this
    // backend has nowhere to put as a constant pool entry: it is materialized in a general register
    // and moved across the bank boundary. r11 is stated as a clobber rather than as a declared
    // expansion temporary for the reason the unrolled block copy states its scratch that way - a
    // clobber keeps a live value out of the register at this one instruction, where a declared
    // temporary would be held back from the whole function.
    //
    // The two forms differ only in the width the pair moves at, which the pseudo reads from the
    // result's own type.
    auto floatImm = [&](MachineFormId id, StringView formName, RegisterClassId cls) {
        auto& form = add(id, OpImm, formName);
        form.defs.push(def(cls));
        form.clobbers.add(gpr(IntRegister::r11));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::FloatImm,
        };
    };

    floatImm(FormImmFloat32, "mov r, imm; movd xmm, r"_v, ClassFloat32);
    floatImm(FormImmFloat64, "mov r, imm; movq xmm, r"_v, ClassFloat64);

    // RIP-relative, against a displacement that is only known once every function and global has
    // been emitted - so the address the legalizer resolves carries the symbol rather than bytes.
    auto symbolAddress = [&](MachineFormId id, MachineOpcodeId opcode, StringView formName) {
        auto& form = add(id, opcode, formName);
        form.defs.push(def());
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Lea,
            .opcode = 0x8d,
            .regField = defRef(0),
            .width = OperationWidth::Fixed64,
        };
    };

    // The elided twin of each: a direct call encodes its target as a rel32 and never reads the
    // address out of a register, and a global read or written through its own address is the same
    // case - `[rip + g]` is an addressing mode, so the access carries the symbol itself and there is
    // nothing left for a register to hold. Both emit no bytes at all.
    symbolAddress(FormGlobalAddress, OpGlobalAddress, "lea r, [rip + global]"_v);
    add(FormGlobalImplicit, OpGlobalAddress, "globaddr (folded)"_v).defs.push(noDef());

    symbolAddress(FormFunctionAddress, OpFunctionAddress, "lea r, [rip + fun]"_v);
    add(FormFunctionImplicit, OpFunctionAddress, "funaddr (elided)"_v).defs.push(noDef());

    /*
     * Moves and casts.
     */

    {
        // MOV r, r/m: a source still in the frame is read in place rather than reloaded into a
        // register the copy would then read again.
        //
        // Both ends are one type, so a register source needs no clearing and a copy between one
        // register and itself is nothing at all - which is what `omitWhenSame` says, and what lets
        // buildWebs coalesce across a `Set` rather than only across a bitcast. The cast that cannot
        // omit itself is `FormCastMov`, whose two ends are *not* one type.
        auto& form = add(FormMove, OpMove, "mov r, r/m"_v);
        form.uses.push(regOrMem(MemoryAccessKind::Read));
        form.defs.push(def());
        form.encoding = regRm(0x8b, defRef(0), useRef(0));
        form.encoding.omitWhenSame = true;
    }

    // MOVSS/MOVSD xmm, xmm/m: the same shape one bank over. A register source merges into the
    // destination's upper bytes rather than clearing them, which costs nothing here - the class is
    // the scalar view, so those bytes hold nothing this value or any other is relying on.
    //
    // A bitcast between two float types of one width is the same copy, and is a form of its own for
    // the same reason the integer bitcast is: it is a copy that emits nothing at all when the
    // allocator has already put source and destination in one register.
    auto floatMove = [&](MachineFormId id, MachineOpcodeId opcode, StringView formName, U8 prefix,
                         RegisterClassId cls, bool omitWhenSame)
    {
        auto& form = add(id, opcode, formName);
        form.uses.push(regOrMem(MemoryAccessKind::Read, cls));
        form.defs.push(def(cls));
        form.encoding = sseRegRm(prefix, 0x10, defRef(0), useRef(0), OperationWidth::FromResult);
        form.encoding.omitWhenSame = omitWhenSame;
    };

    floatMove(FormMoveF32, OpMove, "movss xmm, xmm/m"_v, 0xf3, ClassFloat32, false);
    floatMove(FormMoveF64, OpMove, "movsd xmm, xmm/m"_v, 0xf2, ClassFloat64, false);

    // Casts have no memory form: their source and result widths differ by definition, and a slot is
    // exactly as wide as the value in it, so an access at the other width would take a neighbour
    // with it.
    {
        // Moved at the narrower of the two widths. A 32-bit MOV always clears the upper half of its
        // destination, so one encoding both truncates a 64-bit source and zero-extends into a 64-bit
        // destination - which is what an unsigned cast means in either direction. The move is
        // emitted even between one register and itself, since that clearing is the whole point.
        auto& form = add(FormCastMov, OpCast, "mov r, r"_v);
        form.uses.push(anyReg());
        form.defs.push(def());
        form.encoding = regRm(0x8b, defRef(0), useRef(0));
        form.encoding.width = OperationWidth::Narrowest;
    }

    {
        // The same move, for a cast whose clearing has been shown to be a no-op: the source register
        // already holds the bits the destination has to end up with, so this emits nothing at all
        // once the two are in one register and an ordinary copy while they are not. That is the only
        // difference from the form above, which cannot omit itself for exactly the reason it exists.
        //
        // Which of the two an integer cast takes is trySkipCastExtend's answer, recorded on the
        // instruction; both write nothing to the flags, so choosing between them is not one of the
        // decisions OpCast's flags-selectiveness is about.
        auto& form = add(FormCastCopy, OpCast, "mov r, r (extended)"_v);
        form.uses.push(anyReg());
        form.defs.push(def());
        form.encoding = regRm(0x8b, defRef(0), useRef(0));
        form.encoding.width = OperationWidth::Narrowest;
        form.encoding.omitWhenSame = true;
    }

    {
        // Widening a signed value into a signed one is the one case that has to carry the sign bit
        // up rather than clear the upper half.
        auto& form = add(FormCastSext, OpCast, "movsxd r, r"_v);
        form.uses.push(anyReg());
        form.defs.push(def());
        form.encoding = regRm(0x63, defRef(0), useRef(0));
    }

    {
        // An immediate source makes the cast a constant materialization, already narrowed or widened
        // by the move encoding's own choice of width.
        auto& form = add(FormCastImm, OpCast, "mov r, imm"_v);
        form.uses.push(immediate(ImmediateWidth::Imm64));
        form.defs.push(def());
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::MoveImm,
            .regField = defRef(0), .immField = useRef(0),
        };
    }

    {
        // And zero is `xor r, r` here for the same reason it is under OpImm: two bytes where the
        // move is five, since the register is zeroed whole whatever width either end of the cast
        // declares. The immediate is still declared as the operand it is - what the source is has
        // not changed, only what the encoding does with it - so the operand accounting is the same
        // as the form above's and the peephole that embeds the constant sees one answer for both.
        auto& form = add(FormCastZero, OpCast, "xor r, r"_v);
        form.uses.push(immediate(ImmediateWidth::Imm64));
        form.defs.push(def());
        form.flagsEffect = FlagsEffect::Def;
        form.encoding = regRm(0x31, defRef(0), defRef(0));
        form.encoding.width = OperationWidth::Fixed32;
    }

    {
        // A bitcast between two integer classes is a copy and nothing more, so one between a
        // register and itself emits nothing at all.
        auto& form = add(FormBitcast, OpBitcast, "mov r, r"_v);
        form.uses.push(anyReg());
        form.defs.push(def());
        form.encoding = regRm(0x8b, defRef(0), useRef(0));
        form.encoding.omitWhenSame = true;
    }

    {
        auto& form = add(FormBitcastImm, OpBitcast, "mov r, imm"_v);
        form.uses.push(immediate(ImmediateWidth::Imm64));
        form.defs.push(def());
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::MoveImm,
            .regField = defRef(0), .immField = useRef(0),
        };
    }

    {
        // The zero of the pair, which is the one that matters here: `bitcast 0` is what the lowering
        // makes of a null pointer, and it is the only constant a bitcast is ever given.
        auto& form = add(FormBitcastZero, OpBitcast, "xor r, r"_v);
        form.uses.push(immediate(ImmediateWidth::Imm64));
        form.defs.push(def());
        form.flagsEffect = FlagsEffect::Def;
        form.encoding = regRm(0x31, defRef(0), defRef(0));
        form.encoding.width = OperationWidth::Fixed32;
    }

    /*
     * Conversions between the banks.
     *
     * These are the one place REX.W keeps its ordinary meaning on an SSE encoding: the mandatory
     * prefix states which *float* the instruction works with and REX.W states how wide the
     * *integer* it converts to or from is. So they take their width from whichever operand is the
     * integer one, and are the SSE forms that do not set `widthInPrefix`.
     *
     * Only the signed conversions exist. An unsigned one is not an encoding this instruction set
     * has: 64-bit unsigned needs a halve-convert-double sequence, and 32-bit unsigned needs its
     * source zero-extended into a register the 64-bit conversion can read - neither of which is a
     * form, and both of which selection rejects rather than quietly emitting the signed instruction.
     */

    auto intToFloat = [&](MachineFormId id, StringView formName, U8 prefix, RegisterClassId cls) {
        auto& form = add(id, OpCast, formName);
        form.uses.push(regOrMem(MemoryAccessKind::Read));
        form.defs.push(def(cls));
        form.encoding = sseRegRm(prefix, 0x2a, defRef(0), useRef(0), OperationWidth::FromUse0);
        form.encoding.widthInPrefix = false;

        // One lane written and the rest of the register left alone - see mergesIntoDestination.
        form.encoding.mergesIntoDestination = true;
    };

    intToFloat(FormCastIToF32, "cvtsi2ss xmm, r/m"_v, 0xf3, ClassFloat32);
    intToFloat(FormCastIToF64, "cvtsi2sd xmm, r/m"_v, 0xf2, ClassFloat64);

    // Truncating towards zero, which is what a cast to an integer means everywhere else in this
    // compiler; the rounding conversion is a different instruction and would be a different form.
    auto floatToInt = [&](MachineFormId id, StringView formName, U8 prefix, RegisterClassId cls) {
        auto& form = add(id, OpCast, formName);
        form.uses.push(regOrMem(MemoryAccessKind::Read, cls));
        form.defs.push(def());
        form.encoding = sseRegRm(prefix, 0x2c, defRef(0), useRef(0), OperationWidth::FromResult);
        form.encoding.widthInPrefix = false;
    };

    floatToInt(FormCastF32ToI, "cvttss2si r, xmm/m"_v, 0xf3, ClassFloat32);
    floatToInt(FormCastF64ToI, "cvttsd2si r, xmm/m"_v, 0xf2, ClassFloat64);

    // Between the two float widths, where the prefix is the whole of the width statement again: the
    // one that names the *source*, since that is what the instruction is reading.
    auto floatToFloat = [&](MachineFormId id, StringView formName, U8 prefix,
                            RegisterClassId from, RegisterClassId to)
    {
        auto& form = add(id, OpCast, formName);
        form.uses.push(regOrMem(MemoryAccessKind::Read, from));
        form.defs.push(def(to));
        form.encoding = sseRegRm(prefix, 0x5a, defRef(0), useRef(0), OperationWidth::FromUse0);
        form.encoding.mergesIntoDestination = true;
    };

    floatToFloat(FormCastF32ToF64, "cvtss2sd xmm, xmm/m"_v, 0xf3, ClassFloat32, ClassFloat64);
    floatToFloat(FormCastF64ToF32, "cvtsd2ss xmm, xmm/m"_v, 0xf2, ClassFloat64, ClassFloat32);

    /*
     * Bitcasts across the banks.
     *
     * MOVD/MOVQ, which are one opcode each way and differ in REX.W alone - a bitcast preserves the
     * width by definition, so there is nothing for a prefix to select and the width is fixed per
     * form rather than read from an operand.
     */

    auto floatToIntBits = [&](MachineFormId id, StringView formName, RegisterClassId from,
                              RegisterClassId to, OperationWidth width)
    {
        auto& form = add(id, OpBitcast, formName);
        form.uses.push(anyReg(from));
        form.defs.push(def(to));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = 0x7e, .escape = 0x0f, .prefix = 0x66,
            .regField = useRef(0), .rmField = defRef(0),
            .width = width,
        };
    };

    floatToIntBits(FormBitcastF32ToI, "movd r/m, xmm"_v, ClassFloat32, ClassGpr32, OperationWidth::Fixed32);
    floatToIntBits(FormBitcastF64ToI, "movq r/m, xmm"_v, ClassFloat64, ClassGpr64, OperationWidth::Fixed64);

    auto intToFloatBits = [&](MachineFormId id, StringView formName, RegisterClassId from,
                              RegisterClassId to, OperationWidth width)
    {
        auto& form = add(id, OpBitcast, formName);
        form.uses.push(regOrMem(MemoryAccessKind::Read, from));
        form.defs.push(def(to));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = 0x6e, .escape = 0x0f, .prefix = 0x66,
            .regField = defRef(0), .rmField = useRef(0),
            .width = width,
        };
    };

    intToFloatBits(FormBitcastIToF32, "movd xmm, r/m"_v, ClassGpr32, ClassFloat32, OperationWidth::Fixed32);
    intToFloatBits(FormBitcastIToF64, "movq xmm, r/m"_v, ClassGpr64, ClassFloat64, OperationWidth::Fixed64);

    floatMove(FormBitcastF32, OpBitcast, "movss xmm, xmm/m"_v, 0xf3, ClassFloat32, true);
    floatMove(FormBitcastF64, OpBitcast, "movsd xmm, xmm/m"_v, 0xf2, ClassFloat64, true);

    /*
     * Unary arithmetic.
     *
     * NEG and NOT take their subject as r/m, so a value the allocator left in the frame is negated
     * or inverted in place rather than loaded, changed and stored back.
     *
     * They share an encoding shape and differ in one thing beyond the opcode extension: `neg` is a
     * subtraction from zero and sets the flags accordingly, while `not` is a bitwise complement and
     * leaves them entirely alone. Saying otherwise costs a compare that could have been folded
     * across it.
     */

    auto unaryArith = [&](MachineFormId id, MachineOpcodeId opcode, StringView formName, U8 extension,
                          FlagsEffect flags)
    {
        auto& form = add(id, opcode, formName);
        form.uses.push(regOrMem(MemoryAccessKind::ReadWrite));
        form.defs.push(tiedDef(0));
        form.flagsEffect = flags;
        form.encoding = rmExt(0xf7, extension, useRef(0));
    };

    unaryArith(FormNeg, OpNeg, "neg r/m"_v, 3, FlagsEffect::Def);
    unaryArith(FormNot, OpNot, "not r/m"_v, 2, FlagsEffect::None);

    // `neg` sets ZF from the result the way the group-1 operations do; `not` writes no flag at all,
    // which is why a comparison of its result against zero is a comparison and not a redundancy.
    forms[FormNeg].resultInFlags = true;

    /*
     * The group-1 ALU operations.
     *
     * Two forms each. The register one can take either operand from memory - `add [slot], rcx` when
     * the result lives in that very slot, or `add rax, [slot]` when the right-hand side does - and
     * only one of the two at a time, because both want the single r/m field. The immediate one has
     * no register right-hand side to take from anywhere.
     */

    auto binaryAlu = [&](MachineFormId regId, MachineFormId immId, MachineFormId memId, MachineOpcodeId opcode,
                         StringView regName, StringView immName, StringView memName,
                         U8 rmRegOp, U8 regRmOp, U8 extension, bool logical)
    {
        auto& regForm = add(regId, opcode, regName);
        regForm.uses.push(regOrMem(MemoryAccessKind::ReadWrite));
        regForm.uses.push(regOrMem(MemoryAccessKind::Read));
        regForm.defs.push(tiedDef(0));
        regForm.flagsEffect = FlagsEffect::Def;
        regForm.resultInFlags = true;
        regForm.signInFlags = logical;
        regForm.encoding = regRm(rmRegOp, useRef(1), useRef(0), regRmOp);

        auto& immForm = add(immId, opcode, immName);
        immForm.uses.push(regOrMem(MemoryAccessKind::ReadWrite));
        immForm.uses.push(immediate(ImmediateWidth::Imm8OrImm32));
        immForm.defs.push(tiedDef(0));
        immForm.flagsEffect = FlagsEffect::Def;
        immForm.resultInFlags = true;
        immForm.signInFlags = logical;
        immForm.encoding = rmExtImm(0x83, 0x81, extension, useRef(0), useRef(1));

        // The twin is a copy of the register form, so it carries both flag claims with it - which is
        // right: reading an operand out of memory changes where the operands come from and nothing
        // about what the operation leaves in the flags.
        memoryTwin(memId, regId, memName);
    };

    // `logical` is the second claim: `and`, `or` and `xor` clear OF, so SF against OF is the sign of
    // their result and a signed comparison of it against zero is answered too. `add` and `sub` set OF
    // from the operation and are not that - see MachineForm::signInFlags.
    binaryAlu(FormAddReg, FormAddImm, FormAddMem, OpAdd,
        "add r/m, r"_v, "add r/m, imm"_v, "add r, [address]"_v, 0x01, 0x03, 0, false);
    binaryAlu(FormSubReg, FormSubImm, FormSubMem, OpSub,
        "sub r/m, r"_v, "sub r/m, imm"_v, "sub r, [address]"_v, 0x29, 0x2b, 5, false);
    binaryAlu(FormAndReg, FormAndImm, FormAndMem, OpAnd,
        "and r/m, r"_v, "and r/m, imm"_v, "and r, [address]"_v, 0x21, 0x23, 4, true);
    binaryAlu(FormOrReg, FormOrImm, FormOrMem, OpOr,
        "or r/m, r"_v, "or r/m, imm"_v, "or r, [address]"_v, 0x09, 0x0b, 1, true);
    binaryAlu(FormXorReg, FormXorImm, FormXorMem, OpXor,
        "xor r/m, r"_v, "xor r/m, imm"_v, "xor r, [address]"_v, 0x31, 0x33, 6, true);

    /*
     * Increment and decrement.
     *
     * `inc r/m` is one byte shorter than the `add r/m, 1` it replaces, and takes its subject as r/m
     * exactly as that does - so a destination the allocator left in the frame is incremented in
     * place like any other in-place accumulator. Which of the two an addition of one takes is
     * decided from the immediate alone, so this is a form rather than something the encoder notices.
     */

    auto unitStep = [&](MachineFormId id, MachineOpcodeId opcode, StringView formName, U8 extension) {
        auto& form = add(id, opcode, formName);
        form.uses.push(regOrMem(MemoryAccessKind::ReadWrite));
        form.uses.push(immediate(ImmediateWidth::Imm8OrImm32));
        form.defs.push(tiedDef(0));
        form.flagsEffect = FlagsEffect::Def;
        form.resultInFlags = true;
        form.encoding = rmExt(0xff, extension, useRef(0));
    };

    unitStep(FormAddInc, OpAdd, "inc r/m"_v, 0);
    unitStep(FormAddDec, OpAdd, "dec r/m"_v, 1);
    unitStep(FormSubInc, OpSub, "inc r/m"_v, 0);
    unitStep(FormSubDec, OpSub, "dec r/m"_v, 1);

    /*
     * Multiply and divide.
     *
     * The group-3 forms read their first operand out of rax and write their result back into it (or,
     * for a remainder, into rdx), and take the second as r/m - so a divisor can come straight out of
     * the frame with no reload. rdx is written either way, which is why it is a clobber even where it
     * is also the result: a value living there has to be out of the way regardless.
     */

    auto group3 = [&](MachineFormId id, MachineOpcodeId opcode, StringView formName,
                      IntRegister result, bool clobberRax, U8 extension, EncodingPrelude prelude)
    {
        auto& form = add(id, opcode, formName);
        form.uses.push(fixedReg(IntRegister::rax));
        form.uses.push(regOrMem(MemoryAccessKind::Read));

        auto resultDef = def();
        resultDef.kind = OperandConstraintKind::FixedRegister;
        resultDef.fixedReg = gpr(result);
        form.defs.push(resultDef);

        form.clobbers.add(gpr(IntRegister::rdx));
        if(clobberRax) form.clobbers.add(gpr(IntRegister::rax));

        form.flagsEffect = FlagsEffect::Def;
        form.encoding = rmExt(0xf7, extension, useRef(1));
        form.encoding.prelude = prelude;
    };

    group3(FormMul, OpMul, "mul r/m"_v, IntRegister::rax, false, 4, EncodingPrelude::None);
    group3(FormDiv, OpDiv, "div r/m"_v, IntRegister::rax, false, 6, EncodingPrelude::ZeroRdx);
    group3(FormIDiv, OpIDiv, "idiv r/m"_v, IntRegister::rax, false, 7, EncodingPrelude::SignExtendRax);
    group3(FormRem, OpRem, "div r/m (remainder)"_v, IntRegister::rdx, true, 6, EncodingPrelude::ZeroRdx);
    group3(FormIRem, OpIRem, "idiv r/m (remainder)"_v, IntRegister::rdx, true, 7, EncodingPrelude::SignExtendRax);

    // The same two multiplies, read for the half they are usually asked to throw away. No prelude:
    // unlike a division, a multiply *writes* the whole pair rather than reading it, so nothing has
    // to be in rdx beforehand.
    group3(FormMulHi, OpMulHi, "mul r/m (high)"_v, IntRegister::rdx, true, 4, EncodingPrelude::None);
    group3(FormIMulHi, OpIMulHi, "imul r/m (high)"_v, IntRegister::rdx, true, 5, EncodingPrelude::None);

    {
        // IMUL r, r/m is the two-operand form: the destination doubles as a source, so it is
        // destructive like the group-1 operations. Only one direction exists - the destination is
        // always the reg field - so a spilled operand can only ever be the right-hand side.
        auto& form = add(FormIMulReg, OpIMul, "imul r, r/m"_v);
        form.uses.push(anyReg());
        form.uses.push(regOrMem(MemoryAccessKind::Read));
        form.defs.push(tiedDef(0));
        form.flagsEffect = FlagsEffect::Def;
        form.encoding = regRm(0xaf, useRef(0), useRef(1));
        form.encoding.escape = 0x0f;
    }

    memoryTwin(FormIMulMem, FormIMulReg, "imul r, [address]"_v);

    {
        // IMUL r, r/m, imm is a true three-operand form - the destination can differ from the source
        // - which is why the immediate case is not destructive where the register case is.
        auto& form = add(FormIMulImm, OpIMul, "imul r, r, imm"_v);
        form.uses.push(anyReg());
        form.uses.push(immediate(ImmediateWidth::Imm8OrImm32));
        form.defs.push(def());
        form.flagsEffect = FlagsEffect::Def;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRmImm,
            .opcode = 0x6b, .opcodeAlt = 0x69,
            .regField = defRef(0), .rmField = useRef(0), .immField = useRef(1),
        };
    }

    /*
     * Shifts.
     *
     * Every shift form takes its subject as r/m, so a destination in the frame is shifted in place.
     * The count is either an immediate in the instruction or in cl, and is never the memory operand.
     */

    auto shift = [&](MachineFormId immId, MachineFormId oneId, MachineFormId clId, MachineOpcodeId opcode,
                     StringView immName, StringView oneName, StringView clName, U8 extension)
    {
        auto& immForm = add(immId, opcode, immName);
        immForm.uses.push(regOrMem(MemoryAccessKind::ReadWrite));
        immForm.uses.push(immediate(ImmediateWidth::Imm8));
        immForm.defs.push(tiedDef(0));
        immForm.flagsEffect = FlagsEffect::Def;
        immForm.encoding = rmExtImm(0xc1, 0, extension, useRef(0), useRef(1));

        // A shift by one has an encoding with no immediate byte at all, which is a form rather than
        // an encoder's notice for the same reason `inc` is: it is decided by the value alone.
        auto& oneForm = add(oneId, opcode, oneName);
        oneForm.uses.push(regOrMem(MemoryAccessKind::ReadWrite));
        oneForm.uses.push(immediate(ImmediateWidth::Imm8));
        oneForm.defs.push(tiedDef(0));
        oneForm.flagsEffect = FlagsEffect::Def;
        oneForm.encoding = rmExt(0xd1, extension, useRef(0));

        auto& clForm = add(clId, opcode, clName);
        clForm.uses.push(regOrMem(MemoryAccessKind::ReadWrite));
        clForm.uses.push(fixedReg(IntRegister::rcx));
        clForm.defs.push(tiedDef(0));
        clForm.flagsEffect = FlagsEffect::Def;
        clForm.encoding = rmExt(0xd3, extension, useRef(0));
    };

    shift(FormShlImm, FormShlOne, FormShlCl, OpShl, "shl r/m, imm"_v, "shl r/m, 1"_v, "shl r/m, cl"_v, 4);
    shift(FormShrImm, FormShrOne, FormShrCl, OpShr, "shr r/m, imm"_v, "shr r/m, 1"_v, "shr r/m, cl"_v, 5);
    shift(FormSarImm, FormSarOne, FormSarCl, OpSar, "sar r/m, imm"_v, "sar r/m, 1"_v, "sar r/m, cl"_v, 7);

    /*
     * Comparison.
     *
     * A comparison works at the width of the values compared, not at the width of what it produces:
     * its result is an Int32 whatever went into it, so `widthFromUse` points at the left-hand side.
     * The result is written to a register only when the flags could not be carried to its use
     * directly, which the compare folding decides.
     */

    // Two forms each, differing only in whether the flags are materialized afterwards: `setcc` into
    // the result's low byte and a zero-extension over the rest of it. Which applies is the compare
    // folding's answer, recorded on the instruction as an implicit result.
    auto compare = [&](MachineFormId flagsId, MachineFormId setId, StringView flagsName, StringView setName,
                       const MachineOperandConstraint& rhs, EncodingDescriptor encoding)
    {
        auto& flagsForm = add(flagsId, OpCmp, flagsName);
        flagsForm.uses.push(anyReg());
        flagsForm.uses.push(rhs);
        flagsForm.defs.push(noDef());
        flagsForm.flagsEffect = FlagsEffect::Def;
        flagsForm.encoding = encoding;
        flagsForm.encoding.width = OperationWidth::FromUse0;

        auto& setForm = add(setId, OpCmp, setName);
        setForm.uses.push(anyReg());
        setForm.uses.push(rhs);
        setForm.defs.push(def(ClassGpr32));
        setForm.flagsEffect = FlagsEffect::Def;
        setForm.encoding = encoding;
        setForm.encoding.width = OperationWidth::FromUse0;
        setForm.encoding.materializeFlags = true;
    };

    compare(FormCmpReg, FormCmpRegSet, "cmp r, r/m"_v, "cmp r, r/m; setcc r"_v,
        regOrMem(MemoryAccessKind::Read), regRm(0x39, useRef(1), useRef(0), 0x3b));

    memoryTwin(FormCmpMem, FormCmpReg, "cmp r, [address]"_v);
    memoryTwin(FormCmpMemSet, FormCmpRegSet, "cmp r, [address]; setcc r"_v);

    // A comparison against zero has a shorter equivalent in `test r, r`, which leaves every
    // condition code this backend reads in the same state. It needs the value in a register, so the
    // descriptor states it as the alternative and an operand still in the frame keeps the `cmp`.
    auto cmpImm = rmExtImm(0x83, 0x81, 7, useRef(0), useRef(1));
    cmpImm.zeroRegOpcode = 0x85;

    compare(FormCmpImm, FormCmpImmSet, "cmp r/m, imm"_v, "cmp r/m, imm; setcc r"_v,
        immediate(ImmediateWidth::Imm8OrImm32), cmpImm);

    {
        /*
         * §3.5.2.2 And the comparison that emits nothing, because the instruction that produced its
         * left-hand side already left the answer in ZF.
         *
         * The operands stay ordinary operands. Nothing is folded and nothing is declared implicit:
         * the value being compared is one some other instruction computed and some other instruction
         * reads, and saying that this one no longer names it would shorten a live range for no gain
         * and leave the arithmetic looking like a definition nothing wants. What changes is the
         * encoding and the flags effect, and those are the whole of the difference.
         *
         * The operands are declared exactly as FormCmpImm's are, which is what makes the choice
         * between the two free: the elision is decided after every allocation question has been
         * asked, so a form demanding anything different would be a demand nothing could still meet.
         * The right-hand side is the embedded constant zero, always - `tryElideCompare` admits no
         * other - and the left-hand side wants a register there for the same reason it wants one in
         * FormCmpImm, which is that a comparison against an immediate has no memory form.
         */
        auto& form = add(FormCmpNone, OpCmp, "cmp (already in flags)"_v);
        form.uses.push(anyReg());
        form.uses.push(immediate(ImmediateWidth::Imm8OrImm32));
        form.defs.push(noDef());
        form.flagsEffect = FlagsEffect::None;
    }

    /*
     * Scalar floating-point arithmetic.
     *
     * Destructive in the same way the group-1 integer operations are, and constrained rather more:
     * there is only one direction, so the operand that may stay in the frame is always the
     * right-hand side, and there is no immediate form at all - which is why a float constant is
     * never embedded and always materialized (see isEmbeddableImm in transform.cpp).
     *
     * None of them touches the flags. That is a real difference from the integer opcodes rather
     * than a convenience: it is what lets a comparison be folded across a stretch of floating-point
     * arithmetic into the branch that reads it.
     */

    auto floatArith = [&](MachineFormId f32, MachineFormId f64, MachineOpcodeId opcode,
                          StringView name32, StringView name64, U8 op)
    {
        auto build = [&](MachineFormId id, StringView formName, U8 prefix, RegisterClassId cls) {
            auto& form = add(id, opcode, formName);
            form.uses.push(anyReg(cls));
            form.uses.push(regOrMem(MemoryAccessKind::Read, cls));
            form.defs.push(tiedDef(0, cls));
            form.encoding = sseRegRm(prefix, op, useRef(0), useRef(1), OperationWidth::FromResult);
        };

        build(f32, name32, 0xf3, ClassFloat32);
        build(f64, name64, 0xf2, ClassFloat64);
    };

    floatArith(FormFAdd32, FormFAdd64, OpFAdd, "addss xmm, xmm/m"_v, "addsd xmm, xmm/m"_v, 0x58);
    floatArith(FormFSub32, FormFSub64, OpFSub, "subss xmm, xmm/m"_v, "subsd xmm, xmm/m"_v, 0x5c);
    floatArith(FormFMul32, FormFMul64, OpFMul, "mulss xmm, xmm/m"_v, "mulsd xmm, xmm/m"_v, 0x59);
    floatArith(FormFDiv32, FormFDiv64, OpFDiv, "divss xmm, xmm/m"_v, "divsd xmm, xmm/m"_v, 0x5e);

    /*
     * Floating-point negation.
     *
     * AMD64 has no scalar float negate, so it is an exclusive-or against a sign mask - and the mask
     * is sixteen bytes, which is why this had to wait for the constant pool. Until it existed the
     * sign bit was toggled in a general register instead: `movq r, xmm; btc r, 63; movq xmm, r`,
     * three instructions, a bank crossing in each direction, and r11 declared as a clobber so that
     * nothing live could be sitting in it.
     *
     * That is now `xorps xmm, [rip + m]` - one instruction and no general register. The third gain
     * is the one that reaches past the negation itself: **these no longer touch the flags**, where
     * `btc` wrote the carry, so a negation may now sit inside a comparison's fold window.
     *
     * The mask is on the MachineFunction rather than in the operand list, because the encoding names
     * it and the allocator therefore has nothing to place - see `poolSignMasks`.
     */

    auto floatNeg = [&](MachineFormId id, StringView formName, RegisterClassId cls, OperationWidth width) {
        auto& form = add(id, OpFNeg, formName);
        form.uses.push(anyReg(cls));
        form.defs.push(tiedDef(0, cls));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::FloatNeg,
            .width = width,
        };
    };

    floatNeg(FormFNeg32, "xorps xmm, [rip + signmask]"_v, ClassFloat32, OperationWidth::Fixed32);
    floatNeg(FormFNeg64, "xorpd xmm, [rip + signmask]"_v, ClassFloat64, OperationWidth::Fixed64);

    // The memory sources of the four above, in the same order the enum declares them. Scalar SSE has
    // one direction only, so each twin is its source's own encoding with the r/m field addressed.
    memoryTwin(FormFAdd32Mem, FormFAdd32, "addss xmm, [address]"_v);
    memoryTwin(FormFAdd64Mem, FormFAdd64, "addsd xmm, [address]"_v);
    memoryTwin(FormFSub32Mem, FormFSub32, "subss xmm, [address]"_v);
    memoryTwin(FormFSub64Mem, FormFSub64, "subsd xmm, [address]"_v);
    memoryTwin(FormFMul32Mem, FormFMul32, "mulss xmm, [address]"_v);
    memoryTwin(FormFMul64Mem, FormFMul64, "mulsd xmm, [address]"_v);
    memoryTwin(FormFDiv32Mem, FormFDiv32, "divss xmm, [address]"_v);
    memoryTwin(FormFDiv64Mem, FormFDiv64, "divsd xmm, [address]"_v);

    /*
     * Floating-point comparison.
     *
     * UCOMISS/UCOMISD leave the result in the same flags an unsigned integer comparison does - CF
     * for "below", ZF for "equal" - so every condition code the rest of this backend already writes
     * reads correctly, and, crucially, so does the *negation* of each: `ja` and `jbe` remain exact
     * opposites, which is what the branch and select forms rely on when they flip a condition to
     * fall through the other way.
     *
     * An operand that is NaN sets CF, ZF and PF together, and that is the whole of what makes these
     * forms not enough on their own. Two things arrange the rest, neither of them here:
     *
     *  - orderFloatCompare exchanges the operands of `lt` and `le` so that every ordering comparison
     *    reaching selection is `gt` or `ge`. Those read CF, which a NaN sets, so they answer false -
     *    which is what an ordered comparison of a NaN has to do.
     *  - equality is not a condition code at all, since it needs ZF *and* PF. tryMergeCompare
     *    therefore refuses to leave one in the flags, and genFloatFlagsToReg writes the answer into
     *    the register with the parity correction attached.
     *
     * So all six agree with the LLVM backend and with the JavaScript one: every ordered comparison
     * of a NaN is false, and `!=` alone is true.
     */

    auto floatCompare = [&](MachineFormId flagsId, MachineFormId setId, StringView flagsName,
                            StringView setName, U8 prefix, RegisterClassId cls)
    {
        auto encoding = sseRegRm(prefix, 0x2e, useRef(0), useRef(1), OperationWidth::FromUse0);

        auto& flagsForm = add(flagsId, OpFCmp, flagsName);
        flagsForm.uses.push(anyReg(cls));
        flagsForm.uses.push(regOrMem(MemoryAccessKind::Read, cls));
        flagsForm.defs.push(noDef());
        flagsForm.flagsEffect = FlagsEffect::Def;
        flagsForm.encoding = encoding;

        auto& setForm = add(setId, OpFCmp, setName);
        setForm.uses.push(anyReg(cls));
        setForm.uses.push(regOrMem(MemoryAccessKind::Read, cls));
        setForm.defs.push(def(ClassGpr32));
        setForm.flagsEffect = FlagsEffect::Def;
        setForm.encoding = encoding;
        setForm.encoding.materializeFlags = true;
    };

    floatCompare(FormFCmp32, FormFCmp32Set, "ucomiss xmm, xmm/m"_v, "ucomiss xmm, xmm/m; setcc r"_v,
        0, ClassFloat32);
    floatCompare(FormFCmp64, FormFCmp64Set, "ucomisd xmm, xmm/m"_v, "ucomisd xmm, xmm/m; setcc r"_v,
        0x66, ClassFloat64);

    memoryTwin(FormFCmp32Mem, FormFCmp32, "ucomiss xmm, [address]"_v);
    memoryTwin(FormFCmp32MemSet, FormFCmp32Set, "ucomiss xmm, [address]; setcc r"_v);
    memoryTwin(FormFCmp64Mem, FormFCmp64, "ucomisd xmm, [address]"_v);
    memoryTwin(FormFCmp64MemSet, FormFCmp64Set, "ucomisd xmm, [address]; setcc r"_v);

    /*
     * And every one of them again with a VEX prefix, for a target that has AVX.
     *
     * Selection takes these in place of the forms above wherever the feature is present, and takes
     * the memory twin of whichever it landed on - see selectForm. Nothing else in the backend knows
     * they exist: they are the same opcodes with the same operands and the same effects, so every
     * peephole, every costing and every verifier reads them exactly as it reads the originals.
     *
     * The arithmetic is where the win is, and it is not the one byte the prefix costs or saves. A
     * three-operand encoding needs no copy in front of it to satisfy a tie, which is one instruction
     * removed from every floating-point operation whose result outlives one of its sources.
     *
     * The negation is here too and is the same change made to a pseudo: `vxorps` against the pooled
     * sign mask names its destination separately, where `xorps` toggled the bit in place.
     *
     * The rest of the scalar float set - the conversions, the constant materialization, the select -
     * used to be deliberately absent, on the grounds that none of them carries a tie worth removing
     * and that mixing the two spellings costs nothing while no 256-bit instruction is written. **The
     * second half of that stopped being true when the wide tier landed**, and it was the reasoning
     * rather than the list that was wrong: a legacy encoding is not merely a longer spelling, it is
     * a *partial write* of a register whose upper half something else may have dirtied. So every one
     * of them has a twin now, built by the sweep at the end of this constructor rather than listed
     * here - what is written out by hand is the forms that also need an EVEX tier above them, which
     * is what these are.
     */

    // One operation, both tiers, at both widths, with and without a folded address: sixteen forms
    // from one call, and the only thing stated sixteen times is a name.
    auto vexArith = [&](MachineFormId f32, MachineFormId f64, MachineFormId f32Mem, MachineFormId f64Mem,
                        MachineFormId e32, MachineFormId e64, MachineFormId e32Mem, MachineFormId e64Mem,
                        MachineFormId source32, MachineFormId source64, MachineFormId sourceMem32,
                        MachineFormId sourceMem64, StringView name32, StringView name64,
                        StringView memName32, StringView memName64,
                        StringView eName32, StringView eName64, StringView eMemName32, StringView eMemName64)
    {
        vexTwin(f32, source32, name32, true);
        vexTwin(f64, source64, name64, true);

        // The memory twins are built from the *VEX* forms rather than derived from the legacy memory
        // ones, so that the three-operand shape and the folded address come from one place. Their
        // legacy counterparts get the link as well, which makes the two swaps commute: whichever of
        // them selection applies first, the other still finds what it is looking for.
        memoryTwin(f32Mem, f32, memName32).alternativeOf = sourceMem32;
        memoryTwin(f64Mem, f64, memName64).alternativeOf = sourceMem64;

        forms[sourceMem32].alternative = f32Mem;
        forms[sourceMem64].alternative = f64Mem;

        // And the same again one tier up, out of the VEX forms.
        evexTwin(e32, f32, eName32, true);
        evexTwin(e64, f64, eName64, true);

        memoryTwin(e32Mem, e32, eMemName32).alternativeOf = f32Mem;
        memoryTwin(e64Mem, e64, eMemName64).alternativeOf = f64Mem;

        forms[f32Mem].alternative = e32Mem;
        forms[f64Mem].alternative = e64Mem;
    };

    vexArith(FormFAdd32Vex, FormFAdd64Vex, FormFAdd32VexMem, FormFAdd64VexMem,
        FormFAdd32Evex, FormFAdd64Evex, FormFAdd32EvexMem, FormFAdd64EvexMem,
        FormFAdd32, FormFAdd64, FormFAdd32Mem, FormFAdd64Mem,
        "vaddss xmm, xmm, xmm/m"_v, "vaddsd xmm, xmm, xmm/m"_v,
        "vaddss xmm, xmm, [address]"_v, "vaddsd xmm, xmm, [address]"_v,
        "vaddss (evex) xmm, xmm, xmm/m"_v, "vaddsd (evex) xmm, xmm, xmm/m"_v,
        "vaddss (evex) xmm, xmm, [address]"_v, "vaddsd (evex) xmm, xmm, [address]"_v);

    vexArith(FormFSub32Vex, FormFSub64Vex, FormFSub32VexMem, FormFSub64VexMem,
        FormFSub32Evex, FormFSub64Evex, FormFSub32EvexMem, FormFSub64EvexMem,
        FormFSub32, FormFSub64, FormFSub32Mem, FormFSub64Mem,
        "vsubss xmm, xmm, xmm/m"_v, "vsubsd xmm, xmm, xmm/m"_v,
        "vsubss xmm, xmm, [address]"_v, "vsubsd xmm, xmm, [address]"_v,
        "vsubss (evex) xmm, xmm, xmm/m"_v, "vsubsd (evex) xmm, xmm, xmm/m"_v,
        "vsubss (evex) xmm, xmm, [address]"_v, "vsubsd (evex) xmm, xmm, [address]"_v);

    vexArith(FormFMul32Vex, FormFMul64Vex, FormFMul32VexMem, FormFMul64VexMem,
        FormFMul32Evex, FormFMul64Evex, FormFMul32EvexMem, FormFMul64EvexMem,
        FormFMul32, FormFMul64, FormFMul32Mem, FormFMul64Mem,
        "vmulss xmm, xmm, xmm/m"_v, "vmulsd xmm, xmm, xmm/m"_v,
        "vmulss xmm, xmm, [address]"_v, "vmulsd xmm, xmm, [address]"_v,
        "vmulss (evex) xmm, xmm, xmm/m"_v, "vmulsd (evex) xmm, xmm, xmm/m"_v,
        "vmulss (evex) xmm, xmm, [address]"_v, "vmulsd (evex) xmm, xmm, [address]"_v);

    vexArith(FormFDiv32Vex, FormFDiv64Vex, FormFDiv32VexMem, FormFDiv64VexMem,
        FormFDiv32Evex, FormFDiv64Evex, FormFDiv32EvexMem, FormFDiv64EvexMem,
        FormFDiv32, FormFDiv64, FormFDiv32Mem, FormFDiv64Mem,
        "vdivss xmm, xmm, xmm/m"_v, "vdivsd xmm, xmm, xmm/m"_v,
        "vdivss xmm, xmm, [address]"_v, "vdivsd xmm, xmm, [address]"_v,
        "vdivss (evex) xmm, xmm, xmm/m"_v, "vdivsd (evex) xmm, xmm, xmm/m"_v,
        "vdivss (evex) xmm, xmm, [address]"_v, "vdivsd (evex) xmm, xmm, [address]"_v);

    vexTwin(FormFNeg32Vex, FormFNeg32, "vxorps xmm, xmm, [rip + signmask]"_v, true);
    vexTwin(FormFNeg64Vex, FormFNeg64, "vxorpd xmm, xmm, [rip + signmask]"_v, true);

    vexTwin(FormFCmp32Vex, FormFCmp32, "vucomiss xmm, xmm/m"_v, false);
    vexTwin(FormFCmp32VexSet, FormFCmp32Set, "vucomiss xmm, xmm/m; setcc r"_v, false);
    vexTwin(FormFCmp64Vex, FormFCmp64, "vucomisd xmm, xmm/m"_v, false);
    vexTwin(FormFCmp64VexSet, FormFCmp64Set, "vucomisd xmm, xmm/m; setcc r"_v, false);

    memoryTwin(FormFCmp32VexMem, FormFCmp32Vex, "vucomiss xmm, [address]"_v).alternativeOf = FormFCmp32Mem;
    memoryTwin(FormFCmp32VexMemSet, FormFCmp32VexSet, "vucomiss xmm, [address]; setcc r"_v).alternativeOf = FormFCmp32MemSet;
    memoryTwin(FormFCmp64VexMem, FormFCmp64Vex, "vucomisd xmm, [address]"_v).alternativeOf = FormFCmp64Mem;
    memoryTwin(FormFCmp64VexMemSet, FormFCmp64VexSet, "vucomisd xmm, [address]; setcc r"_v).alternativeOf = FormFCmp64MemSet;

    forms[FormFCmp32Mem].alternative = FormFCmp32VexMem;
    forms[FormFCmp32MemSet].alternative = FormFCmp32VexMemSet;
    forms[FormFCmp64Mem].alternative = FormFCmp64VexMem;
    forms[FormFCmp64MemSet].alternative = FormFCmp64VexMemSet;

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
    {
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

        /*
         * Shifts by a constant count every lane shares.
         *
         * The opcode byte is shared between the three shift directions and ModRM.reg carries the
         * extension that says which, exactly as the scalar group-2 shifts do. There is no register
         * form here: the machine's is a count in the low quadword of a *vector* register, and the
         * IR's register spelling is a scalar in a general one, so the two do not meet without a
         * transfer this backend has yet to write.
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
         * Comparison into a mask.
         *
         * The integer comparisons are two relations - equal and signed greater - and everything else
         * is reached by swapping the operands or inverting the result, which is what
         * `selectFormForTarget` does rather than this table. There is no unsigned packed compare at
         * all, and no quadword one before SSE4.1.
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
        packed(FormVCmpGt8,  OpVCmp, "pcmpgtb xmm, xmm/m"_v, 0x66, 0x64);
        packed(FormVCmpGt16, OpVCmp, "pcmpgtw xmm, xmm/m"_v, 0x66, 0x65);
        packed(FormVCmpGt32, OpVCmp, "pcmpgtd xmm, xmm/m"_v, 0x66, 0x66);

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
        auto extractInt = [&](MachineFormId id, StringView formName, RegisterClassId cls, bool wide) {
            auto& form = add(id, OpVExtract, formName);
            form.uses.push(anyReg(ClassXmm128));
            form.defs.push(def(cls));
            form.requiredFeatures = kFeatureBaseline;
            form.encoding = EncodingDescriptor {
                .family = EncodingFamily::RegRm,
                .opcode = 0x16,
                .escape = 0x0f, .prefix = 0x66,
                .regField = useRef(0), .rmField = defRef(0),
                .width = wide ? OperationWidth::Fixed64 : OperationWidth::Fixed32,
                .opcodeMap = kOpcodeMap0F3A,
            };
            form.encoding.patternImmediate = true;
        };

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

        wideTwin(FormVWideDivF32, FormVDivF32, "vdivps ymm, ymm, ymm/m"_v);
        wideTwin(FormVWideDivF64, FormVDivF64, "vdivpd ymm, ymm, ymm/m"_v);

        wideTwin(FormVWideAnd,    FormVAnd,    "vpand ymm, ymm, ymm/m"_v);
        wideTwin(FormVWideOr,     FormVOr,     "vpor ymm, ymm, ymm/m"_v);
        wideTwin(FormVWideXor,    FormVXor,    "vpxor ymm, ymm, ymm/m"_v);
        wideTwin(FormVWideAndNot, FormVAndNot, "vpandn ymm, ymm, ymm/m"_v);
        wideTwin(FormVWideAndF32, FormVAndF32, "vandps ymm, ymm, ymm/m"_v);
        wideTwin(FormVWideAndF64, FormVAndF64, "vandpd ymm, ymm, ymm/m"_v);

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
        wideTwin(FormVWideCmpGt8,  FormVCmpGt8,  "vpcmpgtb ymm, ymm, ymm/m"_v);
        wideTwin(FormVWideCmpGt16, FormVCmpGt16, "vpcmpgtw ymm, ymm, ymm/m"_v);
        wideTwin(FormVWideCmpGt32, FormVCmpGt32, "vpcmpgtd ymm, ymm, ymm/m"_v);
        wideTwin(FormVWideCmpF32,  FormVCmpF32,  "vcmpps ymm, ymm, ymm/m, predicate"_v);
        wideTwin(FormVWideCmpF64,  FormVCmpF64,  "vcmppd ymm, ymm, ymm/m, predicate"_v);

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

        // The four pseudos, which keep their ties and their clobber. What changes for each is the
        // width its emitter writes - see genPackedTwoAddress - and xmm15 becoming ymm15, which is
        // the same physical register named at the width the expansion now works at.
        wideTwin(FormVWideCmpInverted, FormVCmpInverted, "vpcmpXX; vpcmpeqd; vpxor (inverted)"_v);
        wideTwin(FormVWideNot,         FormVNot,         "vpcmpeqd; vpxor (complement)"_v);
        wideTwin(FormVWideSelect,      FormVSelect,      "vpand; vpandn; vpor (lanewise select)"_v);

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

        wideTwin(FormVWideBitcast, FormVBitcast, "vmovaps ymm, ymm"_v);
        wideTwin(FormVWideMove,    FormVMove,    "vmovaps ymm, ymm"_v);

        // Eight lanes each side rather than four, which is the whole of what widens: the lane count
        // is preserved by a `Cast` between vectors and the register width follows it.
        wideTwin(FormVWideCastIToF32, FormVCastIToF32, "vcvtdq2ps ymm, ymm"_v);
        wideTwin(FormVWideCastFToI32, FormVCastFToI32, "vcvttps2dq ymm, ymm"_v);

        wideTwin(FormVWideSqrtF32, FormVSqrtF32, "vsqrtps ymm, ymm"_v);
        wideTwin(FormVWideSqrtF64, FormVSqrtF64, "vsqrtpd ymm, ymm"_v);

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
            if(scratch) form.clobbers.add(vectorReg(15));
            form.encoding = EncodingDescriptor {
                .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::VecWideLane,
            };
        };

        wideExtract(FormVWideExtract32,  "vextracti128; vpextrd r32, xmm, lane"_v, ClassGpr32, true);
        wideExtract(FormVWideExtract64,  "vextracti128; vpextrq r64, xmm, lane"_v, ClassGpr64, true);
        wideExtract(FormVWideExtractF32, "vextractf128; vpshufd xmm, xmm, lane"_v, ClassFloat32, false);
        wideExtract(FormVWideExtractF64, "vextractf128; vpshufd xmm, xmm, lane"_v, ClassFloat64, false);

        // `vpmovmskb r32, ymm` - the one wide row that is a real encoding rather than a pseudo,
        // because AVX2 widened the instruction itself: thirty-two bytes in, thirty-two bits out, and
        // the general register it writes is the same width at either tier.
        wideTwin(FormVWideMaskBits, FormVMaskBits, "vpmovmskb r32, ymm"_v);

        auto wideInsert = [&](MachineFormId id, StringView formName, RegisterClassId cls) {
            auto& form = add(id, OpVInsert, formName);
            form.uses.push(anyReg(ClassYmm256));
            form.uses.push(anyReg(cls));
            form.defs.push(def(ClassYmm256));
            form.requiredFeatures = kFeatureAvx | kFeatureAvx2;
            form.clobbers.add(vectorReg(15));
            form.encoding = EncodingDescriptor {
                .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::VecWideWithLane,
            };
        };

        wideInsert(FormVWideInsert32,  "vextracti128; vpinsrd; vinserti128"_v,  ClassGpr32);
        wideInsert(FormVWideInsert64,  "vextracti128; vpinsrq; vinserti128"_v,  ClassGpr64);
        wideInsert(FormVWideInsertF32, "vextractf128; vinsertps; vinserti128"_v, ClassFloat32);
        wideInsert(FormVWideInsertF64, "vextractf128; vmovsd/vunpcklpd; vinserti128"_v, ClassFloat64);
    }

    /*
     * Select.
     */

    {
        // CMOVcc r, r/m: the destination doubles as a source, since it keeps its own value when the
        // condition does not hold. The flags were set by a comparison this select consumed.
        auto& form = add(FormSelectFlags, OpSelect, "cmovcc r, r"_v);
        form.uses.push(anyReg());
        form.uses.push(anyReg());
        form.uses.push(folded()); // the condition was consumed by the comparison that set the flags
        form.defs.push(tiedDef(0));
        form.flagsEffect = FlagsEffect::Use;

        // `select` yields its first operand when the condition holds and its second otherwise. The
        // tie has already put the first in the destination, so the move that remains is the second
        // one - which is why the condition the encoding carries is the negated one.
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Conditional,
            .opcode = 0x40, .escape = 0x0f,
            .regField = defRef(0), .rmField = useRef(1),
            .negateCondition = true,
        };
    }

    {
        // The condition arrived in a register instead, so it is tested first - and that test writes
        // the flags, which is why this form and the one above disagree about them.
        auto& form = add(FormSelectReg, OpSelect, "test r, r; cmovcc r, r"_v);
        form.uses.push(anyReg());
        form.uses.push(anyReg());
        form.uses.push(anyReg(ClassGpr32));
        form.defs.push(tiedDef(0));
        form.flagsEffect = FlagsEffect::UseDef;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Conditional,
            .opcode = 0x40, .escape = 0x0f,
            .regField = defRef(0), .rmField = useRef(1),
            .prelude = EncodingPrelude::TestLastUse,
            .negateCondition = true,
        };
    }

    /*
     * Floating-point select.
     *
     * There is no CMOVcc for a vector register, so the conditional move is a conditional *branch*
     * over an unconditional one: the tie has already put the first operand in the destination, so
     * what is left is to skip the copy of the second when the condition holds. Two instructions and
     * a forward jump of known length, against the shuffle-and-blend sequence the alternative would
     * be - which would need a mask in a third vector register and SSE4.1 besides.
     *
     * The copy is MOVAPS rather than MOVSS/MOVSD: it moves the whole register, so one form serves
     * both widths, and it needs no prefix to say which.
     */

    auto floatSelect = [&](MachineFormId id, StringView formName, RegisterClassId cls, bool testCondition) {
        auto& form = add(id, OpSelect, formName);
        form.uses.push(anyReg(cls));
        form.uses.push(anyReg(cls));

        // As for the integer select: a condition already in the flags was consumed by the
        // comparison that set them, and one still in a register is tested here.
        if(testCondition) form.uses.push(anyReg(ClassGpr32));
        else form.uses.push(folded());

        form.defs.push(tiedDef(0, cls));
        form.flagsEffect = testCondition ? FlagsEffect::UseDef : FlagsEffect::Use;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::FloatSelect,
            .opcode = 0x28, .escape = 0x0f,
            .regField = defRef(0), .rmField = useRef(1),
            .width = OperationWidth::Fixed32,
            .prelude = testCondition ? EncodingPrelude::TestLastUse : EncodingPrelude::None,
        };
    };

    floatSelect(FormSelectFloat32Flags, "jcc over; movaps xmm, xmm"_v, ClassFloat32, false);
    floatSelect(FormSelectFloat64Flags, "jcc over; movaps xmm, xmm"_v, ClassFloat64, false);
    floatSelect(FormSelectFloat32Reg, "test r, r; jcc over; movaps xmm, xmm"_v, ClassFloat32, true);
    floatSelect(FormSelectFloat64Reg, "test r, r; jcc over; movaps xmm, xmm"_v, ClassFloat64, true);

    /*
     * Stack allocation.
     *
     * A compile-time size becomes a frame object and one `lea`, which leaves the flags alone; a size
     * only known at run time has to round itself up and move the stack pointer, which does not. So
     * the two forms disagree about the flags, and OpAlloca says so.
     */

    {
        auto& form = add(FormAllocaFixed, OpAlloca, "lea r, [frame]"_v);
        form.uses.push(immediate(ImmediateWidth::Imm64));
        form.defs.push(def());
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::AllocaFixed,
        };
    }

    {
        auto& form = add(FormAllocaDynamic, OpAlloca, "sub rsp, r"_v);
        form.uses.push(anyReg());
        form.defs.push(def());
        form.flagsEffect = FlagsEffect::Clobber;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::AllocaDynamic,
        };
    }

    /*
     * Memory access.
     *
     * One form per access width, because the width decides the bytes: a narrow load has to extend
     * into the whole destination register rather than merge with what it held, which is a different
     * opcode rather than a different operand size. A store only writes the bytes it names, so it
     * needs nothing but the right size - and, at one byte, the REX prefix that names spl/bpl/sil/dil
     * rather than ah/ch/dh/bh.
     */

    auto load = [&](MachineFormId id, StringView formName, U8 opcode, U8 escape, OperationWidth width) {
        auto& form = add(id, OpLoad, formName);
        form.uses.push(address());
        form.defs.push(def());
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::LoadStore,
            .opcode = opcode, .escape = escape,
            .regField = defRef(0),
            .width = width,
        };
    };

    // The narrow loads take their operand size from the *result*, since that is the register the
    // extension has to fill; the wider ones are the width they load.
    load(FormLoad8, "movzx r, byte [address]"_v, 0xb6, 0x0f, OperationWidth::FromResult);
    load(FormLoad8S, "movsx r, byte [address]"_v, 0xbe, 0x0f, OperationWidth::FromResult);
    load(FormLoad16, "movzx r, word [address]"_v, 0xb7, 0x0f, OperationWidth::FromResult);
    load(FormLoad16S, "movsx r, word [address]"_v, 0xbf, 0x0f, OperationWidth::FromResult);
    load(FormLoad32, "mov r32, [address]"_v, 0x8b, 0, OperationWidth::Fixed32);
    load(FormLoad32S, "movsxd r64, [address]"_v, 0x63, 0, OperationWidth::Fixed64);
    load(FormLoad64, "mov r64, [address]"_v, 0x8b, 0, OperationWidth::Fixed64);

    // The float loads say their width in the prefix like every other SSE form, so there is one per
    // width rather than one per (width, signedness): a float is never sign- or zero-extended by
    // being loaded, and a narrower one is a different type rather than a narrower access.
    auto floatLoad = [&](MachineFormId id, StringView formName, U8 prefix, RegisterClassId cls) {
        auto& form = add(id, OpLoad, formName);
        form.uses.push(address());
        form.defs.push(def(cls));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::LoadStore,
            .opcode = 0x10, .escape = 0x0f, .prefix = prefix,
            .regField = defRef(0),
            .widthInPrefix = true,
        };
    };

    floatLoad(FormLoadF32, "movss xmm, [address]"_v, 0xf3, ClassFloat32);
    floatLoad(FormLoadF64, "movsd xmm, [address]"_v, 0xf2, ClassFloat64);

    auto store = [&](MachineFormId id, StringView formName, U8 opcode, OperationWidth width) {
        auto& form = add(id, OpStore, formName);
        form.uses.push(address());
        form.uses.push(anyReg());
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::LoadStore,
            .opcode = opcode,
            .regField = useRef(1),
            .width = width,
        };

        return &form;
    };

    store(FormStore8, "mov byte [address], r"_v, 0x88, OperationWidth::Fixed32)->encoding.byteRegField = true;
    store(FormStore16, "mov word [address], r"_v, 0x89, OperationWidth::Fixed32)->encoding.prefix = 0x66;
    store(FormStore32, "mov dword [address], r"_v, 0x89, OperationWidth::Fixed32);
    store(FormStore64, "mov qword [address], r"_v, 0x89, OperationWidth::Fixed64);

    /*
     * The same, with the value in the encoding rather than in a register.
     *
     * `mov [address], imm` has no second register at all, so ModRM.reg carries the opcode extension
     * /0 instead - which is the case `emitLoadStore` was already written for. What this removes is
     * not only the `mov $imm, r` above the store: it removes the operand, so the constant never
     * enters allocation and never competes for a register. `each` in test/bench/programs/Pipeline.yana
     * is the shape that makes the difference visible - two callee-saved registers were being pushed
     * and popped there to hold the constants 1 and 2 across nothing at all, because a store demanded
     * a register for them and every other register was taken.
     *
     * All four declare **Imm32** rather than a width of their own, and that is what makes the
     * selection below total. `canEmbedImm` in transform.cpp decides whether to embed a constant
     * before any form has been chosen, and it asks by opcode - so it answers for the widest form the
     * opcode has. A narrow form that declared a narrower immediate would be refusing values that
     * question had already accepted, and an operand that has been taken out of allocation has no
     * register left to fall back to. Truncating is not a compromise here: the store discards the
     * upper bytes whatever carries them, so writing the low ones is what the register form does too.
     * The 64-bit form is the only one the width genuinely constrains - its immediate is sign-extended
     * rather than truncated - and Imm32 is exactly that constraint.
     */
    auto storeImm = [&](MachineFormId id, StringView formName, U8 opcode, U8 immediateBytes,
                        U8 prefix, OperationWidth width)
    {
        auto& form = add(id, OpStore, formName);
        form.uses.push(address());
        form.uses.push(immediate(ImmediateWidth::Imm32));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::LoadStore,
            .opcode = opcode,
            .prefix = prefix,
            .extension = 0,
            .immField = useRef(1),
            .width = width,
            .immediateBytes = immediateBytes,
        };
    };

    storeImm(FormStore8Imm, "mov byte [address], imm8"_v, 0xc6, 1, 0, OperationWidth::Fixed32);
    storeImm(FormStore16Imm, "mov word [address], imm16"_v, 0xc7, 2, 0x66, OperationWidth::Fixed32);
    storeImm(FormStore32Imm, "mov dword [address], imm32"_v, 0xc7, 4, 0, OperationWidth::Fixed32);
    storeImm(FormStore64Imm, "mov qword [address], imm32"_v, 0xc7, 4, 0, OperationWidth::Fixed64);

    // A store has no result to take its width from, so it states it - and states it as the width of
    // the value rather than of the address, which is what the prefix already says in bytes.
    auto floatStore = [&](MachineFormId id, StringView formName, U8 prefix, RegisterClassId cls,
                          OperationWidth width)
    {
        auto& form = add(id, OpStore, formName);
        form.uses.push(address());
        form.uses.push(anyReg(cls));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::LoadStore,
            .opcode = 0x11, .escape = 0x0f, .prefix = prefix,
            .regField = useRef(1),
            .width = width,
            .widthInPrefix = true,
        };
    };

    floatStore(FormStoreF32, "movss [address], xmm"_v, 0xf3, ClassFloat32, OperationWidth::Fixed32);
    floatStore(FormStoreF64, "movsd [address], xmm"_v, 0xf2, ClassFloat64, OperationWidth::Fixed64);

    // The VEX transfers. Two operands rather than three: a `vmovss` that reads memory has no merge
    // source - it zeroes the rest of the register, which is what a load of a scalar wants - and a
    // store has no destination register to name.
    vexTwin(FormLoadF32Vex, FormLoadF32, "vmovss xmm, [address]"_v, false);
    vexTwin(FormLoadF64Vex, FormLoadF64, "vmovsd xmm, [address]"_v, false);
    vexTwin(FormStoreF32Vex, FormStoreF32, "vmovss [address], xmm"_v, false);
    vexTwin(FormStoreF64Vex, FormStoreF64, "vmovsd [address], xmm"_v, false);

    /*
     * Block operations.
     *
     * Two encodings with very different register requirements: `rep movsb`/`rep stosb` demand fixed
     * registers and consume them as they run, while the unrolled form works out of whatever
     * registers the operands already occupy. Which one applies is chosen once by the transform
     * pipeline and recorded on the instruction.
     */

    {
        auto& form = add(FormBlockCopyRep, OpBlockCopy, "rep movsb"_v);
        form.uses.push(fixedReg(IntRegister::rdi));
        form.uses.push(fixedReg(IntRegister::rsi));
        form.uses.push(fixedReg(IntRegister::rcx));

        // Consumed as it runs: rdi and rsi are left pointing past the copied region and rcx is
        // counted down to zero, so a value still live afterwards has to be copied somewhere safe
        // first instead of being read back advanced.
        form.clobbers.add(gpr(IntRegister::rdi));
        form.clobbers.add(gpr(IntRegister::rsi));
        form.clobbers.add(gpr(IntRegister::rcx));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::BlockCopyRep,
        };
    }

    /*
     * The unrolled form needs one general register to carry each word through, and states it as a
     * clobber of a fixed register rather than as a declared temporary (MachineForm::temporaries).
     * The two would reserve it at different scopes: a clobber keeps a live value out of r11 at this
     * one instruction, where a declared temporary is held back from the whole function.
     *
     * It comes in two, and the pair is what the count operand costs. The unrolling reads the byte
     * count out of the IR and writes that many `mov`s: the operand appears in none of them, so it
     * needs no location and the ordinary form says so with `folded()`. But being folded is a
     * property of the *value* - `Implicit` is set on the constant, not on this use of it - so a
     * count that some other instruction still needs in a register cannot be folded here either. The
     * second form is that case, and it differs in the one operand.
     *
     * Two forms rather than a fallback to `rep movsb`, which is the other way to be correct: a rep
     * copy of twelve bytes is thirty cycles of startup to avoid materializing a constant.
     */
    auto blockCopyUnrolled = [&](MachineFormId id, bool countInRegister) {
        auto& form = add(id, OpBlockCopy, "mov (unrolled)"_v);
        form.uses.push(anyReg());
        form.uses.push(anyReg());
        form.uses.push(countInRegister ? anyReg() : folded());
        form.clobbers.add(gpr(IntRegister::r11));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::BlockCopyUnrolled,
        };
    };

    blockCopyUnrolled(FormBlockCopyUnrolled, false);
    blockCopyUnrolled(FormBlockCopyUnrolledCount, true);

    {
        // Positional, matching the instruction's own operand order (to, count, pattern) rather than
        // the rdi/rax/rcx order `rep stosb` reads them in.
        auto& form = add(FormBlockSetRep, OpBlockSet, "rep stosb"_v);
        form.uses.push(fixedReg(IntRegister::rdi));
        form.uses.push(fixedReg(IntRegister::rcx));
        form.uses.push(fixedReg(IntRegister::rax));

        // rdi is advanced past the filled region and rcx counted down; rax is only read.
        form.clobbers.add(gpr(IntRegister::rdi));
        form.clobbers.add(gpr(IntRegister::rcx));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::BlockSetRep,
        };
    }

    // The same pair, for the same reason - see the copy above. The pattern stays in a register in
    // both: it is what every store the unrolling writes reads from.
    auto blockSetUnrolled = [&](MachineFormId id, bool countInRegister) {
        auto& form = add(id, OpBlockSet, "mov (unrolled)"_v);
        form.uses.push(anyReg());
        form.uses.push(countInRegister ? anyReg() : folded());
        form.uses.push(anyReg());
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::BlockSetUnrolled,
        };
    };

    blockSetUnrolled(FormBlockSetUnrolled, false);
    blockSetUnrolled(FormBlockSetUnrolledCount, true);

    /*
     * Calls.
     *
     * Operand and result locations come from the selected calling convention rather than from a
     * table here: where an argument goes depends on how many of each bank came before it, which a
     * flat list cannot say. The clobber set comes from the same place.
     */

    auto call = [&](MachineFormId id, StringView formName, PseudoKind pseudo) {
        auto& form = add(id, OpCall, formName);
        form.conventionOperands = true;
        form.flagsEffect = FlagsEffect::Clobber;
        form.encoding = EncodingDescriptor { .family = EncodingFamily::Pseudo, .pseudo = pseudo };
    };

    call(FormCallDirect, "call rel32"_v, PseudoKind::CallDirect);
    call(FormCallIndirect, "call r/m"_v, PseudoKind::CallIndirect);
    call(FormSyscall, "syscall"_v, PseudoKind::Syscall);

    // The argument area is addressed through rsp, at the offset the convention assigned - an address
    // the legalizer resolves like any other, so this is an ordinary store.
    {
        auto& form = add(FormPushArgReg, OpPushArg, "mov [rsp + n], r"_v);
        form.uses.push(anyReg());
        form.defs.push(noDef()); // stands in for the argument in the call's operand list
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::LoadStore,
            .opcode = 0x89,
            .regField = useRef(0),
            .width = OperationWidth::Fixed64,
        };
    }

    // A float argument is stored by the instruction that owns its bank, at its own width: the slot
    // is eight bytes wide whatever goes in it, and the callee reads back exactly the four or eight
    // the convention put there.
    auto floatPushArg = [&](MachineFormId id, StringView formName, U8 prefix, RegisterClassId cls,
                            OperationWidth width)
    {
        auto& form = add(id, OpPushArg, formName);
        form.uses.push(anyReg(cls));
        form.defs.push(noDef());
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::LoadStore,
            .opcode = 0x11, .escape = 0x0f, .prefix = prefix,
            .regField = useRef(0),
            .width = width,
            .widthInPrefix = true,
        };
    };

    floatPushArg(FormPushArgF32, "movss [rsp + n], xmm"_v, 0xf3, ClassFloat32, OperationWidth::Fixed32);
    floatPushArg(FormPushArgF64, "movsd [rsp + n], xmm"_v, 0xf2, ClassFloat64, OperationWidth::Fixed64);

    {
        // MOV r/m64, imm32 sign-extends, which is what a narrower constant occupying a full 8-byte
        // argument slot wants anyway.
        auto& form = add(FormPushArgImm, OpPushArg, "mov [rsp + n], imm"_v);
        form.uses.push(immediate(ImmediateWidth::Imm32));
        form.defs.push(noDef());
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::LoadStore,
            .opcode = 0xc7, .extension = 0,
            .immField = useRef(0),
            .width = OperationWidth::Fixed64,
        };
    }

    /*
     * The remaining target operations.
     */

    {
        auto& form = add(FormLea, OpLea, "lea r, [address]"_v);
        form.defs.push(def());
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Lea,
            .opcode = 0x8d,
            .regField = defRef(0),
            .width = OperationWidth::Fixed64,
        };
    }

    /*
     * Terminators.
     *
     * All four are pseudos: which bytes a branch takes depends on which of its successors the block
     * order put next, and a return has to emit the epilogue the frame layout decided on.
     */

    add(FormJmp, OpJmp, "jmp rel32"_v).encoding = EncodingDescriptor {
        .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::Jump,
    };

    {
        auto& form = add(FormJccFlags, OpJcc, "jcc rel32"_v);
        form.uses.push(folded()); // the condition was consumed by the comparison that set the flags
        form.flagsEffect = FlagsEffect::Use;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::Branch,
        };
    }

    {
        // The same branch, where the comparison that set the flags was materialized as well and so
        // still holds a register - see §3.5.2.1 of the README. The condition is a real operand rather
        // than a folded one, because the value is genuinely live; it is simply not what the branch
        // reads. Two bytes cheaper than the form below, which re-derives the flags from it.
        auto& form = add(FormJccLive, OpJcc, "jcc rel32 (condition live)"_v);
        form.uses.push(anyReg(ClassGpr32));
        form.flagsEffect = FlagsEffect::Use;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::Branch,
        };
    }

    {
        auto& form = add(FormJccReg, OpJcc, "test r, r; jcc rel32"_v);
        form.uses.push(anyReg(ClassGpr32));
        form.flagsEffect = FlagsEffect::UseDef;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::Branch,
            .prelude = EncodingPrelude::TestLastUse,
        };
    }

    {
        // A return's operands are the function's results, placed by the result half of its own
        // convention. Nothing is live once the function has returned, so it clobbers nothing.
        auto& form = add(FormRet, OpRet, "ret"_v);
        form.conventionOperands = true;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::Return,
        };
    }

    {
        /*
         * The one form in this table that emits no bytes, and the only one entitled to.
         *
         * Every other zero-byte case in the backend is an instruction that *became* nothing - a cast
         * whose extension was already done, a copy into the register the value was already in - and
         * each of those is a form that would have emitted something had the fold not applied. This
         * one has nothing to emit in the first place: the block it ends is one nothing arrives at
         * the end of, so there is no epilogue to run and no address to return to. It carries no
         * operands and no successors, which is why it needs neither convention nor clobbers.
         */
        auto& form = add(FormNoReturn, OpNoReturn, "noreturn"_v);
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::NoReturn,
        };
    }

    assertTrue(forms.size() == kMachineFormCount);

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

    // The intrinsics' forms go into the same table, after the described ones, so that everything
    // downstream asks an intrinsic the same questions it asks an `add` - see intrinsic.cpp.
    addIntrinsics(*this);

    assertTrue(validateMachineForms(*this));
    assertTrue(validateIntrinsics(*this));
}

const MachineTarget& machineTarget() {
    static MachineTarget target;
    return target;
}

/*
 * Validation.
 */

/*
 * How deep into one bank's operand-temporary pool a form can reach - see target.h's block comment
 * and `takeTemp` in legalize.cpp, which is what does the reaching.
 *
 * A temporary is handed out per *position*, and every position the walk has to step over is consumed
 * rather than stepped over - it has to be, since the pool is a contiguous block off the top of the
 * register file and the reserve has to hold back the ones that were skipped as well. So the depth a
 * form reaches is not the number of temporaries it wants, and three things feed it:
 *
 *  - the temporaries themselves. A result whose home is a frame slot is computed in one and stored
 *    afterwards; an operand that is neither in a fixed register nor folded into the encoding is read
 *    out of one when its home is a slot. The operand a tied result overwrites is not among them - it
 *    is brought into the result's own temporary, which is what the tie means.
 *
 *  - the registers the form's own expansion writes, which it declares as clobbers. A clobber keeps a
 *    live *web* out of a register, which is what it is for; what it cannot do is keep the pool out,
 *    because the pool is handed out here rather than by placement - and a form reaching for a
 *    register nothing else wants reaches for the top of the file, which is where the pool is.
 *
 *  - the two registers a folded address is holding. An `x86_addr` produces no register of its own
 *    and is resolved one instruction in front of the access that reads it, so a base and an index
 *    that came out of the frame are sitting in two of that access's own scratch registers. Two, and
 *    general ones: an address has a base and an index and nothing else, and neither is a vector.
 *
 * Asked of every form here rather than left to the assertion in `takeTemp`, because that assertion
 * only fires for a program that actually reaches the depth: the lane-wise select was three
 * temporaries and a clobber from the day it was written, and what found it was one fixture, months
 * later, that happened to spill both of its arms.
 */
static Size operandTempReach(const MachineForm& form, RegisterBankId bank) {
    auto& registers = targetRegisters();
    auto bankOf = [&](const MachineOperandConstraint& c) { return registers.regClass(c.regClass).bank; };

    // A form that takes its operands from a convention rather than from its own arrays states none
    // of them here, and needs no temporary either: every one of them is a fixed register.
    if(form.conventionOperands) return 0;

    Size reach = 0;

    for(auto& result: form.defs) {
        if(bankOf(result) != bank) continue;
        if(result.kind == OperandConstraintKind::Register || result.kind == OperandConstraintKind::ReuseOperand) {
            reach++;
        }
    }

    auto tied = form.tiedResult();

    for(Size i = 0; i < form.uses.size(); i++) {
        auto& use = form.uses[i];
        if(bankOf(use) != bank || I32(i) == tied) continue;

        switch(use.kind) {
            // An address operand is counted below rather than here, and once rather than twice: a
            // folded `x86_addr` holds two registers and needs no temporary of its own, a pointer
            // the allocator left in a frame slot needs one and holds none, and no operand is both.
            case OperandConstraintKind::Address:
                break;

            case OperandConstraintKind::Register:
            case OperandConstraintKind::RegisterSubset:
            case OperandConstraintKind::RegisterOrMemory:
                reach++;
                break;
            default:
                break;
        }
    }

    // The pool is counted from the top of the register file, so a clobber either falls inside it or
    // does not - and one that does costs a position whether or not the temporary below it is wanted.
    auto pool = TemporaryReserve::widest();
    auto available = Size(registers.bank(bank).physicalCount);

    for(Size i = 0; i < kMaxOperandTemps && i < available; i++) {
        if(form.clobbers.has(pool.operandTemp(bank, i))) reach++;
    }

    if(bank == BankGpr && form.addressOperand() >= 0) reach += 2;

    return reach;
}

bool validateMachineForms(const MachineTarget& target) {
    auto& registers = targetRegisters();
    auto ok = true;

    auto fail = [&](const MachineForm& form, StringView what) {
        ok = false;
        logError("machine form \"%@\": %@", form.name, what);
    };

    // Every opcode has at least one form, and every form belongs to an opcode that exists.
    bool hasForm[kMachineOpcodeCount] = {};

    for(auto& form: target.forms) {
        if(form.opcode >= kMachineOpcodeCount) {
            fail(form, "names an opcode that does not exist"_v);
            continue;
        }

        hasForm[form.opcode] = true;

        auto checkOperand = [&](const MachineOperandConstraint& c, bool isDef) {
            if(c.regClass >= kRegisterClassCount) {
                fail(form, "names a register class that does not exist"_v);
                return;
            }

            auto& cls = registers.regClass(c.regClass);

            // A fixed register has to be one a value of that operand's class could have been given
            // in the first place: a form demanding rsp, or a general register for a vector operand,
            // is describing a machine that does not exist.
            if(c.kind == OperandConstraintKind::FixedRegister) {
                if(c.fixedReg.bank != cls.bank || !cls.allowedPhysical.has(c.fixedReg)) {
                    fail(form, "fixes an operand to a register its class cannot occupy"_v);
                }
            }

            // Every register the subset names has to be one the class already allows: the subset
            // narrows a class rather than reaching outside it.
            if(c.kind == OperandConstraintKind::RegisterSubset) {
                if(!((c.allowed & cls.allowedPhysical) == c.allowed)) {
                    fail(form, "allows an operand a register outside its class"_v);
                }
            }

            // A tie joins a def to a use that exists, and only a def may be tied.
            if(c.tiedOperand != kNoTiedOperand) {
                if(!isDef) fail(form, "ties a use to another operand"_v);
                if(c.tiedOperand >= form.uses.size()) fail(form, "ties a result to an operand that does not exist"_v);
            }

            if(c.kind == OperandConstraintKind::ReuseOperand && c.tiedOperand == kNoTiedOperand) {
                fail(form, "reuses an operand without saying which"_v);
            }

            if((c.kind == OperandConstraintKind::Immediate) != (c.immediate != ImmediateWidth::None)) {
                fail(form, "states an immediate width for an operand that is not one, or the reverse"_v);
            }

            auto isMemory = c.kind == OperandConstraintKind::RegisterOrMemory
                || c.kind == OperandConstraintKind::Memory;

            if(isMemory != (c.memoryAccess != MemoryAccessKind::None)) {
                fail(form, "states a memory access for an operand that has none, or the reverse"_v);
            }

            /*
             * Descriptor fields no generic pass implements yet.
             *
             * Each of these is part of the representation because the first form that needs it must
             * be able to say so rather than being handled by a special case. But a field placement
             * and legalization do not read is worse than one that does not exist: a form using it
             * looks complete while half of what it declares is silently ignored. So a use of one is
             * rejected here until the pass that would honour it exists, which turns "adding this
             * instruction needs allocator work" into a build failure rather than into wrong code.
             */

            // Placement is first-fit over a class's registers and does not narrow by `allowed`.
            if(c.kind == OperandConstraintKind::RegisterSubset) {
                fail(form, "restricts an operand to a register subset, which placement does not implement"_v);
            }

            // Legalization can leave an operand in a frame slot, but has no rule for one that may
            // *only* be in memory - there is nothing to reload it into and nothing to spill.
            if(c.kind == OperandConstraintKind::Memory) {
                fail(form, "requires an operand in memory, which legalization does not implement"_v);
            }

            // A write-only memory operand would be a result written to a slot the instruction never
            // read, which nothing produces and legalization's in-place rule does not cover.
            if(c.memoryAccess == MemoryAccessKind::Write) {
                fail(form, "writes an operand in memory without reading it, which legalization does not implement"_v);
            }

            // Every form described so far reads all of its operands before writing any result, and
            // placement's rule for a tied result assumes exactly that.
            auto defaultTiming = isDef ? OperandTiming::LateDef : OperandTiming::EarlyUse;
            if(c.timing != defaultTiming) {
                fail(form, "states an operand timing that placement does not implement"_v);
            }
        };

        for(auto& c: form.uses) checkOperand(c, false);
        for(auto& c: form.defs) checkOperand(c, true);

        // At most one operand may be taken from memory at any one instruction, and each of the two
        // roles is named at most once: a general memory operand occupies the r/m field, and there is
        // one of those.
        Size reads = 0, readWrites = 0;
        for(auto& c: form.uses) {
            if(c.kind != OperandConstraintKind::RegisterOrMemory) continue;
            if(c.memoryAccess == MemoryAccessKind::Read) reads++;
            if(c.memoryAccess == MemoryAccessKind::ReadWrite) readWrites++;
        }

        if(reads > 1 || readWrites > 1) fail(form, "names more than one operand for the single r/m field"_v);

        // A form that can write its r/m operand in place has to say which result goes there, since
        // the operand and the result then have to occupy one slot.
        if(readWrites > 0 && form.tiedResult() != form.memoryDef()) {
            fail(form, "writes an operand in place without tying its result to it"_v);
        }

        // And it fits in the scratch pool - see operandTempReach above. The pool is one number for
        // the whole backend, so this is where a form that would outgrow it has to be caught: the
        // alternative is an assertion in the legalizer on whichever program first spills at one.
        for(Size bank = 0; bank < kRegisterBankCount; bank++) {
            if(operandTempReach(form, RegisterBankId(bank)) > kMaxOperandTemps) {
                fail(form, "reaches deeper into the scratch pool than kMaxOperandTemps holds back"_v);
            }
        }

        /*
         * The encoding descriptor.
         */

        auto& encoding = form.encoding;

        // Every field the encoding names has to be an operand the instruction actually has, since
        // emission indexes the resolved operands by these without looking at anything else.
        auto checkField = [&](OperandRef ref, StringView what) {
            if(ref.isNone()) return;

            auto& list = ref.result ? form.defs : form.uses;
            if(Size(ref.index) >= list.size()) fail(form, what);
        };

        checkField(encoding.regField, "names a ModRM.reg field that is not an operand of it"_v);
        checkField(encoding.rmField, "names an r/m field that is not an operand of it"_v);
        checkField(encoding.immField, "names an immediate field that is not an operand of it"_v);
        checkField(encoding.vvvvField, "names a second source that is not an operand of it"_v);

        /*
         * The vector prefixes.
         *
         * Two rules, and both are about a field that would otherwise be written and ignored. Only a
         * VEX or EVEX prefix has anywhere to put a second source register or a vector length, so a
         * legacy encoding naming either is a form whose operands are not what its bytes say - the
         * mistake being fenced off is deriving a VEX form and forgetting to set the encoding.
         */
        if(encoding.prefixEncoding == PrefixEncoding::Legacy) {
            if(!encoding.vvvvField.isNone()) {
                fail(form, "names a second source register, which only a VEX or EVEX prefix can carry"_v);
            }

            if(encoding.vectorLength != 0) {
                fail(form, "states a vector length, which only a VEX or EVEX prefix can carry"_v);
            }
        } else {
            // 512-bit is EVEX's alone: VEX has one length bit and cannot say it.
            if(encoding.prefixEncoding == PrefixEncoding::Vex && encoding.vectorLength > 1) {
                fail(form, "asks for a 512-bit operation under a VEX prefix, which has no length for it"_v);
            }

            if(encoding.opcodeMap < kOpcodeMap0F || encoding.opcodeMap > kOpcodeMap0F3A) {
                fail(form, "names an opcode map no vector prefix can encode"_v);
            }

            // A form that can only be written with a prefix the target may not have is a form that
            // has to say so, or selectForm would pick it on a machine that cannot execute it.
            if((form.requiredFeatures & (kFeatureAvx | kFeatureAvx512f)) == 0) {
                fail(form, "is written with a vector prefix without requiring the extension that defines it"_v);
            }
        }

        /*
         * And the rule from the other side: **a legacy encoding that touches a vector register has
         * to have a vector-prefixed alternative for a target that can encode one.**
         *
         * Without it selection has nothing else to pick, so a build with AVX emits a legacy SSE
         * instruction - which leaves the upper half of the register it writes untouched, and is what
         * a dirty upper half costs a processor for. The whole tier exists to make that impossible,
         * and this is the assertion that it reached every form rather than most of them: a packed
         * operation added to the table above gets its twin from the sweep in the constructor, and
         * one added somewhere the sweep does not look fails here instead of costing silently.
         *
         * A pseudo is exempt and is the one exemption. Its bytes are its own emitter's rather than
         * this descriptor's, and the emitters ask `packedNeedsVex` directly - there is nothing for a
         * second form to describe, the operand constraints being identical either way.
         */
        auto touchesVectorRegister = [&](const MachineForm& f) {
            auto vectorClass = [](const MachineOperandConstraint& c) {
                switch(c.kind) {
                    case OperandConstraintKind::Register:
                    case OperandConstraintKind::FixedRegister:
                    case OperandConstraintKind::RegisterSubset:
                    case OperandConstraintKind::RegisterOrMemory:
                        return c.regClass == ClassFloat32 || c.regClass == ClassFloat64
                            || c.regClass == ClassXmm128;
                    default:
                        return false;
                }
            };

            for(auto& c: f.uses) if(vectorClass(c)) return true;
            for(auto& c: f.defs) if(vectorClass(c)) return true;
            return false;
        };

        if(encoding.prefixEncoding == PrefixEncoding::Legacy && form.alternative == 0
            && encoding.family != EncodingFamily::None && encoding.family != EncodingFamily::Pseudo
            && touchesVectorRegister(form))
        {
            fail(form, "is a legacy vector encoding with no prefixed alternative to select instead"_v);
        }

        // An alternative and the form it replaces are one operation, so what the rest of the backend
        // reads off a form before selection has settled - what it does to the flags, what it costs -
        // has to be the same on both. The operands deliberately are not: dropping the tie is the
        // point of the VEX arithmetic forms.
        if(form.alternativeOf != 0) {
            auto& original = target.forms[form.alternativeOf];

            if(original.opcode != form.opcode) fail(form, "is an alternative to a form of another opcode"_v);
            if(original.flagsEffect != form.flagsEffect) fail(form, "disagrees with the form it replaces about the flags"_v);
            if(original.uses.size() != form.uses.size()) fail(form, "takes a different number of operands than the form it replaces"_v);
        }

        // An immediate field has to name an operand the form declared as one, or - for a constant
        // materialization, whose immediate is the value it defines rather than an operand - a
        // result. Otherwise the encoding would be writing bytes for something with no value.
        if(!encoding.immField.isNone() && !encoding.immField.result) {
            auto& constraint = form.uses[encoding.immField.index];
            if(constraint.kind != OperandConstraintKind::Immediate) {
                fail(form, "encodes an immediate from an operand that is not one"_v);
            }
        }

        /*
         * The trailing byte, of which there is at most one.
         *
         * A packed comparison's predicate and a shuffle's lane pattern are both an `ib` no operand
         * supplies, written after the whole encoding by the same two lines - so a form declaring
         * both would write two bytes where the machine reads one, and the second would be decoded as
         * the next instruction. Neither may stand with an immediate *operand* either: that is a
         * third writer of the same position.
         */
        if(encoding.conditionImmediate && encoding.patternImmediate) {
            fail(form, "ends in both a condition byte and a pattern byte, and an encoding has one trailing byte"_v);
        }

        if((encoding.conditionImmediate || encoding.patternImmediate) && !encoding.immField.isNone()) {
            fail(form, "ends in a trailing byte as well as encoding an immediate operand"_v);
        }

        // The r/m field is the one that may hold a memory operand, so a form with a memory
        // alternative has to encode that operand there and nowhere else.
        auto memoryOperand = form.memoryUse() != -1 ? form.memoryUse() : form.memoryDef();
        if(memoryOperand != -1 && encoding.family != EncodingFamily::Pseudo) {
            auto& rm = encoding.rmField;
            auto& reg = encoding.regField;

            auto encodable = (!rm.isNone() && !rm.result && rm.index == memoryOperand)
                || (encoding.opcodeAlt != 0 && !reg.isNone() && !reg.result && reg.index == memoryOperand);

            if(!encodable) fail(form, "allows an operand in memory that its encoding cannot address"_v);
        }

        // A width taken from an operand needs that operand to exist, whatever the encoding does with
        // it: the memory-operand rules ask the same question to decide whether a slot fits.
        auto width = encoding.width;
        if((width == OperationWidth::FromUse0 || width == OperationWidth::Narrowest) && form.uses.isEmpty()) {
            fail(form, "takes its width from an operand that does not exist"_v);
        }

        if(width == OperationWidth::FromUse1 && form.uses.size() < 2) {
            fail(form, "takes its width from a second operand that does not exist"_v);
        }

        if(width == OperationWidth::FromResult && form.defs.isEmpty() && !form.conventionOperands
            && encoding.family != EncodingFamily::None && encoding.family != EncodingFamily::Pseudo)
        {
            fail(form, "takes its width from a result that does not exist"_v);
        }

        if((encoding.family == EncodingFamily::Pseudo) != (encoding.pseudo != PseudoKind::None)) {
            fail(form, "names a dedicated encoder without being a pseudo, or the reverse"_v);
        }

        // A form with convention-derived operands states no operand constraints of its own: the two
        // would be two answers to one question.
        if(form.conventionOperands && (form.uses.isNotEmpty() || form.defs.isNotEmpty())) {
            fail(form, "states operand constraints as well as taking them from a convention"_v);
        }

        // Implicit effects and clobbers are physical registers by construction, but a clobber that
        // named a reserved register would be an instruction the allocator cannot work around.
        RegSet reserved;
        for(auto& bank: registers.banks) reserved |= bank.reserved;
        if(!(form.clobbers & reserved).isEmpty()) fail(form, "clobbers a reserved register"_v);

        // The other two halves of the same fence as the operand fields above. A register an
        // instruction destroys is expressible today as a clobber, which every pass honours; one it
        // merely *reads* without naming, and one it defines without naming, are read by nothing -
        // so a form stating either would have that effect ignored rather than respected.
        if(!form.implicitUses.isEmpty()) {
            fail(form, "reads a register it does not name, which no pass implements"_v);
        }

        if(!form.implicitDefs.isEmpty()) {
            fail(form, "defines a register it does not name - state it as a clobber instead"_v);
        }

        // And the third: the temporary reserve derives its two pools from what legalization asks for
        // (see TemporaryReserve), and has no pool for a register a form's own *expansion* needs. The
        // one expansion that needs a scratch register today names a fixed one as a clobber instead,
        // which reserves it at the instruction rather than for the whole function.
        for(auto count: form.temporaries.counts) {
            if(count != 0) fail(form, "declares expansion temporaries, which the reserve does not implement"_v);
        }
    }

    for(Size i = 1; i < kMachineOpcodeCount; i++) {
        if(!hasForm[i]) {
            ok = false;
            logError("machine opcode \"%@\" has no form", target.opcodes[i].name);
        }
    }

    // Unless an opcode says its forms differ, they have to agree about the flags. The compare
    // folding in transform.cpp asks what an instruction does to the flags while the peephole passes
    // are still deciding which form it will take, and that question only has one answer if every
    // form of the opcode gives the same one.
    for(Size op = 1; op < kMachineOpcodeCount; op++) {
        if(target.opcodes[op].flagsSelective) continue;

        Maybe<bool> writes;
        for(auto& form: target.forms) {
            if(form.opcode != op) continue;

            auto formWrites = writesFlags(form.flagsEffect);
            if(writes.isNothing()) writes = Just(formWrites);
            else if(writes.unwrap() != formWrites) {
                ok = false;
                logError("machine opcode \"%@\" has forms that disagree about the flags",
                    target.opcodes[op].name);
                break;
            }
        }
    }

    // And the same for the address operand, for the same reason: the address folding runs before
    // the form is settled and asks the *opcode* which operand is an address (opcodeAddressOperand),
    // so a load whose narrow form named its address somewhere else would have the fold rewrite one
    // operand and the encoder read another.
    //
    // A memory-source twin is the one exception, and is excluded here rather than allowed to weaken
    // the rule: it exists precisely to name an address where its source names a register, it is
    // reached only by an instruction a load fold has already rewritten, and opcodeAddressOperand
    // skips it for the same reason. What is checked instead is that it names the operand its source
    // could have read from memory and no other.
    for(Size op = 1; op < kMachineOpcodeCount; op++) {
        Maybe<I32> address;
        for(auto& form: target.forms) {
            if(form.opcode != op) continue;

            if(form.memorySourceOf) {
                if(form.addressOperand() != target.forms[form.memorySourceOf].memoryUse()) {
                    fail(form, "addresses an operand its register form does not read from memory"_v);
                }

                continue;
            }

            auto formAddress = form.addressOperand();
            if(address.isNothing()) address = Just(formAddress);
            else if(address.unwrap() != formAddress) {
                ok = false;
                logError("machine opcode \"%@\" has forms that disagree about which operand is an address",
                    target.opcodes[op].name);
                break;
            }
        }
    }

    // Both directions of the twinning agree, so that nothing can hold a form id that answers only
    // one of the two questions.
    for(auto& form: target.forms) {
        if(form.memorySource && target.forms[form.memorySource].memorySourceOf != form.id) {
            fail(form, "names a memory source that does not name it back"_v);
        }

        if(form.memorySourceOf && target.forms[form.memorySourceOf].memorySource != form.id) {
            fail(form, "is a memory source its register form does not name"_v);
        }
    }

    return ok;
}

/*
 * Selection.
 */

MachineOpcodeId opcodeFor(LowerBase base, LowerInst* inst) {
    // Whether this instruction's operands live in the vector bank. Read from the operands rather
    // than from the result, because the two disagree for exactly the operation whose opcode this
    // most needs to decide: a comparison of two floats produces an integer.
    auto isFloatOp = [&] {
        if(inst->createdCount > 0 && isFloat(inst->created()[0].type)) return true;
        return inst->usedCount > 0 && isFloat(base[inst->used()[0]]->type);
    };

    // And whether they live in a vector register as a *packed* value, which is the third answer the
    // two above cannot give: a vector add and a float add read the same bank and are not the same
    // machine operation. Read from the operands for the same reason - a comparison of two vectors
    // produces a mask, which is a different type from either of them.
    auto isPackedOp = [&] {
        if(inst->createdCount > 0 && isVectorLike(inst->created()[0].type)) return true;
        return inst->usedCount > 0 && isVectorLike(base[inst->used()[0]]->type);
    };

    switch(inst->kind) {
        case LowerInst::Arg:        return OpArg;
        case LowerInst::Global:     return OpGlobalAddress;
        case LowerInst::Fun:        return OpFunctionAddress;
        case LowerInst::Imm:        return OpImm;
        case LowerInst::Nop:        return OpNop;
        case LowerInst::Set:        return OpMove;
        case LowerInst::Cast:       return OpCast;
        case LowerInst::Bitcast:    return OpBitcast;
        /*
         * The signed multiply, which is the *usual* kind over a vector and not the exceptional one.
         *
         * `signedOperand` answers a vector's lane's signedness, so an ordinary `Vec(Int)` product
         * arrives here as an `IMul` and `Mul` is what nobody writes - which is the same trap §5.6
         * records `selectPackedForm` falling into. This line answered `OpIMul` unconditionally, and
         * the form selected for it is `OpVMul`'s: the two disagreed, and `verifySelection` says so.
         *
         * It was unreachable until the 32-bit lane got a form. The only packed integer multiply
         * before that was `pmullw` at a 16-bit lane, which no fixture in this tree writes.
         */
        case LowerInst::IMul:       return isPackedOp() ? OpVMul : OpIMul;
        case LowerInst::IDiv:       return OpIDiv;
        case LowerInst::Rem:        return OpRem;
        case LowerInst::IRem:       return OpIRem;
        case LowerInst::MulHi:      return OpMulHi;
        case LowerInst::IMulHi:     return OpIMulHi;

        // The six the IR states once and the machine has twice or three times, one operation per
        // bank and per packing. A packed operation is asked about first because a vector of floats
        // answers yes to neither of the other two: `isFloat` is a scalar predicate by construction.
        case LowerInst::Shl:        return isPackedOp() ? OpVShl : OpShl;
        case LowerInst::Shr:        return isPackedOp() ? OpVShr : OpShr;
        case LowerInst::Sar:        return isPackedOp() ? OpVSar : OpSar;
        case LowerInst::And:        return isPackedOp() ? OpVAnd : OpAnd;
        case LowerInst::Or:         return isPackedOp() ? OpVOr : OpOr;
        case LowerInst::Xor:        return isPackedOp() ? OpVXor : OpXor;
        case LowerInst::Add:        return isPackedOp() ? OpVAdd : isFloatOp() ? OpFAdd : OpAdd;
        case LowerInst::Sub:        return isPackedOp() ? OpVSub : isFloatOp() ? OpFSub : OpSub;
        case LowerInst::Mul:        return isPackedOp() ? OpVMul : isFloatOp() ? OpFMul : OpMul;
        case LowerInst::Div:        return isPackedOp() ? OpVDiv : isFloatOp() ? OpFDiv : OpDiv;
        case LowerInst::Cmp:        return isPackedOp() ? OpVCmp : isFloatOp() ? OpFCmp : OpCmp;

        // Both are a constant this machine has no packed immediate for, and both can build the
        // constant they need out of a scratch register - all ones is a register compared with
        // itself, and zero is one exclusive-ored with itself. So neither is a refusal.
        case LowerInst::Neg:
            return isPackedOp() ? OpVNeg : isFloatOp() ? OpFNeg : OpNeg;
        case LowerInst::Not:
            return isPackedOp() ? OpVNot : OpNot;

        // The magnitude of an integer lane. A float one never reaches here: `expandVectorAbs` has
        // turned it into an `and` against a pooled mask, which is `OpVAnd`.
        case LowerInst::Abs: return OpVAbs;

        // The two that are one opcode across both banks: `sqrtss` and `sqrtps` differ in a mandatory
        // prefix, so a scalar square root and a packed one are the same machine operation at two
        // widths rather than two operations. Same for `vfmadd213`.
        case LowerInst::Sqrt: return OpSqrt;
        case LowerInst::Fma:  return OpFma;

        // A shuffle one `pshufd` expresses. The refusal is `selectPackedForm`'s rather than this
        // one's, because which shuffles those are is a property of the *pattern* and not of the
        // kind - so the opcode is answered here and the form is what does or does not exist.
        case LowerInst::VecShuffle:
            return OpVShuffle;

        case LowerInst::VecSplat:
            return OpVBroadcast;

        case LowerInst::VecLane:
            return OpVExtract;

        case LowerInst::VecWithLane:
            return OpVInsert;

        /*
         * A reduction is a tree of shuffles and pairwise operations, which §5.3 of
         * Implementation-Vector.md says to expand into IR rather than into a pseudo - so every kind
         * but one is refused here rather than mapped onto a neighbouring opcode, which is the
         * failure that would be silent.
         *
         * The one is `Bits`, which is not a tree and not a combination: it is `pmovmskb`, one
         * instruction, and `expandMaskReduce` writes it in terms of which the other three mask
         * reductions are ordinary integer arithmetic. It reaches here rather than being expanded
         * because there is nothing to expand it into.
         */
        case LowerInst::VecReduce:
            if(((LowerInstVecReduce*)inst)->getReduce() == LowerReduce::Bits) return OpVMaskBits;

            assertTrue("no machine opcode for this vector instruction yet" == nullptr);
            return OpNone;

        // A lane-wise select, which the scalar `cmov` is not a narrower version of - it moves one
        // whole value or the other, where this takes each lane from whichever side its own mask lane
        // names. `isPackedOp` reads the *result*, which is the vector; the condition is the mask.
        case LowerInst::Select:     return isPackedOp() ? OpVBlend : OpSelect;
        case LowerInst::Alloca:     return OpAlloca;
        case LowerInst::Load:       return OpLoad;
        case LowerInst::Store:      return OpStore;
        case LowerInst::Copy:       return OpBlockCopy;
        case LowerInst::SetPattern: return OpBlockSet;
        case LowerInst::Call:       return OpCall;
        case LowerInst::Je:         return OpJcc;
        case LowerInst::Jmp:        return OpJmp;
        case LowerInst::Ret:        return OpRet;
        case LowerInst::Unreachable: return OpNoReturn;
        case LowerInst::Phi:        return OpPhi;
        case LowerInst::X86Address: return OpAddress;
        case LowerInst::X86Lea:     return OpLea;

        // The minimum and the maximum, which are two opcodes rather than one with a flag for the
        // reason every other pair here is two: what the allocator and the encoder read is the same
        // for both, and what a form of one may not be is a form of the other.
        case LowerInst::X86MinMax:
            return ((LowerInstX86MinMax*)inst)->isMax() ? OpVMax : OpVMin;
        case LowerInst::Intrinsic:
            return machineTarget().intrinsic(((LowerInstIntrinsic*)inst)->getIntrinsic()).opcode;
        case LowerInst::X86PushArg: return OpPushArg;
    }

    assertTrue("no machine opcode for this instruction" == nullptr);
    return OpNone;
}

// The right-hand side of a binary operation, as the selector sees it: an immediate the peepholes
// embedded into the encoding, or an operand that still needs a register.
static bool hasEmbeddedRhs(LowerBase base, LowerInst* inst) {
    return isImm(base[((LowerInstBinary*)inst)->rhs]);
}

// The value of an embedded immediate operand. Only asked about operands hasEmbeddedRhs has already
// answered for, so the instruction behind it is an Imm by construction.
static U64 embeddedValue(LowerBase base, LowerPtr<LowerValue> operand) {
    return ((LowerImm*)base[operand]->inst())->i;
}

LowerType operationType(LowerBase base, const MachineForm& form, LowerInst* inst) {
    auto resultType = [&] {
        assertTrue(inst->createdCount > 0); // a form taking its width from a result that does not exist
        return inst->created()[0].type;
    };

    auto firstUseType = [&] {
        assertTrue(inst->usedCount > 0); // a form taking its width from an operand that does not exist
        return base[inst->used()[0]]->type;
    };

    auto secondUseType = [&] {
        assertTrue(inst->usedCount > 1); // a form taking its width from an operand that does not exist
        return base[inst->used()[1]]->type;
    };

    switch(form.encoding.width) {
        case OperationWidth::FromResult: return resultType();
        case OperationWidth::FromUse0:   return firstUseType();
        case OperationWidth::FromUse1:   return secondUseType();
        case OperationWidth::Fixed32:    return LowerType::Int32;
        case OperationWidth::Fixed64:    return LowerType::Int64;

        case OperationWidth::Narrowest:
            // A 32-bit move clears the upper half of its destination, so one encoding both truncates
            // a wide source and zero-extends a narrow one. Using the wider of the two would copy the
            // source's upper half unchanged when widening, propagating whatever it held.
            return is64Bit(firstUseType()) && is64Bit(resultType()) ? resultType() : LowerType::Int32;
    }

    return resultType();
}

Maybe<LowerCmp> selectCondition(LowerInst* inst) {
    switch(inst->kind) {
        case LowerInst::Cmp:
            return Just(((LowerInstCmp*)inst)->getCmp());

        // A condition that arrived in a register rather than in the flags is turned into flags by
        // the `test` the form declares as its prelude, and `test r, r` sets ZF exactly when the
        // register is zero - so "the condition holds" is the not-equal case.
        case LowerInst::Je: {
            auto embedded = ((LowerInstJe*)inst)->getEmbeddedCmp();
            return embedded ? embedded : Just(LowerCmp::neq);
        }

        case LowerInst::Select: {
            auto embedded = ((LowerInstSelect*)inst)->getEmbeddedCmp();
            return embedded ? embedded : Just(LowerCmp::neq);
        }

        default:
            return Nothing();
    }
}

// The rejections belong here rather than in the encoder: a selector that returned an integer form
// for a float operand, or a signed conversion for an unsigned one, would produce a working compile
// of the wrong program, and no later stage could tell.
static void requireIntLike(LowerType type) {
    assertTrue(isIntLike(type)); // an integer form asked for a floating-point operand
}

// One of the two scalar float forms an operation has, chosen by the width its operands work at.
static MachineFormId byFloatWidth(LowerType type, MachineFormId f32, MachineFormId f64) {
    assertTrue(isFloat(type)); // a floating-point form asked for an operand that is not one
    return type == LowerType::Float32 ? f32 : f64;
}

// The form an instruction takes, before the target's own features are consulted.
static MachineFormId selectFormForTarget(LowerBase base, LowerInst* inst);

// An instruction whose memory-capable operand holds an X86Address had a load folded into it, and
// takes the twin that reads it there rather than out of a register - see MachineForm::memorySource.
//
// The X86Address *is* the record of the fold. It is the one value that can only ever be an address,
// so an operand holding one is an operand the encoding dereferences, and there is no flag anywhere
// that has to be kept in step with the operand list. foldLoads in transform.cpp is what puts it
// there, including for a load whose pointer arrived in a register - `[reg]` is an addressing mode
// like any other, and making it one is what keeps this question answerable from the value alone.
static MachineFormId selectMemorySourceForm(LowerBase base, MachineFormId id, LowerInst* inst) {
    auto& form = machineTarget().form(id);
    if(!form.memorySource) return id;

    auto memory = form.memoryUse();
    assertTrue(memory >= 0 && Size(memory) < inst->used().size()); // a twin of a form with no memory operand

    return isMem(base[inst->used()[memory]]) ? form.memorySource : id;
}

// The same operation written with a vector prefix, where this target can encode one - see
// MachineForm::alternative. A swap made from a fact about the *target* rather than about the
// instruction, which is what lets it be one line here instead of a feature test in every case of
// selectFormForTarget below.
static MachineFormId selectAlternativeForm(MachineFormId id) {
    // A chain rather than a single step: the legacy form's alternative is the VEX one and the VEX
    // form's is the EVEX one, so the highest tier the target can encode is the last link that
    // answers. Walking it is what keeps each tier derived from the one below rather than every tier
    // having to know about the ones above it.
    for(;;) {
        auto alternative = machineTarget().form(id).alternative;
        if(!alternative) return id;

        auto& form = machineTarget().form(alternative);
        if((form.requiredFeatures & ~targetFeatures()) != 0) return id;

        id = alternative;
    }
}

MachineFormId selectForm(LowerBase base, LowerInst* inst) {
    // The alternative first and the memory twin second, because the twin of a VEX form is a VEX form
    // and the alternative of a memory form is a memory form. For the scalar set either order reaches
    // the same place, both links being complete.
    //
    // For the packed set only this one does, and that is the whole of how the vector load fold is
    // target-dependent: a legacy packed operation faults on an unaligned memory operand, so it has
    // no memory twin at all and the fold is reachable only *through* the alternative. Applying the
    // memory swap first would find nothing to swap to and leave the operation reading a register
    // that the fold had already removed the definition of.
    auto id = selectMemorySourceForm(base, selectAlternativeForm(selectFormForTarget(base, inst)), inst);

    // A form whose encoding needs an extension this build does not have is not selectable, and the
    // rejection belongs here rather than in the encoder: by then the operands have been allocated
    // against the form's constraints and there is nothing left to choose instead. Checked for every
    // form rather than for the intrinsics alone, which is what makes adding a VEX or EVEX
    // alternative a question of listing it with its features rather than of remembering to guard it.
    assertTrue((machineTarget().form(id).requiredFeatures & ~targetFeatures()) == 0);
    return id;
}

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
static bool isWholePackedRegister(LowerType type) {
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
static MachineFormId widthForm(MachineFormId narrow, LowerType type) {
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
static bool packedCompareIsInverted(LowerType type, LowerCmp cmp) {
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

LowerImm* packedShiftConstantCount(LowerBase base, LowerInst* inst) {
    auto count = base[((LowerInstBinary*)inst)->rhs]->inst();

    // A splat of a constant is the language's spelling and a bare constant is the fixtures'. One
    // level of unwrapping and no more: a splat of a runtime value is the other machine form's
    // business, and a splat of a splat is not a thing this IR produces.
    if(count->kind == LowerInst::VecSplat) count = base[((LowerInstVecSplat*)count)->from]->inst();

    return count->kind == LowerInst::Imm ? (LowerImm*)count : nullptr;
}

static MachineFormId selectPackedForm(LowerBase base, LowerInst* inst) {
    // Indexed by laneColumn: i8, i16, i32, i64, f32, f64.
    static const MachineFormId kAdd[6] = { FormVAdd8, FormVAdd16, FormVAdd32, FormVAdd64, FormVAddF32, FormVAddF64 };
    static const MachineFormId kSub[6] = { FormVSub8, FormVSub16, FormVSub32, FormVSub64, FormVSubF32, FormVSubF64 };
    // The 32-bit column is filled in below rather than here, because which of its two forms applies
    // is a feature question and a row is not.
    static const MachineFormId kMul[6] = { 0, FormVMul16, 0, 0, FormVMulF32, FormVMulF64 };
    static const MachineFormId kDiv[6] = { 0, 0, 0, 0, FormVDivF32, FormVDivF64 };

    static const MachineFormId kShlImm[6] = { 0, FormVShl16Imm, FormVShl32Imm, FormVShl64Imm, 0, 0 };
    static const MachineFormId kShrImm[6] = { 0, FormVShr16Imm, FormVShr32Imm, FormVShr64Imm, 0, 0 };
    static const MachineFormId kSarImm[6] = { 0, FormVSar16Imm, FormVSar32Imm, 0, 0, 0 };

    static const MachineFormId kCmpEq[6] = { FormVCmpEq8, FormVCmpEq16, FormVCmpEq32, 0, FormVCmpF32, FormVCmpF64 };
    static const MachineFormId kCmpGt[6] = { FormVCmpGt8, FormVCmpGt16, FormVCmpGt32, 0, FormVCmpF32, FormVCmpF64 };

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
        case LowerInst::Div: {
            auto type = ((LowerInstBinary*)inst)->result.type;
            if(!isVectorLike(type)) return 0;

            switch(inst->kind) {
                case LowerInst::Add: return packedForm(kAdd, type);
                case LowerInst::Sub: return packedForm(kSub, type);
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
             * One constant count for every lane. A count in a register - per lane, or shared - is
             * the gap the form table names.
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
            assertTrue(packedShiftConstantCount(base, inst)); // no packed shift by a register count yet

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
         * A lane-wise select, which is one form for every lane type and for a mask: the three
         * bitwise instructions it expands into have no lane width, so nothing here indexes a row.
         */
        case LowerInst::Select: {
            auto type = ((LowerInstSelect*)inst)->result.type;
            if(!isVectorLike(type)) return 0;
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

                return laneBytes(type.lane) == 8 && !type.isMask()
                    ? FormVWideExtract64 : FormVWideExtract32;
            }

            if(isFloatVector(type)) {
                return laneBytes(type.lane) == 4 ? FormVExtractF32 : FormVExtractF64;
            }

            /*
             * A mask reads at 32 bits whatever its lanes are wide, because `scalarFormOf` says a
             * mask's scalar form is an `Int32` at every lane width - what a lane holds is a truth
             * value and not a number of that width. Reading the lane's own width instead would
             * define a 64-bit register for a value the rest of the function has typed as 32-bit,
             * which is a class disagreement rather than a wrong number.
             */
            auto wide = laneBytes(type.lane) == 8 && !type.isMask();
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
         * One row at each tier and no lane table: `pmovmskb` reads bytes whatever the mask's lanes
         * are, so the lane width is the arithmetic above it and not the form.
         */
        case LowerInst::VecReduce: {
            if(((LowerInstVecReduce*)inst)->getReduce() != LowerReduce::Bits) return 0;

            return isWideVector(base[((LowerInstVecReduce*)inst)->from]->type)
                ? FormVWideMaskBits : FormVMaskBits;
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
            auto reduce = ((LowerInstVecReduce*)inst)->getReduce();

            if(reduce == LowerReduce::Mul && !isFloatVector(type) && laneColumn(type) != 2) {
                return Just("the machine has no packed integer multiply at this lane width, so neither has a product reduction"_v);
            }

            /*
             * A minimum or a maximum, whose pairwise step is a packed comparison - so it reaches
             * exactly the lane widths one does. That used to be the 32-bit lane alone for the
             * *unsigned* pair, because the bias `biasUnsignedPackedCompares` builds is a splat and
             * the narrow broadcasts did not exist; both halves are general now, and what is left is
             * the quadword, where `pcmpgtq` is SSE4.2.
             */
            auto ordering = reduce == LowerReduce::Min || reduce == LowerReduce::Max ||
                            reduce == LowerReduce::IMin || reduce == LowerReduce::IMax;

            if(isIntVector(type) && laneBytes(type.lane) == 8 && ordering) {
                return Just("the machine has no packed comparison of a quadword integer lane before SSE4.1, so neither has a minimum or a maximum of one"_v);
            }

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
                    // The refusal that is specific to this tier, and the one worth naming
                    // separately: every shuffle AVX2 has works *inside* each 128-bit half, so a
                    // pattern that moves a lane across the middle is not an instruction unless it
                    // moves the whole half. `vpermd` would express the general case and takes its
                    // pattern out of a vector register, which needs the constant pool this backend
                    // has not opened to vectors.
                    return Just("no single instruction here expresses this lane pattern at 256 bits - every shuffle at this width works inside each 128-bit half, and the only crossing is an exchange of whole halves"_v);
                }

                return Just("no single instruction here expresses this lane pattern - `shufps` takes a run of lanes from each source and the interleaves take one of each, and an 8- or 16-bit lane has nothing beyond those"_v);
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
            if(laneColumn(type) != 1 && laneColumn(type) != 2 && !isFloatVector(type)) {
                return Just("the machine has no packed integer multiply of a byte or quadword lane at any feature level"_v);
            }

            return {};

        // The high half, which exists at a 16-bit lane (`pmulhw`/`pmulhuw`) and is not in the form
        // table - so it is refused with the widths that have no instruction at all.
        case LowerInst::MulHi:
        case LowerInst::IMulHi:
            return Just("this backend has no packed multiply-high yet"_v);

        case LowerInst::Div:
        case LowerInst::IDiv:
        case LowerInst::Rem:
        case LowerInst::IRem:
            if(!isFloatVector(type)) {
                return Just("no x86 has a packed integer divide or remainder - it expands lane by lane, which this backend does not do yet"_v);
            }

            return {};

        case LowerInst::Shl:
        case LowerInst::Shr:
        case LowerInst::Sar: {
            /*
             * Read through the splat the language's spelling wraps the count in - see
             * `packedShiftConstantCount`, and `unwrapVectorShiftCounts` in transform.cpp, which is
             * the pass this stands on the other side of.
             *
             * What is left refused is a count that is not a constant at all. The machine does have a
             * form for that - the count in the low quadword of a vector register - and reaching it
             * needs the scalar moved into lane zero with the rest cleared, since `pslld` reads the
             * whole quadword as one count and a splat would arrive as a number far past the lane
             * width. That transfer is the missing piece rather than the instruction.
             */
            if(!packedShiftConstantCount(base, inst)) {
                return Just("a packed shift by a count that is not a constant is not implemented here - the count would have to be moved into lane zero of a vector register with the rest cleared"_v);
            }

            if(laneBytes(type.lane) == 1) return Just("the machine has no packed shift of a byte lane"_v);

            if(inst->kind == LowerInst::Sar && laneBytes(type.lane) == 8) {
                return Just("there is no packed arithmetic shift of a quadword before AVX-512"_v);
            }

            return {};
        }

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
             * There is no packed compare of a quadword lane before SSE4.1 (`pcmpeqq`) and SSE4.2
             * (`pcmpgtq`), and no *unsigned* packed compare at any level: the four unsigned
             * relations would each need both operands biased by the sign bit first. Both are refused
             * here rather than reaching a row that would answer for the neighbouring width.
             */
            if(isFloatVector(type)) return {};
            if(laneBytes(type.lane) == 8) {
                return Just("the machine has no packed comparison of a quadword integer lane before SSE4.1"_v);
            }

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
         * mask at both float ones - so the only gap is the quadword integer lane, where there is no
         * `pabsq` outside AVX-512 and no `pcmpgtq` below SSE4.2 to build the comparison-and-select
         * fallback out of either.
         */
        case LowerInst::Abs:
            if(isIntVector(type) && laneBytes(type.lane) == 8) {
                return Just("the machine has no packed absolute value of a quadword integer lane before AVX-512"_v);
            }

            return {};

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

static MachineFormId selectFormForTarget(LowerBase base, LowerInst* inst) {
    if(auto packed = selectPackedForm(base, inst)) return packed;

    switch(inst->kind) {
        case LowerInst::Nop:        return FormNop;
        case LowerInst::Arg:        return FormArg;
        case LowerInst::Phi:        return FormPhi;
        case LowerInst::X86Address: return FormAddress;
        case LowerInst::X86Lea:     return FormLea;

        /*
         * Answered above, at every width rather than at the packed ones alone - see the note there.
         * Listed here so that the -Wswitch sweep keeps saying something about this switch.
         *
         * The minimum and the maximum are here for a slightly different reason: they are packed at
         * every width they exist at, so `selectPackedForm` has answered for every one that reaches
         * this. One that did not would be an instruction built at a lane the form table has no row
         * for, which is what `packedMinMaxSupported` exists to have refused.
         */
        case LowerInst::X86MinMax:
        case LowerInst::Abs:
        case LowerInst::Sqrt:
        case LowerInst::Fma:
            assertTrue("a packed instruction selectPackedForm did not answer for" == nullptr);
            return FormNop;

        // An intrinsic's form is a row of the registry rather than a case here - see intrinsic.cpp.
        // What is checked at the point of selection is what only the target knows: that this build
        // has the features the encoding needs, and that the values the program gave it are ones the
        // instruction accepts.
        case LowerInst::Intrinsic: {
            auto intrinsic = (LowerInstIntrinsic*)inst;
            auto& desc = machineTarget().intrinsic(intrinsic->getIntrinsic());

            assertTrue(desc.defined); // an intrinsic this target has no description for
            assertTrue((desc.requiredFeatures & ~targetFeatures()) == 0); // ... that it cannot encode
            assertTrue(checkIntrinsicOperands(base, desc, intrinsic)); // ... with operands it cannot take

            return desc.form;
        }

        case LowerInst::Global:
            return isImplicit(&((LowerInstGlobal*)inst)->result) ? FormGlobalImplicit : FormGlobalAddress;

        case LowerInst::X86PushArg: {
            auto arg = base[((LowerInstX86PushArg*)inst)->arg];
            if(isImm(arg)) return FormPushArgImm;
            if(isFloat(arg->type)) return byFloatWidth(arg->type, FormPushArgF32, FormPushArgF64);
            return FormPushArgReg;
        }

        case LowerInst::Imm: {
            // Decided from the value alone. Whether the immediate is embedded is a peephole's
            // answer and may still change; whether it would be materialized with `xor` or with
            // `mov` is not, which is what lets the compare folding read the flags effect early.
            auto imm = (LowerImm*)inst;
            if(isImplicit(&imm->result)) return FormImmImplicit;

            // No SSE encoding carries a float as an immediate, so a float constant is never
            // embedded (isEmbeddableImm says so) and always takes the materializing pseudo.
            auto type = imm->result.type;
            if(isFloat(type)) return byFloatWidth(type, FormImmFloat32, FormImmFloat64);

            return imm->i == 0 ? FormImmZero : FormImmMov;
        }

        case LowerInst::Fun:
            return isImplicit(&((LowerInstFun*)inst)->result) ? FormFunctionImplicit : FormFunctionAddress;

        case LowerInst::Set: {
            auto type = ((LowerInstUnary*)inst)->result.type;
            if(isFloat(type)) return byFloatWidth(type, FormMoveF32, FormMoveF64);
            return FormMove;
        }

        case LowerInst::Cast: {
            auto cast = (LowerInstCast*)inst;
            auto from = base[cast->from]->type;
            auto to = cast->result.type;

            // Between the banks, where only the signed direction has an encoding. An unsigned
            // conversion never reaches here: it is a sequence rather than an instruction, and
            // expandBankConversions replaced it with one made of signed ones before selection
            // ran. Emitting a signed instruction for it instead would be wrong for exactly the
            // values that motivated asking for unsigned in the first place.
            if(isFloat(from) != isFloat(to)) {
                if(isFloat(to)) {
                    assertTrue(cast->isSignedSource()); // an unsigned conversion the expansion missed
                    return byFloatWidth(to, FormCastIToF32, FormCastIToF64);
                }

                assertTrue(cast->isSignedResult()); // an unsigned conversion the expansion missed
                return byFloatWidth(from, FormCastF32ToI, FormCastF64ToI);
            }

            if(isFloat(from)) {
                assertTrue(from != to); // a cast between one float type and itself
                return from == LowerType::Float32 ? FormCastF32ToF64 : FormCastF64ToF32;
            }

            requireIntLike(from);
            requireIntLike(to);
            // An embedded constant makes the cast a materialization, and zero is materialized with
            // `xor` here for the same reason it is under Imm above. Which of the two it is depends
            // on the value alone; whether the source is embedded at all is a peephole's answer, and
            // is settled before anything asks what this writes - the flags window is walked in a
            // sweep of its own, after every form decision a peephole makes.
            auto source = base[cast->from];
            if(isImm(source)) return immValue(source) == 0 ? FormCastZero : FormCastImm;

            // A cast the peephole proved changes no bit is a copy, and a copy between one register
            // and itself is nothing. Asked before the sign question below because it subsumes it:
            // the peephole never marks a widening that has a sign bit to carry.
            if(cast->skipsExtend()) return FormCastCopy;

            /*
             * Only a signed value *widened* into a signed one has to carry its sign bit up; every
             * other cast between integer classes is the truncating-and-clearing move.
             *
             * Widening rather than merely signed, because `movsxd` reads a 32-bit source whatever
             * register it is given - it is the 32-to-64 encoding and there is no other. Choosing it
             * for a cast whose source is already 64 bits drops the top half and sign-extends what is
             * left, which is silent: the values it is wrong for are exactly the ones that do not fit
             * in 32 bits. A refinement of a 64-bit type widening to the type it refines - `@bits(40)
             * WideInt` to `WideInt` - is signed at both ends and 64 bits at both ends, and is what
             * reached this.
             */
            auto widens = !is64Bit(from) && is64Bit(to);
            return widens && cast->isSignedSource() && cast->isSignedResult() ? FormCastSext
                                                                             : FormCastMov;
        }

        case LowerInst::Bitcast: {
            auto bitcast = (LowerInstUnary*)inst;
            auto from = base[bitcast->from]->type;
            auto to = bitcast->result.type;

            // Between two vectors it is the register read at another lane shape, which is no
            // operation on the bits at all. It reaches a form rather than nothing because the two
            // ends are still separate *values* and the allocator may have put them in two places.
            if(isVectorLike(from) || isVectorLike(to)) {
                assertTrue(isWholePackedRegister(from) && isWholePackedRegister(to));
                return widthForm(FormVBitcast, to);
            }

            // A bitcast preserves the width, so crossing the banks is MOVD or MOVQ and which of the
            // two is decided by that width alone.
            if(isFloat(from) != isFloat(to)) {
                assertTrue(is64Bit(from) == is64Bit(to)); // a bitcast between two different widths

                return isFloat(to)
                    ? byFloatWidth(to, FormBitcastIToF32, FormBitcastIToF64)
                    : byFloatWidth(from, FormBitcastF32ToI, FormBitcastF64ToI);
            }

            // Within the vector bank a bitcast is a copy, and one between a register and itself is
            // no instruction at all.
            if(isFloat(from)) {
                assertTrue(from == to); // a bitcast between two float types of different widths
                return byFloatWidth(to, FormBitcastF32, FormBitcastF64);
            }

            requireIntLike(from);
            requireIntLike(to);

            // The same two materializing forms a constant-sourced cast takes, chosen the same way.
            auto source = base[bitcast->from];
            if(isImm(source)) return immValue(source) == 0 ? FormBitcastZero : FormBitcastImm;

            return FormBitcast;
        }

        case LowerInst::Neg: {
            auto type = ((LowerInstUnary*)inst)->result.type;
            if(isFloat(type)) return byFloatWidth(type, FormFNeg32, FormFNeg64);
            return FormNeg;
        }

        case LowerInst::Not:
            requireIntLike(base[((LowerInstUnary*)inst)->from]->type);
            return FormNot;

        case LowerInst::And: return hasEmbeddedRhs(base, inst) ? FormAndImm : FormAndReg;
        case LowerInst::Or:  return hasEmbeddedRhs(base, inst) ? FormOrImm : FormOrReg;
        case LowerInst::Xor: return hasEmbeddedRhs(base, inst) ? FormXorImm : FormXorReg;

        // A comparison whose result the folding could not leave in the flags has to be materialized
        // into a register afterwards, which is a form of its own rather than a tail the encoder
        // decides to add.
        case LowerInst::Cmp: {
            auto type = base[((LowerInstBinary*)inst)->lhs]->type;
            auto materialize = !isImplicit(&((LowerInstCmp*)inst)->result);

            if(isFloat(type)) {
                return materialize
                    ? byFloatWidth(type, FormFCmp32Set, FormFCmp64Set)
                    : byFloatWidth(type, FormFCmp32, FormFCmp64);
            }

            requireIntLike(type);

            // §3.5.2.2 The elided one is asked for first, because it is the one answer that is not
            // about how the operands arrived: nothing is emitted, so nothing about the encoding is
            // left to decide. The folding only ever sets it on a comparison it also merged, so
            // `materialize` is false wherever this is true.
            if(((LowerInstCmp*)inst)->getFlagsLive()) return FormCmpNone;

            if(hasEmbeddedRhs(base, inst)) return materialize ? FormCmpImmSet : FormCmpImm;
            return materialize ? FormCmpRegSet : FormCmpReg;
        }

        // Add and subtract come through here too, since the two banks share their IR instruction.
        case LowerInst::Add:
        case LowerInst::Sub:
        case LowerInst::Mul:
        case LowerInst::Div: {
            auto type = ((LowerInstBinary*)inst)->result.type;

            if(isFloat(type)) {
                switch(inst->kind) {
                    case LowerInst::Add: return byFloatWidth(type, FormFAdd32, FormFAdd64);
                    case LowerInst::Sub: return byFloatWidth(type, FormFSub32, FormFSub64);
                    case LowerInst::Mul: return byFloatWidth(type, FormFMul32, FormFMul64);
                    default:             return byFloatWidth(type, FormFDiv32, FormFDiv64);
                }
            }

            // An addition or subtraction of one is a byte shorter as `inc`/`dec`, and which of the
            // two it is depends only on the constant - so it is chosen here rather than noticed by
            // the encoder. A subtraction of one decrements, and of minus one increments.
            if(inst->kind == LowerInst::Add || inst->kind == LowerInst::Sub) {
                auto reg = inst->kind == LowerInst::Add ? FormAddReg : FormSubReg;
                if(!hasEmbeddedRhs(base, inst)) return reg;

                auto value = embeddedValue(base, ((LowerInstBinary*)inst)->rhs);
                auto up = inst->kind == LowerInst::Add ? FormAddInc : FormSubInc;
                auto down = inst->kind == LowerInst::Add ? FormAddDec : FormSubDec;

                if(value == 1) return inst->kind == LowerInst::Add ? up : down;
                if(value == U64(I64(-1))) return inst->kind == LowerInst::Add ? down : up;
                return inst->kind == LowerInst::Add ? FormAddImm : FormSubImm;
            }

            // The group-3 multiply and divide read and write the rdx:rax pair, which only the
            // integer encodings have.
            assertTrue(isInt(type)); // no integer form for this type
            return inst->kind == LowerInst::Mul ? FormMul : FormDiv;
        }

        case LowerInst::IDiv:
        case LowerInst::Rem:
        case LowerInst::IRem:
        case LowerInst::MulHi:
        case LowerInst::IMulHi:
        case LowerInst::IMul: {
            assertTrue(isInt(((LowerInstBinary*)inst)->result.type)); // no integer form for this type

            switch(inst->kind) {
                case LowerInst::IDiv:   return FormIDiv;
                case LowerInst::Rem:    return FormRem;
                case LowerInst::IRem:   return FormIRem;
                case LowerInst::MulHi:  return FormMulHi;
                case LowerInst::IMulHi: return FormIMulHi;
                default: return hasEmbeddedRhs(base, inst) ? FormIMulImm : FormIMulReg;
            }
        }

        // A shift by one has an encoding that carries no immediate byte at all.
        case LowerInst::Shl:
        case LowerInst::Shr:
        case LowerInst::Sar: {
            static const struct { MachineFormId imm, one, cl; } shifts[] = {
                { FormShlImm, FormShlOne, FormShlCl },
                { FormShrImm, FormShrOne, FormShrCl },
                { FormSarImm, FormSarOne, FormSarCl },
            };

            auto& forms = shifts[inst->kind - LowerInst::Shl];
            if(!hasEmbeddedRhs(base, inst)) return forms.cl;
            return embeddedValue(base, ((LowerInstBinary*)inst)->rhs) == 1 ? forms.one : forms.imm;
        }

        case LowerInst::Select: {
            auto select = (LowerInstSelect*)inst;
            auto onFlags = select->getEmbeddedCmp();
            auto type = select->result.type;

            if(isFloat(type)) {
                return onFlags
                    ? byFloatWidth(type, FormSelectFloat32Flags, FormSelectFloat64Flags)
                    : byFloatWidth(type, FormSelectFloat32Reg, FormSelectFloat64Reg);
            }

            return onFlags ? FormSelectFlags : FormSelectReg;
        }

        case LowerInst::Alloca:
            return isImm(base[((LowerInstAlloca*)inst)->byteCount]) ? FormAllocaFixed : FormAllocaDynamic;

        // One form per access width and signedness: a narrow load extends into the whole destination
        // register, which is a different opcode rather than a different operand size. A 4-byte load
        // only needs one when its result is wider than it is, since a 32-bit move already clears the
        // upper half of what it writes.
        case LowerInst::Load: {
            auto load = (LowerInstLoad*)inst;
            auto isSigned = load->isSigned();

            // A whole vector, in whichever domain its lanes are: the two spellings do the same
            // thing and differ in a forwarding penalty for a value produced in one and consumed in
            // the other. The width is the vector's own - validateLoad has already required it - so
            // there is nothing here to choose from it.
            if(isVectorLike(load->result.type)) {
                // A whole register, at whichever of the two widths this backend holds one at.
                assertTrue(isWholePackedRegister(load->result.type));
                return widthForm(isFloatVector(load->result.type) ? FormVLoad : FormVLoadInt,
                                 load->result.type);
            }

            // A float is loaded by the instruction that owns its bank, at exactly its own width:
            // nothing extends into a vector register, so there is no narrow form to choose between.
            if(isFloat(load->result.type)) {
                assertTrue(load->getWidth() == (load->result.type == LowerType::Float32 ? 4u : 8u));
                return byFloatWidth(load->result.type, FormLoadF32, FormLoadF64);
            }

            switch(load->getWidth()) {
                case 1: return isSigned ? FormLoad8S : FormLoad8;
                case 2: return isSigned ? FormLoad16S : FormLoad16;
                case 4: return isSigned && is64Bit(load->result.type) ? FormLoad32S : FormLoad32;
                default: return FormLoad64;
            }
        }

        case LowerInst::Store: {
            auto store = (LowerInstStore*)inst;
            auto type = base[store->value]->type;

            if(isVectorLike(type)) {
                assertTrue(isWholePackedRegister(type)); // a whole register, at either width
                return widthForm(isFloatVector(type) ? FormVStore : FormVStoreInt, type);
            }

            if(isFloat(type)) {
                assertTrue(store->getWidth() == (type == LowerType::Float32 ? 4u : 8u));
                return byFloatWidth(type, FormStoreF32, FormStoreF64);
            }

            // A constant goes into the encoding rather than into a register. Every width has such a
            // form and every one of them declares Imm32, so this answers for exactly the constants
            // `canEmbedImm` has already accepted - which it has to, since by here the operand may
            // have been taken out of allocation and have no register to fall back to.
            if(isImm(base[store->value]) &&
               fitsImmediate(ImmediateWidth::Imm32, embeddedValue(base, store->value)))
            {
                switch(store->getWidth()) {
                    case 1: return FormStore8Imm;
                    case 2: return FormStore16Imm;
                    case 4: return FormStore32Imm;
                    default: return FormStore64Imm;
                }
            }

            switch(store->getWidth()) {
                case 1: return FormStore8;
                case 2: return FormStore16;
                case 4: return FormStore32;
                default: return FormStore64;
            }
        }

        /*
         * Which of the two unrolled forms is the one question left to the value rather than to the
         * instruction: the count is folded into the unrolling wherever it is implicit, and implicit
         * is something a constant is or is not - a count some other instruction still reads out of a
         * register is neither. See the pair in the table above.
         */
        case LowerInst::Copy: {
            auto copy = (LowerInstCopy*)inst;
            if(!copy->isUnrolled()) return FormBlockCopyRep;
            return isImplicit(base[copy->count]) ? FormBlockCopyUnrolled : FormBlockCopyUnrolledCount;
        }

        case LowerInst::SetPattern: {
            auto set = (LowerInstSetPattern*)inst;
            if(!set->isUnrolled()) return FormBlockSetRep;
            return isImplicit(base[set->count]) ? FormBlockSetUnrolled : FormBlockSetUnrolledCount;
        }

        case LowerInst::Call: {
            auto call = (LowerInstCall*)inst;
            if(call->getCallType() == LowerCallType::Syscall) return FormSyscall;

            // A statically known callee is a rel32 call that never reads the address out of a
            // register; anything else goes through one.
            auto callee = base[call->used()[0]];
            return callee->inst()->kind == LowerInst::Fun ? FormCallDirect : FormCallIndirect;
        }

        /*
         * Three forms rather than two. A branch reading the flags is the merged one where the
         * comparison went nowhere else and the folded one where it did: `Implicit` on the condition
         * is what distinguishes them, and it is the same question every other folded operand is
         * asked, so the verifier's rule about a folded operand needing no location keeps holding.
         */
        case LowerInst::Je: {
            auto je = (LowerInstJe*)inst;
            if(!je->getEmbeddedCmp()) return FormJccReg;
            return isImplicit(base[je->cond]) ? FormJccFlags : FormJccLive;
        }
        case LowerInst::Jmp: return FormJmp;
        case LowerInst::Ret: return FormRet;
        case LowerInst::Unreachable: return FormNoReturn;

        // A shuffle reaches `selectPackedForm` above rather than this switch, which is where every
        // packed form is chosen. Falling through to the failure below would mean the pattern was one
        // no form expresses, and that is the refusal `packedShufflePattern` states.
        case LowerInst::VecShuffle:
            break;

        // A broadcast and a lane extract reach `selectPackedForm` too, and for the same reason:
        // their forms are chosen by the lane, which is where every packed form is chosen from.
        case LowerInst::VecSplat:
        case LowerInst::VecLane:
            break;

        // The two that only a vector can be an operand of. `opcodeFor` has already refused each of
        // them - the machine operations are named in §5.3 of Implementation-Vector.md and neither is
        // described yet - so this is unreachable rather than a second refusal. Except for the one
        // reduction that *is* an instruction, which `selectPackedForm` answers above.
        case LowerInst::VecWithLane:
        case LowerInst::VecReduce:
            break;
    }

    assertTrue("no machine form for this instruction" == nullptr);
    return FormNop;
}

I32 opcodeAddressOperand(MachineOpcodeId opcode) {
    // The first form of the opcode answers for all of them: validateMachineForms requires them to
    // agree, which is what lets this be asked before selection has chosen between them.
    //
    // Except for the memory-source twins, which are skipped here. An ALU operation whose load has
    // been folded does reference memory, and the passes that ask this run before that fold - so the
    // answer they need is the one for an instruction the fold has not touched. What reads the twin's
    // address operand is legalization and the verifiers, which ask the *selected form* and get the
    // right answer for both.
    for(auto& form: machineTarget().forms) {
        if(form.opcode == opcode && !form.memorySourceOf) return form.addressOperand();
    }

    return -1;
}

bool opcodeCanEmbedImmediate(MachineOpcodeId opcode, Size index, U64 value) {
    // Every form of the opcode, rather than the first one that names an immediate there: which form
    // an instruction ends up in is not settled while the peepholes are still running, so the
    // question is whether *any* of them could carry this value in this position.
    for(auto& form: machineTarget().forms) {
        if(form.opcode != opcode) continue;
        if(index >= form.uses.size()) continue;
        if(form.uses[index].kind != OperandConstraintKind::Immediate) continue;
        if(fitsImmediate(form.uses[index].immediate, value)) return true;
    }

    return false;
}
