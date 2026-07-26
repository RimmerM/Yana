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

    // The three vector classes likewise overlap completely - xmm3, ymm3 and zmm3 are one physical
    // register - so a value in any of them interferes with a value in either other.
    describe(ClassXmm128, BankVector, StackSlotClass::Slot128);
    describe(ClassYmm256, BankVector, StackSlotClass::Slot256);
    describe(ClassZmm512, BankVector, StackSlotClass::Slot512);

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

TemporaryReserve TemporaryReserve::widest() {
    TemporaryReserve out;

    for(Size bank = 0; bank < kRegisterBankCount; bank++) {
        out.operandTemps[bank] = U8(kMaxOperandTemps);
        out.moveTemps[bank] = U8(kMaxMoveTemps);
    }

    return out;
}

PhysicalReg TemporaryReserve::operandTemp(RegisterBankId bank, Size index) const {
    return topOfBank(bank, index);
}

PhysicalReg TemporaryReserve::moveTemp(RegisterBankId bank, Size index) const {
    return topOfBank(bank, operandTemps[bank] + index);
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
