#include "machine_internal.h"

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

    /*
     * And the positions a clobber of this form's own would make `takeTemp` step over.
     *
     * ~~The pool is counted from the top of the register file, so a clobber either falls inside it or
     * does not.~~ The pool is chosen per function now (§42), so which registers it holds is not a
     * property of the table this checks - and this check is about the table. So every clobber in the
     * bank is counted as though it fell inside, which is the worst case and the only answer that
     * holds for every function.
     */
    Size clobbers = 0;

    for(Size i = 0; i < Size(registers.bank(bank).physicalCount); i++) {
        if(form.clobbers.has(PhysicalReg { bank, U16(i) })) clobbers++;
    }

    reach += clobbers < kMaxOperandTemps ? clobbers : Size(kMaxOperandTemps);

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

            /*
             * A form that can only be written with a prefix the target may not have is a form that
             * has to say so, or selectForm would pick it on a machine that cannot execute it.
             *
             * The bit-manipulation levels are in the set beside AVX because VEX is not only the
             * vector prefix: `bzhi` and the rest of BMI2 are VEX-encoded and name general registers
             * exclusively, so a form requiring one of those has said everything there is to say
             * about whether the prefix decodes. What is being checked is that *some* claimed
             * extension implies the encoding, not that the operation is a vector one.
             */
            static constexpr FeatureSet kVexBearing =
                kFeatureAvx | kFeatureAvx512f | kFeatureBmi1 | kFeatureBmi2;

            if((form.requiredFeatures & kVexBearing) == 0) {
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
         * A pseudo is exempt for a reason of its own: its bytes are its own emitter's rather than
         * this descriptor's, and the emitters ask `packedNeedsVex` directly - there is nothing for a
         * second form to describe, the operand constraints being identical either way.
         *
         * And a form that declares `legacyOnly`, which is the other exemption and the sharper one:
         * the SHA extension's seven instructions have no VEX encoding *in the architecture*, so a
         * twin of one is a byte sequence that decodes as nothing. See MachineForm::legacyOnly, and
         * the same test in `needsVexTwin`, which is the sweep this rule exists to police.
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
            && !form.legacyOnly
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
            if(original.uses.size() != form.uses.size()) fail(form, "takes a different number of operands than the form it replaces"_v);

            // The flags may differ only where the opcode declared that they do. Which is not a
            // weakening: whether an alternative is taken depends on `targetFeatures()` alone, so the
            // answer is settled before the first pass runs and every reader of it goes through
            // `selectForm` - a stronger guarantee than either of the two MachineOpcodeDesc::
            // flagsSelective describes, and the one the BMI2 rows rely on.
            if(original.flagsEffect != form.flagsEffect && !target.opcodes[form.opcode].flagsSelective) {
                fail(form, "disagrees with the form it replaces about the flags"_v);
            }
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

        // The third writer of that position is the one that *is* an operand - see
        // EncodingDescriptor::immediateByte. It needs the operand it names and it has to fit in the
        // byte it writes, which is what declaring the operand `Imm8` says.
        if(encoding.immediateByte) {
            if(encoding.conditionImmediate || encoding.patternImmediate) {
                fail(form, "ends in an immediate operand as well as a trailing byte of its own"_v);
            }

            if(encoding.immField.isNone()) {
                fail(form, "writes a trailing immediate with no operand to take it from"_v);
            } else if(form.immediateWidth() != ImmediateWidth::Imm8) {
                fail(form, "writes a trailing immediate wider than the one byte it emits"_v);
            }
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
    // folding in transform_peephole.cpp asks what an instruction does to the flags while the peephole passes
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
