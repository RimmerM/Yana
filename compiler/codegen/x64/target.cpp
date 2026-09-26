#include "target.h"
#include "../../compiler/settings.h"

/*
 * The AMD64 register description.
 *
 * Data, built once and shared by every function. Nothing here reads the IR or the settings: what
 * registers exist, which are allocatable and which classes they can serve is a property of the
 * target, and the two things that vary per function - whether rbp is a frame pointer, and whether
 * scratch registers are held back - are subtracted by the allocator from the allocatable set rather
 * than described differently here.
 */

// What this backend claims about AMD64 itself: x86-64-v2, which is the floor every target
// description here is written against. See kFeatureBaseline, and TargetExtensions for why the floor
// is a requirement rather than a default.
static constexpr FeatureSet kBaselineFeatures = kFeatureBaseline;

static FeatureSet gTargetFeatures = kBaselineFeatures;
static bool gLegacyVectorEncodings = false;

FeatureSet targetFeatures() {
    return gTargetFeatures;
}

// See the note in target.h. False by default, which is what every function that holds no
// legacy-only form gets and is what this backend did before there was such a form.
bool legacyVectorEncodings() {
    return gLegacyVectorEncodings;
}

void setLegacyVectorEncodings(bool legacy) {
    gLegacyVectorEncodings = legacy;
}

void setTargetFeatures(FeatureSet features) {
    // Nothing to check against the register description any more: there are two of them, and which
    // one `targetRegisters` answers follows the features - see there. A change between functions is
    // what the test drivers do all day, compiling one fixture at `sse4` and the next at `avx512`.
    gTargetFeatures = features;
}

/*
 * The block-expansion policy - see BlockExpansion.
 *
 * **The ceiling is two bounds met, and neither of them alone is the one that matters.** A count of
 * transfer *pairs* is what bounds the instructions a call site grows by, and it is the number worth
 * holding steady across the two step widths; a count of *bytes* is what bounds how far past
 * `rep movsb`'s flat rate a straight line is worth going, and that number does not scale with the
 * register. So a level names both and the smaller applies - which is what keeps a v2 build at
 * `Speed` from spending sixteen `movdqu` pairs to do what eight `vmovdqu` pairs do one level up.
 *
 * `Size` comes to thirty-two bytes at every level, which is exactly what this backend straight-lined
 * before the policy existed. That is deliberate: a build asking for smaller code should not get
 * larger code out of this change, and the *width* of the transfers is a size win by itself - thirty-
 * two bytes is one `vmovdqu` pair where it used to be four `mov` pairs.
 *
 * The step is `targetVectorBytes`' answer and not a reading of the feature set, which is what keeps
 * this a pure function of the settings: the two agree by construction, since both are the level.
 *
 * `legacyVectors` is the one thing a *function* has a say in, and it is a parameter rather than a
 * reading of `legacyVectorEncodings()` for the reason the struct's own note gives - see
 * `expandBlockOperations`, which passes `LowerFunction::legacyVectors` and so reads no target state
 * to get it.
 */
BlockExpansion x64BlockExpansionFor(const CompileSettings& settings, bool legacyVectors) {
    // The widest register a transfer can go through. Capped at thirty-two because this backend holds
    // a vector in at most a `ymm` - see targetVectorBytes, which says the same thing about a value.
    auto step = min(U32(32), targetVectorBytes(settings));

    /*
     * And capped again at sixteen inside a function encoded without a vector prefix.
     *
     * A wide step is a wide *value* - `typeForStep` makes anything from sixteen bytes up a packed
     * type - and a wide vector has no legacy spelling to be chosen against, so a thirty-two byte
     * transfer is a `vmovdqu` whatever `packedNeedsVex` answers. Two of them beside the unprefixed
     * arithmetic they were expanded next to is exactly the crossing `@x86_legacy_sse` exists to
     * stop, in the same register and a few instructions apart, which is the 140x case rather than
     * the entry one - see `legacyVectorEncodings` above.
     *
     * **Capped here rather than reported by `checkLegacyVectorEncoding`**, which cannot see it: that
     * check runs before `kTransformPipeline` and this value does not exist yet, and moving it after
     * would only buy a diagnostic that names the wrong thing. The author wrote a struct assignment
     * and there is no wide type in their source to narrow. At sixteen the expansion is the
     * `movdqu` pair it should always have been in such a body, and there is nothing left to report.
     *
     * The ceiling follows, because `limit` is computed from the step below: a level that spends four
     * pairs spends them on half as many bytes here, and a copy past the smaller ceiling is
     * `rep movsb` - general registers, and no encoding question to answer.
     */
    if(legacyVectors) step = min(step, U32(16));

    /*
     * `Balanced` is what `Speed` is: eight pairs and 256 bytes, so 128 at v2 and 256 at v3 - which is
     * also LLVM's own line (`MaxStoresPerMemcpy` is eight at -O2). It was four pairs and 128 bytes,
     * and the difference was measured rather than argued (J181): on the Zen 5 this was built on,
     * `rep movsb` over a 128-256 byte record costs 14-15 ns a copy where the eight-pair line costs
     * 1.3-2.5, so "about thirty cycles" is an underestimate by half and the ceiling was a factor of
     * six on every copy between the old one and this. The corpus carried twenty-one such copies,
     * 92 and 96 bytes at v2 in iterator steps and drops. `Speed` is left where it was: nothing past
     * 256 bytes has been measured.
     */
    U64 pairs = 8, bytes = 256;

    if(settings.inlining == InlineLevel::None || settings.inlining == InlineLevel::Size) {
        pairs = 2;
        bytes = 32;
    }

    auto limit = min(bytes, U64(step) * pairs);

    return BlockExpansion {
        .copyLimit = limit,
        .copyStep = step,
        .setLimit = limit,
        .setStep = step,
    };
}

/*
 * The level, as the bits the form table is written in terms of.
 *
 * One switch and no implications drawn on the way: what used to be four separate claims - SSE4.1
 * from the ladder, AVX from a rung above it, FMA3 from a flag beside it, BMI1 from a rule about
 * which parts exist - is a level saying what a level means. The bits stay separate below this line
 * because a form's requirement is documentation (see kFeatureBaseline); they are simply never set
 * apart from each other any more.
 *
 * There is no `explicitExtensions` test here and no host detection either. The level is settled by
 * the time this is asked - a build that named one has it, and a build that named none has the one
 * its host meets - so this reads a decision rather than making one.
 */
FeatureSet x64FeaturesFor(const CompileSettings& settings) {
    auto features = kBaselineFeatures;
    auto& extensions = settings.extensions;

    if(extensions.level >= TargetExtensions::Avx2) {
        features |= kFeatureAvx | kFeatureAvx2 | kFeatureBmi1 | kFeatureBmi2 | kFeatureFma3
                  | kFeatureMovbe | kFeatureLzcnt;
    }

    // The whole of the top level - every instruction set the level brings, which Analysis-Avx512.md
    // §1.2's stage 1 built at the width `avx2` has and its stage 2 at 512 bits. See
    // `vectorRegisterCountFor` for what that makes of the register file.
    if(extensions.level >= TargetExtensions::Avx512) features |= kFeatureAvx512;

    // And the flags, asked through `has` so that a flag the level implies and a flag the build named
    // are one bit - see TargetExtensions::flags.
    static const struct { InstructionSets set; FeatureSet feature; } flags[] = {
        { kSetSha, kFeatureSha }, { kSetAes, kFeatureAes }, { kSetPclmul, kFeaturePclmul },
        { kSetVaes, kFeatureVaes }, { kSetVpclmul, kFeatureVpclmul }, { kSetGfni, kFeatureGfni },
    };

    for(auto& flag: flags) {
        if(extensions.has(flag.set)) features |= flag.feature;
    }

    return features;
}

// Every register of a bank, as a mask. Written from the count rather than register by register so
// that a bank with 32 registers is one edit rather than sixteen.
static RegisterUnitMask fullBank(Size count) {
    return count >= 64 ? ~RegisterUnitMask(0) : (RegisterUnitMask(1) << count) - 1;
}

// The union of what the *bank descriptions* say rather than a second statement of which registers
// exist and which are held back. That matters for a bank whose reservations are not rsp: a mask
// bank cannot hand out k0, and an extended vector bank has sixteen registers no encoding can name
// without EVEX. Deriving it here means a bank that reserves something new is one edit in the
// constructor below rather than two that can disagree.
RegSet allocatableRegs() {
    RegSet set;
    for(auto& bank: targetRegisters().banks) set |= bank.allocatable;
    return set;
}

/*
 * How large the feature-dependent banks are.
 *
 * The general registers are the architecture's and do not vary. The other two are the ISA level's:
 * EVEX is what makes xmm16-31 nameable at all and what introduces the mask registers, so without it
 * the upper half of the vector bank and the whole of the mask bank are registers this machine does
 * not have - not registers it merely cannot encode.
 *
 * **With it, both are the machine's whole file** - Analysis-Avx512.md §1.2's stage 1. The cap this
 * replaced held them at sixteen and zero because a register the allocator hands out has to be
 * nameable by every form a value in it reaches, and most forms were legacy or VEX. That is still true,
 * and is now answered per *web* rather than per bank: a form that cannot name xmm16 says so
 * (`MachineForm::namesUpperVectors`), and the webs it reads and writes stay in the lower sixteen,
 * while everything else is encoded with whichever prefix its operands turn out to need (§1.1, see
 * `writeVectorPrefix` in gen.cpp).
 */

Size vectorRegisterCountFor(FeatureSet features) {
    return (features & kFeatureAvx512f) ? 32 : 16;
}

Size maskRegisterCountFor(FeatureSet features) {
    return (features & kFeatureAvx512f) ? 8 : 0;
}

Size vectorRegisterCount() {
    return vectorRegisterCountFor(targetFeatures());
}

RegSet upperVectorRegisters() {
    RegSet set;
    for(Size i = 16; i < vectorRegisterCount(); i++) set.add(vectorReg(i));
    return set;
}

/*
 * Whether a mask lives in the mask bank on this target, or in the vector bank as the vector of all-ones
 * lanes a comparison without AVX-512 produces.
 *
 * **Not the same question as whether the mask bank exists**, which is what this used to be read off:
 * the registers arrive with the level, and whether a function's masks may live in them is a question
 * about the function as well - one encoded without a vector prefix cannot name a mask register at
 * all. Analysis-Avx512.md §2.1 is the design: a mask's *memory* form is the vector at every level,
 * and where a live one is held is this backend's business alone.
 */
bool maskInMaskBank() {
    // The mask registers are EVEX's, the 32-bit operations on them are BW and the crossings to and
    // from a vector are DQ - every one of which the level has. A function encoded without a vector
    // prefix has none of it, which is `@x86_legacy_sse`'s promise (see legacyVectorEncodings).
    auto needed = kFeatureAvx512f | kFeatureAvx512bw | kFeatureAvx512dq | kFeatureAvx512vl;
    return (targetFeatures() & needed) == needed && !legacyVectorEncodings();
}

TargetRegisters::TargetRegisters(FeatureSet features) {
    auto& gprBank = banks[BankGpr];
    gprBank.id = BankGpr;
    gprBank.name = "gpr"_v;
    gprBank.physicalCount = U16(kGprCount);
    gprBank.reserved.add(stackPointerReg());

    auto& vectorBank = banks[BankVector];
    vectorBank.id = BankVector;
    vectorBank.name = "vector"_v;
    vectorBank.physicalCount = U16(vectorRegisterCountFor(features));

    auto& maskBank = banks[BankMask];
    maskBank.id = BankMask;
    maskBank.name = "mask"_v;
    maskBank.physicalCount = U16(maskRegisterCountFor(features));

    // k0 is never handed out. It exists and can be written, but every masked encoding reads the
    // field naming it as "no mask at all" - so a value living there could not be used as the mask it
    // was allocated to be. Reserving it in the bank is the same statement rsp is: a register the
    // target has and never gives to a value. A form that wants to exclude it for its *own* reason -
    // a mask operand that may not be k0 where an unmasked one may - narrows the class instead, and
    // that is the operand-subset constraint validateMachineForms still fences off.
    if(maskBank.physicalCount > 0) maskBank.reserved.add(maskReg(0));

    // A bank hands out every register it has that it did not reserve. Derived rather than written out,
    // so that reserving one is a single line above and the two sets cannot come to disagree.
    for(auto& bank: banks) {
        for(Size i = 0; i < bank.physicalCount; i++) {
            auto reg = PhysicalReg { bank.id, U16(i) };
            if(!bank.reserved.has(reg)) bank.allocatable.add(reg);
        }
    }

    auto describe = [&](RegisterClassId id, RegisterBankId bank, StackSlotClass spill) {
        auto& cls = classes[id];
        cls.id = id;
        cls.bank = bank;
        cls.spillClass = spill;
        cls.allowedPhysical = banks[bank].allocatable;
    };

    // GPR32 and GPR64 are two classes over one bank: they name the same registers and differ only in
    // the width an encoder writes and the slot a spill needs.
    describe(ClassGpr32, BankGpr, StackSlotClass::Slot32);
    describe(ClassGpr64, BankGpr, StackSlotClass::Slot64);

    // The five vector classes overlap completely - a scalar double, xmm3, ymm3 and zmm3 are all one
    // physical register - so a value in any of them interferes with a value in every other. The two
    // scalar ones are what the lowering's floats occupy; the packed ones are what the register model
    // already describes and no IR type produces yet.
    describe(ClassFloat32, BankVector, StackSlotClass::Slot32);
    describe(ClassFloat64, BankVector, StackSlotClass::Slot64);
    describe(ClassXmm128, BankVector, StackSlotClass::Slot128);
    describe(ClassYmm256, BankVector, StackSlotClass::Slot256);
    describe(ClassZmm512, BankVector, StackSlotClass::Slot512);

    // The mask classes name k1-k7, or nothing at all on a target without EVEX. Both are the same
    // registers at the two widths `kmov` distinguishes.
    describe(ClassMask32, BankMask, StackSlotClass::Slot32);
    describe(ClassMask64, BankMask, StackSlotClass::Slot64);

    // A bank names only registers of its own, which is what lets everything else iterate a bank's
    // allocatable set and take the bank from the registers it yields.
    for(auto& bank: banks) {
        for(Size i = 0; i < kRegisterBankCount; i++) {
            if(RegisterBankId(i) == bank.id) continue;
            assertTrue(bank.allocatable.banks[i] == 0);
            assertTrue(bank.reserved.banks[i] == 0);
        }
    }

    // Every class belongs to a bank that exists, names only registers that exist, and names no
    // register the bank reserved. Checked here rather than trusted, because a class that could name
    // rsp would be an allocator that hands out the stack pointer. The range check comes first: it is
    // what makes indexing `banks` below defined at all.
    for(auto& cls: classes) {
        assertTrue(cls.bank < kRegisterBankCount);

        auto& bank = banks[cls.bank];
        assertTrue((cls.allowedPhysical.banks[cls.bank] & ~fullBank(bank.physicalCount)) == 0);
        assertTrue((cls.allowedPhysical & bank.reserved).isEmpty());

        for(Size i = 0; i < kRegisterBankCount; i++) {
            if(RegisterBankId(i) != cls.bank) assertTrue(cls.allowedPhysical.banks[i] == 0);
        }
    }
}

/*
 * The register file of the target in force - one of two, built once each.
 *
 * Two descriptions rather than one rebuilt, because what differs between them is not a rule but a
 * count: every mask, every class's `allowedPhysical` and every convention's clobber set is sized by
 * how many registers the banks have, and a function compiled at `avx2` after one compiled at `avx512`
 * must not see a single one of the upper sixteen. Choosing by the features at the point of asking is
 * what `setTargetFeatures` being called once per function makes safe: nothing in this backend holds a
 * description across that call.
 */
const TargetRegisters& targetRegisters() {
    if(targetFeatures() & kFeatureAvx512f) {
        static TargetRegisters evex { kFeatureAvx512f };
        return evex;
    }

    static TargetRegisters plain { kFeatureBaseline };
    return plain;
}

/*
 * Scratch registers. See the block comment in target.h.
 */

// Position `index` counted down from the top of a bank's register file, which is where every
// temporary comes from and the only arithmetic either pool does.
static PhysicalReg topOfBank(RegisterBankId bank, Size index) {
    // The top of the lower sixteen in the vector bank, whatever the level: a temporary is what a form
    // whose operand was left in the frame is handed, and the forms that cannot name xmm16 are exactly
    // the ones a temporary has to serve - see `upperVectorRegisters`.
    auto count = bank == BankVector ? min(Size(16), Size(targetRegisters().bank(bank).physicalCount))
                                    : Size(targetRegisters().bank(bank).physicalCount);
    assertTrue(index < count); // a reserve larger than the register file it is taken from
    return PhysicalReg { bank, U16(count - 1 - index) };
}

TemporaryReserve TemporaryReserve::widestLike(const TemporaryReserve& like) {
    TemporaryReserve out = like;

    for(Size bank = 0; bank < kRegisterBankCount; bank++) {
        // Never more than the bank has. A bank the enabled ISA level leaves empty - the mask
        // registers without EVEX - can be asked for no scratch at all, because nothing can be
        // placed in it to need one.
        auto count = Size(targetRegisters().bank(RegisterBankId(bank)).physicalCount);
        if(bank == BankVector && count > 16) count = 16; // see topOfBank
        auto operands = count < kMaxOperandTemps ? count : Size(kMaxOperandTemps);

        out.operandTemps[bank] = U8(operands);
        out.moveTemps[bank] = U8(count - operands < kMaxMoveTemps ? count - operands : Size(kMaxMoveTemps));
    }

    return out;
}

// Position `index` of a bank's pool: the register `chooseTemporaryPool` put there, or - for a reserve
// that was never chosen against a function - the same count down from the top of the file this
// always answered.
PhysicalReg TemporaryReserve::operandTemp(RegisterBankId bank, Size index) const {
    if(!chosen) return topOfBank(bank, index);

    assertTrue(index < kMaxTemporaryPool); // a pool position past the two roles' own maximum
    return PhysicalReg { bank, pool[bank][index] };
}

PhysicalReg TemporaryReserve::moveTemp(RegisterBankId bank, Size index) const {
    return operandTemp(bank, operandTemps[bank] + index);
}

RegSet TemporaryReserve::regs() const {
    RegSet set;

    for(Size bank = 0; bank < kRegisterBankCount; bank++) {
        auto id = RegisterBankId(bank);
        for(Size i = 0; i < operandTemps[bank]; i++) set.add(operandTemp(id, i));
        for(Size i = 0; i < moveTemps[bank]; i++) set.add(moveTemp(id, i));
    }

    return set;
}

bool TemporaryReserve::growTo(const TemporaryReserve& other) {
    auto grew = false;

    for(Size i = 0; i < kRegisterBankCount; i++) {
        if(other.operandTemps[i] > operandTemps[i]) { operandTemps[i] = other.operandTemps[i]; grew = true; }
        if(other.moveTemps[i] > moveTemps[i]) { moveTemps[i] = other.moveTemps[i]; grew = true; }
    }

    return grew;
}

bool TemporaryReserve::shrinkTo(const TemporaryReserve& other) {
    auto shrank = false;

    for(Size i = 0; i < kRegisterBankCount; i++) {
        if(other.operandTemps[i] < operandTemps[i]) { operandTemps[i] = other.operandTemps[i]; shrank = true; }
        if(other.moveTemps[i] < moveTemps[i]) { moveTemps[i] = other.moveTemps[i]; shrank = true; }
    }

    return shrank;
}
