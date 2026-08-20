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

FeatureSet targetFeatures() {
    return gTargetFeatures;
}

void setTargetFeatures(FeatureSet features) {
    // The register description is built once and never rebuilt, so a feature change that would give
    // the machine a different number of registers has to be refused rather than ignored - a bank
    // whose size disagrees with the masks already handed out is an allocator writing outside them.
    // Nothing does this today; the check is what keeps it that way as feature levels are added.
    assertTrue(vectorRegisterCount() == vectorRegisterCountFor(features));
    assertTrue(maskRegisterCount() == maskRegisterCountFor(features));

    gTargetFeatures = features;
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

    if(settings.extensions.level >= TargetExtensions::V3) {
        features |= kFeatureAvx | kFeatureAvx2 | kFeatureBmi1 | kFeatureBmi2 | kFeatureFma3
                  | kFeatureMovbe | kFeatureLzcnt;
    }

    /*
     * AVX-512 is claimed for its *encoding* and not for its register file.
     *
     * EVEX is two things at once: a longer prefix, and sixteen more vector registers plus the mask
     * bank that only that prefix can name. The encoder below writes it, so the first half is here;
     * the second half is not, because a register the allocator may hand out has to be nameable by
     * every form that could receive a value in it, and the legacy and VEX forms cannot name xmm16.
     * Handing out xmm16 while any form is still legacy is an encoding that silently means xmm0.
     *
     * So `vectorRegisterCountFor` holds the bank at sixteen whatever the features say, and lifting
     * it is what makes v4 a width rather than a prefix - it comes with the EVEX forms that make the
     * upper half reachable, and with `targetVectorBytes` answering 64.
     */
    if(settings.extensions.level >= TargetExtensions::V4) features |= kFeatureAvx512f;

    return features;
}

// Every register of a bank, as a mask. Written from the count rather than register by register so
// that a bank with 32 registers is one edit rather than sixteen.
static RegisterUnitMask fullBank(Size count) {
    return count >= 64 ? ~RegisterUnitMask(0) : (RegisterUnitMask(1) << count) - 1;
}

// Both of these are the union of what the *bank descriptions* say rather than a second statement of
// which registers exist and which are held back. That matters for a bank whose reservations are not
// rsp: a mask bank cannot hand out k0, and an extended vector bank has sixteen registers no encoding
// can name without EVEX. Deriving them here means a bank that reserves something new is one edit in
// the constructor below rather than two that can disagree.
RegSet allRegs() {
    RegSet set;
    for(auto& bank: targetRegisters().banks) set |= bank.allocatable | bank.reserved;
    return set;
}

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
 * Both are capped below what the feature bit allows, and the cap is not about the encoder. It is
 * that a register the allocator hands out has to be nameable by *every* form a value in it can
 * reach, and the legacy and VEX forms this backend still selects cannot name xmm16 or a k-register
 * at all. Lifting the cap is stage 5's, together with the EVEX forms that make the upper half
 * reachable; see x64FeaturesFor.
 */

Size vectorRegisterCountFor(FeatureSet) {
    return 16;
}

Size maskRegisterCountFor(FeatureSet) {
    return 0;
}

Size vectorRegisterCount() {
    return vectorRegisterCountFor(targetFeatures());
}

Size maskRegisterCount() {
    return maskRegisterCountFor(targetFeatures());
}

TargetRegisters::TargetRegisters() {
    auto& gprBank = banks[BankGpr];
    gprBank.id = BankGpr;
    gprBank.name = "gpr"_v;
    gprBank.physicalCount = U16(kGprCount);
    gprBank.reserved.add(stackPointerReg());

    auto& vectorBank = banks[BankVector];
    vectorBank.id = BankVector;
    vectorBank.name = "vector"_v;
    vectorBank.physicalCount = U16(vectorRegisterCount());

    auto& maskBank = banks[BankMask];
    maskBank.id = BankMask;
    maskBank.name = "mask"_v;
    maskBank.physicalCount = U16(maskRegisterCount());

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

const TargetRegisters& targetRegisters() {
    static TargetRegisters registers;
    return registers;
}

/*
 * Scratch registers. See the block comment in target.h.
 */

// Position `index` counted down from the top of a bank's register file, which is where every
// temporary comes from and the only arithmetic either pool does.
static PhysicalReg topOfBank(RegisterBankId bank, Size index) {
    auto count = targetRegisters().bank(bank).physicalCount;
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
