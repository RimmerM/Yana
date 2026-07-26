#pragma once

#include "../../lower/lower_inst.h"

/*
 * The target's register description, and what a value can be given.
 *
 * Four things used to be one 16-bit integer: a general register, a vector register, a frame slot and
 * a rematerialization recipe. That made every API describing what an *instruction* does to the
 * machine - a clobber set, a convention's preserved registers - silently accept a frame slot, and
 * every API describing where a *value* lives silently accept a register number from the wrong bank.
 * Both are separated here.
 *
 *   PhysicalReg      a register that exists: a bank and an index within it.
 *   MachineLocation  where a value is: a physical register, a frame slot, a recipe, or nowhere.
 *   RegSet           a set of physical registers. Nothing else can be a member.
 *
 * The three descriptions layered on top of PhysicalReg are what lets one allocator serve register
 * files that are not alike:
 *
 *   bank    a storage family - the integer registers, the vector registers, later the mask
 *           registers. Counts, encodings and preservation policies are per bank, and none of them
 *           may be assumed equal: AVX-512 has 32 vector registers and 16 general ones.
 *   class   which registers, and at which width, a value or an operand may use. GPR32 and GPR64
 *           are two classes over one bank; XMM128, YMM256 and ZMM512 are three over another.
 *   view    one register seen at one class's width - eax and rax, xmm3 and zmm3. This is what the
 *           encoder writes; the allocator only ever chooses the underlying physical register.
 *
 * Interference is over *units* rather than over names, because two views of one register are the
 * same storage: writing eax destroys rax. Today every class covers its register completely, so a
 * unit is a register and the mask has one bit set. The interface does not assume that stays true -
 * explicit upper-half operations would refine the masks without changing allocation.
 */

/*
 * Target features.
 *
 * Which extensions the machine being compiled for has. This is target description rather than
 * instruction description, which is why it lives here: the *register file itself* depends on it -
 * AVX-512 raises the vector bank from sixteen registers to thirty-two and adds the mask bank
 * outright - and so do the forms an encoding may be selected into.
 *
 * A form whose features the target does not have is not selectable, which is checked where the
 * selection is made rather than left to the encoder. See selectForm and verifySelection.
 */

using FeatureSet = U32;

static constexpr FeatureSet kFeatureBaseline = 0;
static constexpr FeatureSet kFeaturePopcnt = 1 << 0;
static constexpr FeatureSet kFeatureRdtscp = 1 << 1;

// VEX encoding: the 128- and 256-bit three-operand vector forms, and the ymm half of the vector bank.
static constexpr FeatureSet kFeatureAvx = 1 << 2;

// EVEX encoding: 512-bit operations, the upper sixteen vector registers, and the mask registers.
static constexpr FeatureSet kFeatureAvx512f = 1 << 3;

// The features this backend is compiling for. A configurable target description replaces this the
// moment there is anything to configure; until then every extension the form table names is assumed
// present, and the point of the check is that adding one that is not cannot be silent.
FeatureSet targetFeatures();

/*
 * Banks.
 */

using RegisterBankId = U8;

enum : RegisterBankId {
    BankGpr = 0,
    BankVector = 1,
    BankMask = 2,

    kRegisterBankCount = 3,
};

// The widest bank the target describes, used only to size the fixed inline arrays that index by
// register. Bank counts themselves are per bank - see RegisterBankDesc::physicalCount - so a bank
// with fewer registers costs nothing beyond the unused entries.
static constexpr Size kMaxRegistersPerBank = 32;

// One bit per physical register within a bank. Interference is the intersection of two of these.
using RegisterUnitMask = U64;

static_assert(kMaxRegistersPerBank <= 64, "a unit mask no longer fits one word");

/*
 * Physical registers.
 */

struct PhysicalReg {
    RegisterBankId bank = BankGpr;
    U16 index = 0;

    bool operator == (PhysicalReg other) const { return bank == other.bank && index == other.index; }
    bool operator != (PhysicalReg other) const { return !(*this == other); }
};

// The AMD64 general-purpose registers, in their encoding order - which is also the order the
// allocator indexes them by.
enum class IntRegister: U8 {
    rax = 0, rcx, rdx, rbx, rsp, rbp, rsi, rdi,
    r8, r9, r10, r11, r12, r13, r14, r15,
};

static constexpr Size kGprCount = 16;

// The vector and mask files are the *ISA level's* rather than the target's, which is the whole
// reason bank sizes are per bank: sixteen vector registers without AVX-512 and thirty-two with it,
// and no mask registers at all until there are. Both are read from targetFeatures() once, when the
// register description is built.
Size vectorRegisterCount();
Size maskRegisterCount();

inline PhysicalReg gpr(IntRegister reg) { return PhysicalReg { BankGpr, U16(reg) }; }
inline PhysicalReg gpr(Size index) { return PhysicalReg { BankGpr, U16(index) }; }
inline PhysicalReg vectorReg(Size index) { return PhysicalReg { BankVector, U16(index) }; }
inline PhysicalReg maskReg(Size index) { return PhysicalReg { BankMask, U16(index) }; }

// The units one physical register occupies. One bit today: every described class covers its whole
// register, so two values in one register always interfere and two in different ones never do.
inline RegisterUnitMask unitsOf(PhysicalReg reg) {
    return RegisterUnitMask(1) << reg.index;
}

/*
 * Register sets.
 *
 * One bitmask per bank rather than one overall, which is what lets a convention state that a call
 * destroys xmm0-15 as well as the caller-saved integer registers. A single mask could only ever
 * describe one bank, which is why no convention could describe a vector clobber before.
 *
 * Only a physical register can be a member. A frame slot or a recipe is not something an instruction
 * can clobber or a convention can preserve, and it is no longer possible to say that it is.
 */
struct RegSet {
    RegisterUnitMask banks[kRegisterBankCount] = {};

    bool has(PhysicalReg reg) const {
        return (banks[reg.bank] & unitsOf(reg)) != 0;
    }

    void add(PhysicalReg reg) {
        banks[reg.bank] |= unitsOf(reg);
    }

    void remove(PhysicalReg reg) {
        banks[reg.bank] &= ~unitsOf(reg);
    }

    bool isEmpty() const {
        for(auto bank: banks) {
            if(bank) return false;
        }

        return true;
    }

    // How many registers are in the set - which the prologue needs as often as it needs the registers
    // themselves, since what it pushes decides where everything below it lands.
    Size count() const {
        Size total = 0;
        for(auto bank: banks) {
            for(auto remaining = bank; remaining; remaining >>= 1) total += remaining & 1;
        }

        return total;
    }

    RegSet& operator |= (const RegSet& other) {
        for(Size i = 0; i < kRegisterBankCount; i++) banks[i] |= other.banks[i];
        return *this;
    }

    RegSet operator | (const RegSet& other) const {
        auto set = *this;
        set |= other;
        return set;
    }

    RegSet operator & (const RegSet& other) const {
        RegSet set;
        for(Size i = 0; i < kRegisterBankCount; i++) set.banks[i] = banks[i] & other.banks[i];
        return set;
    }

    // The registers of `within` that this set does not contain. A convention's preserved set is
    // exactly this applied to its clobber set: a register a call leaves alone is one its callee
    // owes back.
    RegSet complement(const RegSet& within) const {
        RegSet set;
        for(Size i = 0; i < kRegisterBankCount; i++) set.banks[i] = ~banks[i] & within.banks[i];
        return set;
    }

    bool operator == (const RegSet& other) const {
        for(Size i = 0; i < kRegisterBankCount; i++) {
            if(banks[i] != other.banks[i]) return false;
        }

        return true;
    }

    // Every register in the set, in bank and then index order. Iteration is by bank because the
    // banks are independent: nothing may assume they have the same number of registers.
    template<class F>
    void iterate(F&& f) const {
        for(Size bank = 0; bank < kRegisterBankCount; bank++) {
            auto remaining = banks[bank];

            for(Size i = 0; remaining; i++, remaining >>= 1) {
                if(remaining & 1) f(PhysicalReg { RegisterBankId(bank), U16(i) });
            }
        }
    }
};

/*
 * Stack slots.
 *
 * Spill slots are grouped by width so that a slot is reused only by values that fit it exactly,
 * which keeps first-fit reuse and alignment simple and stops a 4-byte value from pinning down
 * 64 bytes of frame.
 */

using StackSlotId = U16;
using RematId = U16;

enum class StackSlotClass: U8 {
    Slot32, Slot64, Slot128, Slot256, Slot512,
};

static constexpr Size kStackSlotClassCount = 5;

inline U32 stackSlotSize(StackSlotClass c) { return 4u << (Size)c; }

/*
 * Register classes and views.
 */

using RegisterClassId = U8;

enum : RegisterClassId {
    ClassGpr32 = 0,
    ClassGpr64,

    // A scalar float in a vector register, which is a *view* of it exactly as eax is a view of rax:
    // the class is four or eight bytes wide, the register underneath is sixteen, and the rest of it
    // holds nothing this value cares about. They are classes of their own rather than a narrow use
    // of ClassXmm128 because a class is what says which instruction a copy of it is - `movss`,
    // `movsd` and `movups` differ in a mandatory prefix - and because a class's spill slot is then
    // its own width with nothing to reconcile.
    ClassFloat32,
    ClassFloat64,

    ClassXmm128,
    ClassYmm256,
    ClassZmm512,

    ClassMask32,
    ClassMask64,

    kRegisterClassCount,
};

// Which registers, at which width, a value of this class may occupy.
struct RegisterClassDesc {
    RegisterClassId id = ClassGpr32;
    RegisterBankId bank = BankGpr;

    // The physical registers of the bank this class can name. A form may restrict it further - an
    // encoding limited to the legacy vector registers, or a mask operand that cannot be k0.
    RegSet allowedPhysical;

    // The slot a value of this class needs, which is the class's own width and not the underlying
    // register's: a scalar double takes eight bytes of frame however wide the xmm register holding
    // it is. Preserving a *register* entire - which is what a callee-saved vector register needs -
    // is a separate question, and the frame answers it with the bank's full width rather than with
    // the class of whatever happened to be in it.
    StackSlotClass spillClass = StackSlotClass::Slot64;
};

// One physical register seen at one class's width - `eax` as against `rax`, `xmm3` as against
// `zmm3`. The allocator chooses the physical register; the encoder chooses the view from the
// selected form and the operand's type.
struct RegisterView {
    RegisterClassId regClass = ClassGpr32;
    PhysicalReg physical;

    // The units the view touches, which is what interference is computed over.
    RegisterUnitMask units = 0;

    // The number that goes into the instruction. Identical to the register index for every class
    // AMD64 describes; it is separate because nothing generic should assume that.
    U8 encoding = 0;
};

struct RegisterBankDesc {
    RegisterBankId id = BankGpr;
    StringView name;
    U16 physicalCount = 0;

    // The registers a value can actually be given, and the ones that exist but never will be.
    RegSet allocatable;
    RegSet reserved;
};

// The target description, built once and shared. Constant for every function.
struct TargetRegisters {
    TargetRegisters();

    RegisterBankDesc banks[kRegisterBankCount];
    RegisterClassDesc classes[kRegisterClassCount];

    const RegisterBankDesc& bank(RegisterBankId id) const { return banks[id]; }
    const RegisterClassDesc& regClass(RegisterClassId id) const { return classes[id]; }

    RegisterView viewOf(RegisterClassId cls, PhysicalReg reg) const {
        assertTrue(classes[cls].bank == reg.bank); // a view of a register from another bank
        return RegisterView { cls, reg, unitsOf(reg), U8(reg.index) };
    }
};

const TargetRegisters& targetRegisters();

/*
 * Types to classes.
 */

// The bank a value of this type lives in.
inline RegisterBankId bankForType(LowerType type) {
    return isIntLike(type) ? BankGpr : BankVector;
}

// The class a value of this type occupies. Every scalar the lowering produces is at most eight bytes
// wide, so a float takes the scalar view of a vector register rather than the whole of one; the
// packed classes exist for the vector values the register model describes and the IR has no type for
// yet.
inline RegisterClassId classForType(LowerType type) {
    if(isIntLike(type)) return type == LowerType::Int32 ? ClassGpr32 : ClassGpr64;
    return type == LowerType::Float32 ? ClassFloat32 : ClassFloat64;
}

// The slot class a value of this type needs when it does not get a register - its own width, which
// is its register class's. Asked by the allocator when it spills a value and by the memory-operand
// rules when they decide whether an instruction may read one in place, which have to agree: a slot
// is exactly as wide as the value in it, and an access of any other width would take a neighbouring
// value with it.
inline StackSlotClass stackSlotClassFor(LowerType type) {
    return type == LowerType::Int32 || type == LowerType::Float32
        ? StackSlotClass::Slot32
        : StackSlotClass::Slot64;
}

/*
 * Locations.
 *
 * Where a value is between the instructions that touch it. A frame slot and a recipe are locations
 * but not places in the register file, which is exactly the distinction that used to be missing:
 * neither can be a member of a RegSet, and neither can be handed to an encoder.
 */

enum class LocationKind: U8 {
    Invalid,
    Physical,
    StackSlot,
    Rematerializable,
};

struct MachineLocation {
    LocationKind kind = LocationKind::Invalid;
    RegisterBankId bank = BankGpr; // Physical only
    U16 index = 0;                 // register index, slot id or recipe id, by kind

    static MachineLocation invalid() { return MachineLocation {}; }

    static MachineLocation physical(PhysicalReg reg) {
        return MachineLocation { LocationKind::Physical, reg.bank, reg.index };
    }

    static MachineLocation stack(StackSlotId slot) {
        return MachineLocation { LocationKind::StackSlot, BankGpr, slot };
    }

    static MachineLocation remat(RematId recipe) {
        return MachineLocation { LocationKind::Rematerializable, BankGpr, recipe };
    }

    bool isValid() const { return kind != LocationKind::Invalid; }

    // Somewhere an encoder can read a value out of directly. A slot and a recipe both answer no, and
    // for the same reason: an instruction that cannot address one has to be given a register with
    // the value brought into it.
    bool isPhysical() const { return kind == LocationKind::Physical; }

    // A frame slot rather than a register: it has an address instead of a number, and only the frame
    // layout knows what that address is.
    bool isStack() const { return kind == LocationKind::StackSlot; }

    // A recipe rather than a place: the value is recreated wherever it is needed and is not stored
    // anywhere in between. See Remat in gen.h.
    bool isRemat() const { return kind == LocationKind::Rematerializable; }

    PhysicalReg physicalReg() const {
        assertTrue(isPhysical()); // a location that is not a register was read as one
        return PhysicalReg { bank, index };
    }

    StackSlotId stackSlot() const {
        assertTrue(isStack());
        return StackSlotId(index);
    }

    RematId rematId() const {
        assertTrue(isRemat());
        return RematId(index);
    }

    bool operator == (MachineLocation other) const {
        return kind == other.kind && bank == other.bank && index == other.index;
    }

    bool operator != (MachineLocation other) const { return !(*this == other); }
};

/*
 * The registers this target holds back.
 */

// rsp is never handed out: push/pop/call/ret need it to be the stack pointer, in every function.
inline PhysicalReg stackPointerReg() { return gpr(IntRegister::rsp); }

// Every register of each bank, and the ones a value can actually be given. Anything outside the
// allocatable set either does not exist or is rsp, so a calling convention's preserved set is stated
// relative to it: the registers a function has to give back are exactly the ones it could have taken
// in the first place.
RegSet allRegs();
RegSet allocatableRegs();

/*
 * The frame pointer.
 *
 * rbp is allocatable like any other register in a function that does not establish a frame pointer,
 * and reserved for the whole function in one that does. Which it is is decided from the IR before
 * allocation starts - see functionNeedsFramePointer - because both the allocator and frame layout
 * have to be working from the same answer: a value living in rbp while the frame is addressed
 * through it is silent memory corruption, not a missed optimization.
 *
 * Two consequences elsewhere follow from rbp being an ordinary register here.
 *
 * It is callee-saved under every convention, and has to be: a caller may be holding its frame
 * pointer in rbp, and nothing in the IR represents that for the allocator to rescue at a call. The
 * convention tables therefore leave rbp out of every clobber set, including Clobber's, and `finish`
 * derives it into every preserved set from allocatableRegs above. A function that takes rbp pays a
 * push and a pop for it, which is why it is the last register the allocator reaches for.
 *
 * And a function that keeps a value in rbp has no frame-pointer chain, so a backtracer that walks
 * one cannot pass through it. That is the ordinary consequence of omitting a frame pointer at all,
 * which `FramePointerMode::Needed` already does; `all` and `non-leaf` are the way to ask for the
 * chain back, and they reserve rbp again as a side effect of asking.
 */
inline PhysicalReg framePointerReg() { return gpr(IntRegister::rbp); }

inline RegSet framePointerRegs() {
    RegSet set;
    set.add(framePointerReg());
    return set;
}

/*
 * Scratch registers.
 *
 * A value living in the frame cannot be read by every encoder, so it is brought into a register at
 * the instructions that cannot address it where it is. Those registers have to come from somewhere,
 * and they cannot be found after the fact: a register handed to a web is one legalization can no
 * longer borrow, so whatever it is going to need has to be held back before placement runs.
 *
 * So a function is placed once with nothing held back. If nothing went homeless, that is the answer
 * and the common case has paid nothing. If something did, the reserve legalizing that placement will
 * need is *measured* - by legalizing it and recording what it asked for - and the function is placed
 * again with that much held back. The measurement cannot drift from the pass that spends the reserve,
 * because it is that pass.
 *
 * The reserve is per bank and derived rather than fixed. It used to be the top five registers of
 * every bank after any spill at all, which charged a function that left one integer value in the
 * frame for the two temporaries a vector move cycle would have wanted and for three operand
 * temporaries no instruction in it could ask for.
 *
 * Two pools, because both can be live at one instruction: the operand pool serves the operands and
 * results of the instruction being legalized, and the move pool serves the parallel copies around it
 * - a cycle that has to go through a register, and a transfer with a frame slot at both ends.
 *
 * A third pool belongs here as soon as a form declares temporaries for its own expansion (see
 * MachineForm::temporaries, which validateMachineForms rejects until then). The unrolled block
 * operation's scratch is not one of those: it is a fixed physical register the form states as a
 * clobber, which keeps a live value out of it at that one instruction rather than for the whole
 * function, and is the cheaper of the two ways to say it.
 *
 * Temporaries are taken from the top of the register file on purpose: r11-r15 are outside every
 * described convention's argument and result registers, so a scratch can never collide with a fixed
 * register the same instruction is also placing.
 */

// The most operand temporaries one instruction can ask for of one bank, which is a property of the
// widest form described: two unconstrained operands and a result sharing with neither. A
// fixed-register operand needs none, being loaded straight into the register the instruction demands.
//
// This is not the reserve - it is the limit the measurement is checked against, so that a form
// wanting more is a loud failure rather than two temporaries quietly naming one register.
static constexpr Size kMaxOperandTemps = 3;

// The two the move sequencer can want at once: one to break a cycle that has no exchange to use, and
// one to carry a transfer whose ends are both frame slots.
static constexpr Size kMaxMoveTemps = 2;

struct TemporaryReserve {
    U8 operandTemps[kRegisterBankCount] = {};
    U8 moveTemps[kRegisterBankCount] = {};

    // The widest pools any one instruction can ask for. Held during the measuring pass only, so that
    // measuring hands out a distinct register per temporary: two temporaries naming one register
    // would look like a copy cycle that the real pass does not have, and would be measured as a
    // demand for a scratch register nothing needs.
    static TemporaryReserve widest();

    bool isEmpty() const {
        for(Size i = 0; i < kRegisterBankCount; i++) {
            if(operandTemps[i] || moveTemps[i]) return false;
        }

        return true;
    }

    // Scratch register `index` of a bank's operand pool and of its move pool. Positions counted from
    // the top of the register file with the operand pool first, so the two can never hand out the
    // same register - and so that narrowing the operand pool hands back the registers below it rather
    // than leaving a hole in the middle of the reserve.
    PhysicalReg operandTemp(RegisterBankId bank, Size index) const;
    PhysicalReg moveTemp(RegisterBankId bank, Size index) const;

    // Every register this reserve holds back.
    RegSet regs() const;

    // Raises every count to the larger of the two, and answers whether anything grew. Monotone on
    // purpose: it is what bounds the placement loop, since each growth is a strict increase in a
    // count the register file bounds from above.
    bool growTo(const TemporaryReserve& other);
};
