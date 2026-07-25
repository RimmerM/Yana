#include "target.h"

/*
 * The AMD64 register description.
 *
 * Data, built once and shared by every function. Nothing here reads the IR or the settings: what
 * registers exist, which are allocatable and which classes they can serve is a property of the
 * target, and the two things that vary per function - whether rbp is a frame pointer, and whether
 * scratch registers are held back - are subtracted by the allocator from the allocatable set rather
 * than described differently here.
 */

// Every register of a bank, as a mask. Written from the count rather than register by register so
// that a bank with 32 registers is one edit rather than sixteen.
static RegisterUnitMask fullBank(Size count) {
    return count >= 64 ? ~RegisterUnitMask(0) : (RegisterUnitMask(1) << count) - 1;
}

RegSet allRegs() {
    RegSet set;
    set.banks[BankGpr] = fullBank(kGprCount);
    set.banks[BankVector] = fullBank(kVectorCount);
    return set;
}

RegSet allocatableRegs() {
    auto set = allRegs();
    set.remove(stackPointerReg());
    return set;
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
    vectorBank.physicalCount = U16(kVectorCount);

    for(Size i = 0; i < kGprCount; i++) {
        if(!gprBank.reserved.has(gpr(i))) gprBank.allocatable.add(gpr(i));
    }

    for(Size i = 0; i < kVectorCount; i++) vectorBank.allocatable.add(vectorReg(i));

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

    // The three vector classes likewise overlap completely - xmm3, ymm3 and zmm3 are one physical
    // register - so a value in any of them interferes with a value in either other.
    describe(ClassXmm128, BankVector, StackSlotClass::Slot128);
    describe(ClassYmm256, BankVector, StackSlotClass::Slot256);
    describe(ClassZmm512, BankVector, StackSlotClass::Slot512);

    // Every class belongs to a bank that exists, names only registers that exist, and names no
    // register the bank reserved. Checked here rather than trusted, because a class that could name
    // rsp would be an allocator that hands out the stack pointer.
    for(auto& cls: classes) {
        auto& bank = banks[cls.bank];
        assertTrue(cls.bank < kRegisterBankCount);
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

PhysicalReg spillTemp(RegisterBankId bank, Size index) {
    assertTrue(index < kTotalSpillTemps);

    auto count = targetRegisters().bank(bank).physicalCount;
    return PhysicalReg { bank, U16(count - 1 - index) };
}

PhysicalReg moveTemp(RegisterBankId bank, Size index) {
    assertTrue(index < kMoveTemps);
    return spillTemp(bank, kMaxSpillTemps + index);
}

RegSet spillTempRegs() {
    RegSet set;

    for(Size i = 0; i < kTotalSpillTemps; i++) {
        for(Size bank = 0; bank < kRegisterBankCount; bank++) {
            set.add(spillTemp(RegisterBankId(bank), i));
        }
    }

    return set;
}
