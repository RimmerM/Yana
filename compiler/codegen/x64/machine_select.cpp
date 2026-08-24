#include "machine_internal.h"

/*
 * Selection.
 */

// The one statement of which operations have an in-place memory form and what each one's is - see
// StoreUpdateOp, and the registration above, which builds the six forms of each row from it.
static const StoreUpdateOp kStoreUpdateOps[] = {
    { LowerInst::Add, OpStoreAdd, FormStoreAdd8 },
    { LowerInst::Sub, OpStoreSub, FormStoreSub8 },
    { LowerInst::And, OpStoreAnd, FormStoreAnd8 },
    { LowerInst::Or,  OpStoreOr,  FormStoreOr8  },
    { LowerInst::Xor, OpStoreXor, FormStoreXor8 },
};

Buffer<const StoreUpdateOp> storeUpdateOps() {
    return Buffer<const StoreUpdateOp> { kStoreUpdateOps, sizeof(kStoreUpdateOps) / sizeof(StoreUpdateOp) };
}

const StoreUpdateOp* storeUpdateOpFor(LowerInst::Kind op) {
    for(auto& row: kStoreUpdateOps) {
        if(row.op == op) return &row;
    }

    return nullptr;
}

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
        case LowerInst::VZeroUpper: return OpVZeroUpper;
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
        // The high half, which is packed at a 16-bit lane and scalar everywhere else - and which
        // keeps its two kinds apart at both, the top of a product being where a multiplication's
        // signedness first shows in its bits.
        case LowerInst::MulHi:      return isPackedOp() ? OpVMulHi : OpMulHi;
        case LowerInst::IMulHi:     return isPackedOp() ? OpVIMulHi : OpIMulHi;

        // The six the IR states once and the machine has twice or three times, one operation per
        // bank and per packing. A packed operation is asked about first because a vector of floats
        // answers yes to neither of the other two: `isFloat` is a scalar predicate by construction.
        case LowerInst::Shl:        return isPackedOp() ? OpVShl : OpShl;
        case LowerInst::Shr:        return isPackedOp() ? OpVShr : OpShr;
        case LowerInst::Sar:        return isPackedOp() ? OpVSar : OpSar;

        // The rotations, which have no packed opcode at all: `expandVectorRotate` has rewritten a
        // vector one into shifts and an `or` long before selection, so only the scalar pair can
        // reach here. Asserted rather than answered, for `expandRoundAway`'s reason.
        case LowerInst::Rol:        assertTrue(!isPackedOp()); return OpRol;
        case LowerInst::Ror:        assertTrue(!isPackedOp()); return OpRor;
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

        // The byte reversal, which has no packed spelling to choose between: a lane-wise one is a
        // shuffle against a pattern, and the IR has already refused a vector operand.
        case LowerInst::Bswap: return OpBswap;

        /*
         * The three BMI2 operations, which never reach selection: `expandBitOperations` has replaced
         * each of them with an intrinsic or with the arithmetic network it stands for, long above
         * here. Asserted rather than answered, for `expandRoundAway`'s reason - a case that answered
         * would be a form for an operation this machine has no opcode for.
         */
        case LowerInst::BitsUpTo:
        case LowerInst::GatherBits:
        case LowerInst::ScatterBits:
            assertTrue("a bit operation reached selection unexpanded" == nullptr);
            return OpNone;

        // The checksum step, which unlike the three above has no expansion in front of it: the
        // instruction is baseline, so this is where it arrives and where it stays.
        case LowerInst::Crc32: return OpCrc32;


        // The two accesses that reverse on the way. Written only where the feature is present, so
        // reaching one of these is already a decision - see selectByteSwapMemory.
        case LowerInst::X86MovbeLoad:  return OpMovbeLoad;
        case LowerInst::X86MovbeStore: return OpMovbeStore;

        // The magnitude of an integer lane. A float one never reaches here: `expandVectorAbs` has
        // turned it into an `and` against a pooled mask, which is `OpVAnd`.
        case LowerInst::Abs: return OpVAbs;

        // The two that are one opcode across both banks: `sqrtss` and `sqrtps` differ in a mandatory
        // prefix, so a scalar square root and a packed one are the same machine operation at two
        // widths rather than two operations. Same for `vfmadd213`.
        case LowerInst::Sqrt: return OpSqrt;
        case LowerInst::Fma:  return OpFma;

        // The SHA extension's two kinds, one opcode each. Which of the seven instructions a
        // `ShaBinary` is is the *form*'s answer rather than this one's, exactly as which shuffle a
        // `VecShuffle` is is.
        case LowerInst::ShaBinary:    return OpSha;
        case LowerInst::Sha256Rounds: return OpSha256Rounds;

        // The three directed roundings, which are one instruction with the mode in a trailing byte -
        // so one op here, exactly as the four square roots are one. `Round` is absent because
        // `expandRoundAway` has already replaced it; reaching this with one is the bug that assert
        // is for.
        case LowerInst::Trunc:
        case LowerInst::Floor:
        case LowerInst::Ceil:  return OpRound;
        case LowerInst::Round:
            assertTrue("a ties-away round expandRoundAway did not rewrite" == nullptr);
            return OpNop;

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

        // The in-place update, whose operation is the instruction's own field rather than anything
        // read off its operands: the value it combines with the location may be of any width the
        // access truncates to, so the type says nothing about which of the five this is.
        case LowerInst::X86StoreOp:
            return storeUpdateOpFor(((LowerInstX86StoreOp*)inst)->getOp())->opcode;

        // One opcode at all three source widths, which is where this differs from every other pair
        // in this table: what the width picks is a *form*, since it is an opcode byte rather than
        // anything the allocator or the flags window reads.
        case LowerInst::X86Sext:  return OpSext;

        // The two BMI1 rewrites, which are only ever written where the feature is present - see
        // `selectBitPeepholes`, where the test is.
        case LowerInst::X86AndNot: return OpAndNot;
        case LowerInst::X86LowBit: return OpLowBit;

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

        // And the masked vector, which is the bitwise `and` this table already has - the kind exists
        // to carry which of the two it is and to have made the zero operand go away, not to name an
        // instruction the opcode list was missing.
        case LowerInst::X86MulWide:
            return ((LowerInstX86MulWide*)inst)->isSignedLanes() ? OpVIMulWide : OpVMulWide;
        case LowerInst::X86MaskAnd:
            return ((LowerInstX86MaskAnd*)inst)->isComplemented() ? OpVAndNot : OpVAnd;

        // Its own opcode rather than one of `OpVShuffle`'s forms: what selects between its two rows
        // is the lane *kind*, where every other shuffle row is selected by a pattern, and an opcode
        // whose forms are chosen two different ways is one whose selection has to ask which kind of
        // shuffle it is looking at before it can ask anything else.
        case LowerInst::X86Permute:
            return OpVPermute;

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
// that has to be kept in step with the operand list. foldLoads in transform_address.cpp is what puts it
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

        /*
         * And the one thing a *function* has a say in - see `legacyVectorEncodings` in target.h.
         *
         * A function that holds an instruction with no VEX spelling is encoded without one
         * throughout, because alternating the two inside a loop costs far more than either
         * encoding buys. Only the prefixed alternatives are refused: the scalar ones a level also
         * brings - `shlx` off `shl cl`, `mulx` off `mul` - name general registers and are no part
         * of this.
         */
        if(legacyVectorEncodings() && form.encoding.prefixEncoding != PrefixEncoding::Legacy) {
            return id;
        }

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


static MachineFormId selectFormForTarget(LowerBase base, LowerInst* inst) {
    if(auto packed = selectPackedForm(base, inst)) return packed;

    switch(inst->kind) {
        case LowerInst::Nop:        return FormNop;
        // One form, and the only one this kind ever has: it is a whole instruction with no operands,
        // so there is nothing for `selectPackedForm` above to have chosen between.
        case LowerInst::VZeroUpper: return FormVZeroUpper;
        case LowerInst::Arg:        return FormArg;
        case LowerInst::Phi:        return FormPhi;
        case LowerInst::X86Address: return FormAddress;
        case LowerInst::X86Lea:     return FormLea;

        /*
         * The narrow sign-extension, whose form is the width it reads - see LowerInst::X86Sext.
         *
         * Four bytes is `movsxd`, which exists only as a 32-to-64 encoding, so `selectSignExtends`
         * builds one only at a 64-bit result. Asserted rather than checked: a four-byte source at a
         * 32-bit result would be a copy that the pass has no reason to write, and emitting `movsxd`
         * for it would silently sign-extend a value already as wide as its register.
         */
        case LowerInst::X86Sext:
            switch(((LowerInstX86Sext*)inst)->sourceBytes()) {
                case 1: return FormSext8;
                case 2: return FormSext16;
                default:
                    assertTrue(is64Bit(((LowerInstX86Sext*)inst)->result.type)); // no 32-to-32 movsxd
                    return FormSext32;
            }

/*
         * The three BMI2 operations, which never reach selection: `expandBitOperations` has replaced
         * each of them with an intrinsic or with the arithmetic network it stands for, long above
         * here. Asserted rather than answered, for `expandRoundAway`'s reason - a case that answered
         * would be a form for an operation this machine has no opcode for.
         */
        case LowerInst::BitsUpTo:
        case LowerInst::GatherBits:
        case LowerInst::ScatterBits:
            assertTrue("a bit operation reached selection unexpanded" == nullptr);
            return FormNop;

        // The checksum step, at the width its operands are - the resolve verifier has already
        // refused every other one.
        case LowerInst::Crc32:
            return is64Bit(((LowerInstBinary*)inst)->result.type) ? FormCrc32_64 : FormCrc32_32;

        // The BMI1 pair-replacements, one form each. The lowest-bit family is three forms of one
        // opcode selected by the instruction's own field, exactly as `X86MinMax` is.
        case LowerInst::X86AndNot: return FormAndNot;
        case LowerInst::X86LowBit:
            switch(((LowerInstX86LowBit*)inst)->getLowBit()) {
                case LowerX86LowBit::Clear:   return FormLowBitClear;
                case LowerX86LowBit::Isolate: return FormLowBitIsolate;
                default:                      return FormLowBitMask;
            }

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
        case LowerInst::X86MulWide:
        case LowerInst::X86MaskAnd:
        case LowerInst::X86Permute:
        case LowerInst::Abs:
        case LowerInst::Sqrt:
        case LowerInst::Fma:
        case LowerInst::Trunc:
        case LowerInst::Floor:
        case LowerInst::Ceil:
        case LowerInst::Round:
        case LowerInst::ShaBinary:
        case LowerInst::Sha256Rounds:
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

        // One form at both widths, the operand size being the whole of what separates them - and
        // there is no narrow row to pick, `bswap r16` being undefined on this architecture and no
        // 16-bit swap reaching the backend.
        case LowerInst::Bswap:
            requireIntLike(base[((LowerInstUnary*)inst)->from]->type);
            return FormBswap;

        // The access width is the register's, `selectByteSwapMemory` having refused a narrower one:
        // what a reversal reverses is the whole of its operand, so a load of fewer bytes than the
        // reversal reads is a different value rather than a narrower form.
        case LowerInst::X86MovbeLoad:
            return ((LowerInstX86MovbeLoad*)inst)->getWidth() == 4 ? FormMovbeLoad32 : FormMovbeLoad64;

        case LowerInst::X86MovbeStore:
            return ((LowerInstX86MovbeStore*)inst)->getWidth() == 4 ? FormMovbeStore32 : FormMovbeStore64;

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
        case LowerInst::Sar:
        case LowerInst::Rol:
        case LowerInst::Ror: {
            // Indexed by the distance from `Shl`, which is why the two rotations sit immediately
            // after `Sar` in the kind enum and why the note on them there says so.
            static const struct { MachineFormId imm, one, cl; } shifts[] = {
                { FormShlImm, FormShlOne, FormShlCl },
                { FormShrImm, FormShrOne, FormShrCl },
                { FormSarImm, FormSarOne, FormSarCl },
                { FormRolImm, FormRolOne, FormRolCl },
                { FormRorImm, FormRorOne, FormRorCl },
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
         * The in-place update, whose six forms sit contiguously per operation - so the operation
         * chooses the block and the width and the value choose the row within it.
         *
         * The immediate rows exist at the two widths a group-1 immediate has, and are taken on the
         * same terms the store's are: a constant `canEmbedImm` has already accepted has no register
         * to fall back to, and both rows declare the `Imm8OrImm32` that question was asked for.
         */
        case LowerInst::X86StoreOp: {
            auto update = (LowerInstX86StoreOp*)inst;
            auto first = storeUpdateOpFor(update->getOp())->firstForm;
            auto width = update->getWidth();

            if(width >= 4 && isImm(base[update->value])
               && fitsImmediate(ImmediateWidth::Imm8OrImm32, embeddedValue(base, update->value)))
            {
                return MachineFormId(first + (width == 4 ? 4 : 5));
            }

            switch(width) {
                case 1: return first;
                case 2: return MachineFormId(first + 1);
                case 4: return MachineFormId(first + 2);
                default: return MachineFormId(first + 3);
            }
        }

        /*
         * A block operation that reached form selection is one the expansion left alone - a count
         * that is not a constant, or one past the ceiling that makes straight-lining it worth doing
         * (`expandBlockOperations`). Both take the string instruction, whose operands are ordinary
         * fixed registers, so there is nothing left to choose here.
         */
        case LowerInst::Copy: return FormBlockCopyRep;
        case LowerInst::SetPattern: return FormBlockSetRep;

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
