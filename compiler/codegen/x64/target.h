#pragma once

#include "../../lower/lower_inst.h"

struct CompileSettings;

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

/*
 * The baseline is **x86-64-v2**, and it is the floor rather than a default.
 *
 * POPCNT, RDTSCP and XSAVE were claimed here unconditionally long before the levels were named, and
 * those three are on every part that meets v2 and on nothing below it - so this backend already
 * required v2 and only said so by accident. SSE4.1 was the one that gave the game away: it was a
 * feature bit with a fallback behind it, and the fallback was for a machine the popcnt claim had
 * already excluded. Naming the floor removed both the bit and the fallback.
 *
 * So a feature below is never a question about the *baseline* - it is a question about which of the
 * two levels above it the target is. They are kept as separate bits rather than collapsed into a
 * level number because a form's `requiredFeatures` is documentation as much as a check: `vpaddd ymm`
 * needing AVX2 and `vfmadd` needing FMA3 are different facts about the instruction set, and a future
 * level that splits them differently should find them written down separately.
 */
static constexpr FeatureSet kFeatureBaseline = 0;

// VEX encoding: the 128- and 256-bit three-operand vector forms, and the ymm half of the vector bank.
static constexpr FeatureSet kFeatureAvx = 1 << 2;

/*
 * AVX2: the integer half of the 256-bit tier, which is what makes a `ymm` a register a value can
 * live in here rather than one a float operation happens to be able to name.
 *
 * A feature of its own beside `kFeatureAvx` rather than a wider reading of it, because AVX widened
 * the *float* operations alone: `vaddps ymm` is AVX and `vpaddd ymm` is AVX2, and a `Vec(I32)` that
 * had to be split in half for want of the integer form would not be one vector. `targetVectorBytes`
 * already draws the line in the same place - it answers 32 at AVX2 and 16 at AVX - so the feature
 * and the width the type system hands out agree by construction rather than by a rule kept in two
 * places.
 *
 * Every wide form in the table requires this, the float ones included. Nothing is lost by it: a
 * target with AVX and not AVX2 has a natural vector width of 16, so no value of 32 bytes is ever
 * built for one, and a form the width can never reach is a row nobody selects.
 */
static constexpr FeatureSet kFeatureAvx2 = 1 << 7;

// EVEX encoding: 512-bit operations, the upper sixteen vector registers, and the mask registers.
static constexpr FeatureSet kFeatureAvx512f = 1 << 3;

/*
 * BMI1, which here is `tzcnt` and nothing else yet - and which is v3, like everything beside it.
 *
 * It was claimed with AVX2 before the levels were named, on the grounds that no part has one without
 * the other; the level says the same thing and says it once. BMI2 is v3 as well and has a bit of its
 * own below, which `bzhi` is what finally wanted.
 *
 * What it buys is the one thing a sentinel bit cannot do at every width: `tzcnt` answers the
 * *operand's width* for a zero operand, where `bsf` leaves its destination undefined. A movemask
 * that fills its word - thirty-two bytes of a `ymm`, and a 64-lane `k` register when the mask bank
 * lands - has no bit above itself to mark, and this is what answers "nothing is set" there. See
 * `expandMaskFirstSet`.
 *
 * The hazard the linkage removes is the one that has no diagnostic: `tzcnt` is `f3 0f bc`, which a
 * processor without BMI1 decodes as `bsf` and runs, leaving the destination alone where the program
 * wanted the width. A feature claimed wrongly here is a wrong answer rather than an illegal
 * instruction, which is why it is claimed from a level that implies it rather than detected.
 */
static constexpr FeatureSet kFeatureBmi1 = 1 << 8;

/*
 * BMI2, which here is `bzhi` and nothing else yet - and which is v3, beside BMI1.
 *
 * A bit of its own rather than a wider reading of `kFeatureBmi1`, on the argument the file opens
 * with: the two are separate facts about the instruction set, and a level that ever splits them
 * should find them written down separately. Every part that has one has the other, so today the two
 * are set and cleared together.
 *
 * What it buys is the tail of a chunked loop. `bzhi dst, src, index` copies the low `index` bits of
 * its source and clears the rest, which is exactly "only these lanes are live" applied to a movemask
 * - so the lane range a masked tail is written over stops being a *vector* (a splat of the count, a
 * comparison against `iota`, and an `and` per consumer, with `iota` and its bias held in registers
 * for the whole function) and becomes one general-register instruction below the movemask every
 * consumer already goes through. See `matchLaneRangeMask` and `laneRangeIndex` in transform_reduce.cpp,
 * which `lowerVectorReductions` reads.
 *
 * The index is read from the low byte of its operand and an index at or above the operand width
 * clears nothing, which is what makes the clamp that pass emits a *narrowing* question rather than a
 * saturating one - see the note there.
 */
static constexpr FeatureSet kFeatureBmi2 = 1 << 9;

/*
 * MOVBE - the byte-reversing load and store, and v3 like everything beside it.
 *
 * A bit of its own rather than a wider reading of BMI1 or BMI2, on the argument the file opens with:
 * it is a separate fact about the instruction set - one Atom generation had `movbe` and none of the
 * rest of v3 - and a level that ever splits them should find them written down separately.
 *
 * What it buys is a register and an instruction at every access to a value stored the other way
 * round, which is every binary format a program reads or writes: `bswap` after a load is two
 * instructions and a register holding a value nobody wanted, and this is one instruction and no
 * register. See `selectByteSwapMemory`.
 *
 * The hazard is the ordinary one for a feature claimed wrongly, and here it is the *loud* kind
 * rather than the silent kind `tzcnt` has: `0f 38 f0` decodes as nothing at all on a processor
 * without MOVBE, so a wrong claim faults rather than answering differently. That is why the fold is
 * refused where the feature is absent instead of the form being selected and legalized afterwards.
 */
static constexpr FeatureSet kFeatureMovbe = 1 << 10;

/*
 * LZCNT - the leading-zero count, and v3 with everything beside it.
 *
 * A bit of its own rather than a wider reading of BMI1, on the argument the file opens with: on
 * Intel it arrived with BMI1 and on AMD it arrived four generations earlier, as the one bit of ABM
 * that outlived that extension, so they are separate facts about the instruction set even though
 * every part this backend describes has both or neither.
 *
 * What it buys is the zero case and one subtraction. `bsr` is baseline and answers the *index* of
 * the highest set bit, so a leading-zero count off it is `width - 1 - bsr` and is undefined at zero;
 * `lzcnt` is the count itself and answers the width at zero, which is what the language's
 * `leadingZeros` is defined to do. `expandBitScans` is the fallback, and it is four instructions
 * against this one.
 *
 * The hazard is `tzcnt`'s exactly, and it is the silent kind: `lzcnt` is `f3 0f bd`, which a
 * processor without the feature decodes as `bsr` and runs - answering an index where the program
 * wanted a count, with no fault to notice. So this is claimed from a level that implies it rather
 * than detected, beside `kFeatureBmi1` and for its reason.
 */
static constexpr FeatureSet kFeatureLzcnt = 1 << 11;

/*
 * The fused multiply-add, at every width and both lane kinds - v3, with the rest.
 *
 * It was a flag beside the SSE ladder when the ladder had a rung for AVX-without-AVX2, since Sandy
 * Bridge has that rung and no FMA. Under the levels there is no such rung: a part with AVX and
 * without FMA is v2, and every v3 part has both.
 *
 * What it buys is a rounding rather than an instruction count. Design-Vector §3.3 makes `fma` a
 * *permission* to fuse rather than a promise, so a target without this expands it into the multiply
 * and the add it always meant - which is `expandFusedMultiplyAdd`, and is two roundings.
 */
static constexpr FeatureSet kFeatureFma3 = 1 << 6;

/*
 * The features this backend is compiling for.
 *
 * A process-wide value rather than a parameter, for the same reason `targetRegisters()` is one: it
 * is read by form selection, which is asked the same question from a dozen places that have an
 * instruction in front of them and no settings - see selectForm in machine_select.cpp and the peepholes in transform_peephole.cpp
 * that ask what a form writes. `setTargetFeatures` is called once per function by transformFunction,
 * which is the narrowest point every path through this backend passes.
 *
 * The default is what this backend claimed when the set was a constant, so a driver that configures
 * nothing gets exactly the code it got before there was anything to configure.
 */
FeatureSet targetFeatures();
void setTargetFeatures(FeatureSet features);

/*
 * Whether this function must be encoded without a vector prefix, whatever the target's features say.
 *
 * **One function, one encoding.** Mixing VEX-encoded vector instructions with legacy SSE ones inside
 * a function is architecturally legal and is a performance trap: a legacy SSE write leaves the upper
 * half of the register it names alone, so the processor has to preserve state the VEX instructions
 * around it were free to discard, and every crossing costs. On the part this was measured on
 * (Golden Cove) the cost is **140x** on a function that alternates the two every few instructions,
 * which is exactly what a SHA-NI compression loop is: `sha256rnds2` is legacy-encoded and has no VEX
 * spelling in the architecture, and the `paddd` and `pshufd` around it do.
 *
 * So a function holding one of those - `MachineForm::legacyOnly` - is encoded in legacy form
 * throughout, and `selectAlternativeForm` is where that is spent. What it costs is the three-operand
 * shape and the unaligned memory operand a VEX form would have had; what it buys is the 25x the
 * hardware digest is worth in the first place.
 *
 * Process-wide and per function, for the reason `targetFeatures` is: form selection is asked from a
 * dozen places that have an instruction and no function. `transformFunction` sets it, which is the
 * narrowest point every path through this backend passes.
 */
bool legacyVectorEncodings();
void setLegacyVectorEncodings(bool legacy);

/*
 * And what a given set of settings comes to.
 *
 * Only the vector extensions are read, and only when they were *named*: see
 * `CompileSettings::explicitExtensions`. The three baseline bits below are this backend's own claim
 * about AMD64 rather than a reading of the settings, and are left alone - moving them onto detected
 * extensions would change the code generated for every existing program according to the machine
 * that built it, which is the thing the explicit-only rule exists to prevent.
 */
FeatureSet x64FeaturesFor(const CompileSettings& settings);

/*
 * How far a block operation with a compile-time size is straight-lined.
 *
 * `rep movsb` has a flat startup of about thirty cycles: a copy of one byte and a copy of a hundred
 * and twenty-eight cost the same. So every block operation short enough to be written out as
 * transfers is cheaper written out, and the only question is where "short enough" stops - which is
 * the one decision in this backend that trades size for speed rather than being a straight win, and
 * therefore the one that reads a setting.
 *
 * **The setting it reads is `-inline`**, and not `-opt`. `optimization` is a level handed to LLVM
 * and says nothing about this path at all; `InlineLevel` is the knob whose whole axis is the one
 * `-Os` and `-Ofast` name, and a build that asked for smaller code by that flag means it here too.
 * The alternative was a flag of its own, which would be a second spelling of one intent.
 *
 * **A value derived from the settings, and deliberately not a process-wide one.** `targetFeatures`
 * is process-wide because form selection is asked its question from a dozen places that have an
 * instruction in front of them and no settings; this has exactly one reader - `expandBlockOperations`
 * - and that pass is handed the `Context` like every other. A second global would be a second thing
 * standing between this compiler and compiling two modules at once, bought for nothing.
 */
struct BlockExpansion {
    /*
     * The ceiling on a `Copy`, in bytes, and the widest single transfer one may use.
     *
     * Two numbers rather than one, because they answer different questions: the ceiling is the
     * size/speed trade, and the step is a fact about the machine - a `ymm` where the target has one
     * and an `xmm` where it does not. A copy at the ceiling costs `copyLimit / copyStep` transfer
     * pairs, so the two together are what decides how many instructions a site can grow to.
     */
    U64 copyLimit = 32;
    U32 copyStep = 8;

    // And the same for a `SetPattern`, which shares them: the pattern is replicated into a value
    // once above the stores, so a fill's step is as wide as a copy's. It was eight bytes and its own
    // much smaller ceiling while the expansion was an encoding, because a pattern in a general
    // register had no way to become a vector without a register to build one in.
    U64 setLimit = 32;
    U32 setStep = 8;
};

// What a given set of settings comes to. A pure function of them: no target state is read, so the
// answer for a module does not depend on which module was compiled before it.
BlockExpansion x64BlockExpansionFor(const CompileSettings& settings);

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
// register description is built - and both are held below what the feature bit allows until every
// form can name the registers they would add. See the comment in target.cpp.
Size vectorRegisterCount();
Size maskRegisterCount();

// The same, for a feature set other than the one in force. Read by setTargetFeatures, which refuses
// a change that would need a register description that has already been built to be a different one.
Size vectorRegisterCountFor(FeatureSet features);
Size maskRegisterCountFor(FeatureSet features);

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

/*
 * Whether a mask value lives in the mask bank on this target, or in the vector bank as the vector of
 * all-ones lanes that a comparison without AVX-512 produces.
 *
 * This is a *codegen* choice and deliberately not a Repr one - see §2 of Implementation-Vector.md.
 * A mask's representation in memory is the vector at both feature levels, so `Maybe(Mask(Float))`
 * means one thing everywhere; what changes here is only where a live one is held.
 */
inline bool maskInMaskBank() {
    return maskRegisterCount() > 0;
}

// Whether this type occupies the wide tier's register rather than the narrow one's. Asked of the
// *byte width* rather than of the class, because a mask's class is the vector class it is held in
// and the two answers have to be the same one. Read by form selection and by the pseudo encoders,
// which is why it is here rather than beside either.
inline bool isWideVector(LowerType type) {
    return type.byteWidth() > 16;
}

// Which of the three packed classes a vector of this many bytes occupies. A vector narrower than a
// register - `i32x2` is eight bytes - occupies the smallest one that holds it, which is the same
// thing the machine does with it.
inline RegisterClassId vectorClassForBytes(U32 bytes) {
    if(bytes <= 16) return ClassXmm128;
    if(bytes <= 32) return ClassYmm256;
    return ClassZmm512;
}

// The bank a value of this type lives in.
inline RegisterBankId bankForType(LowerType type) {
    if(type.isMask()) return maskInMaskBank() ? BankMask : BankVector;
    return isIntLike(type) ? BankGpr : BankVector;
}

// The class a value of this type occupies. Every scalar the lowering produces is at most eight bytes
// wide, so a float takes the scalar view of a vector register rather than the whole of one; a vector
// takes the whole of one, at the width its lanes come to.
inline RegisterClassId classForType(LowerType type) {
    if(type.isMask()) {
        // A mask register holds one bit per lane, so which of the two mask classes it is depends on
        // how many lanes there are and not on how wide they are: 32 lanes fit `k` at 32 bits.
        if(maskInMaskBank()) return type.lanes() > 32 ? ClassMask64 : ClassMask32;
        return vectorClassForBytes(type.byteWidth());
    }

    if(type.isVector()) return vectorClassForBytes(type.byteWidth());
    if(isIntLike(type)) return type == LowerType::Int32 ? ClassGpr32 : ClassGpr64;
    return type == LowerType::Float32 ? ClassFloat32 : ClassFloat64;
}

// The slot class a value of this type needs when it does not get a register - its own width, which
// is its register class's. Asked by the allocator when it spills a value and by the memory-operand
// rules when they decide whether an instruction may read one in place, which have to agree: a slot
// is exactly as wide as the value in it, and an access of any other width would take a neighbouring
// value with it.
//
// A vector reads it out of the class rather than restating it, because that is where the answer is
// least able to drift: `RegisterClassDesc::spillClass` is what the allocator sizes the slot from.
inline StackSlotClass stackSlotClassFor(LowerType type) {
    if(isVectorLike(type)) return targetRegisters().regClass(classForType(type)).spillClass;

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
 * ~~Temporaries are taken from the top of the register file on purpose: r11-r15 are outside every
 * described convention's argument and result registers, so a scratch can never collide with a fixed
 * register the same instruction is also placing.~~
 *
 * **The pool is chosen per function** (§42, `chooseTemporaryPool` in register.cpp), because that
 * argument was wrong in both directions. It was wrong about safety - a reserve of five or six
 * reaches r11, which `kComplexArgs` and `kComplexResults` name as a fixed operand at a call - and it
 * was wrong about cost, since r15 downwards is *callee-saved*, so a leaf function that wants one
 * scratch register pays a `push` and a `pop` for it while half its register file sits unused.
 *
 * So the pool is a list of register indices rather than an offset from the top, chosen once per
 * function from what that function's own instructions leave alone. What it never contains is a
 * register any instruction *fixes* - which is the safety half of the old rule, stated about the
 * function rather than about the convention - and what it prefers is a register the convention lets
 * the function destroy, which is the cost half the old rule had backwards.
 */

/*
 * The highest operand-temporary position one instruction can reach in one bank.
 *
 * Not the number of temporaries the widest form wants, which is three - the lane-wise select reads
 * three unconstrained operands and ties its result to the first, so a spilled result and two reloads
 * want three at once, and a fixed-register operand wants none. A *position* is reached past as well
 * as taken: `takeTemp` steps over any position whose register the instruction is already using - the
 * two a folded address is holding, and the ones the form's own expansion clobbers - and consumes it,
 * because the pool is a contiguous block off the top of the register file and the reserve has to
 * hold back what was stepped over as well as what was taken.
 *
 * So this is the sum of the two, and `operandTempReach` in machine_validate.cpp is what adds them up - for
 * every form in the table, when the table is built. That check is the point: this number is one
 * number for a whole backend, and both times it has been wrong the form that outgrew it looked
 * perfectly ordinary next to the ones that did not.
 *
 * This is not the reserve - it is the limit the measurement is checked against, so that a form
 * wanting more is a loud failure rather than two temporaries quietly naming one register.
 */
static constexpr Size kMaxOperandTemps = 4;

// The two the move sequencer can want at once: one to break a cycle that has no exchange to use, and
// one to carry a transfer whose ends are both frame slots.
static constexpr Size kMaxMoveTemps = 2;

// How many positions a bank's pool has: the two roles end to end, since the move pool starts where
// the operand pool stops and `widest()` may ask for all of both at once.
static constexpr Size kMaxTemporaryPool = kMaxOperandTemps + kMaxMoveTemps;

struct TemporaryReserve {
    U8 operandTemps[kRegisterBankCount] = {};
    U8 moveTemps[kRegisterBankCount] = {};

    /*
     * Which registers the two pools are drawn from, in the order the positions are handed out.
     *
     * Empty until `chooseTemporaryPool` fills it, and a reserve with nothing chosen answers from the
     * top of the register file as this always did - which is what keeps a hand-built reserve
     * (`widest()` in the form-table check) meaningful without a function to choose against.
     */
    U8 pool[kRegisterBankCount][kMaxTemporaryPool] = {};
    bool chosen = false;

    // The widest pools any one instruction can ask for, over the same registers as `like`. Held
    // during the measuring pass only, so that measuring hands out a distinct register per temporary:
    // two temporaries naming one register would look like a copy cycle that the real pass does not
    // have, and would be measured as a demand for a scratch register nothing needs.
    //
    // Over `like`'s registers and not the file's top, because the measurement has to be of the pass
    // that will actually run: a demand measured against one set of registers and spent on another is
    // one that stepped over the wrong clobbers.
    static TemporaryReserve widestLike(const TemporaryReserve& like);
    static TemporaryReserve widest() { return widestLike(TemporaryReserve {}); }

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

    // Raises every count to the larger of the two, and answers whether anything grew. Each growth is
    // a strict increase in a count the register file bounds from above, which is what makes the
    // growing half of the placement loop finite whatever the function looks like.
    bool growTo(const TemporaryReserve& other);

    /*
     * And the other direction: every count lowered to what the last placement actually asked for.
     * Answers whether anything shrank.
     *
     * The growth above stops as soon as one pass's demand *fits*, which is not the same as its being
     * the demand. A first pass holds nothing back and spills whatever the pressure makes it spill;
     * the reserve measured against that placement is then held back from the *next* one, which
     * therefore has fewer registers and - the point - often spills less and asks for less. The count
     * that came out of the crowded pass is left standing over a placement that no longer needs it,
     * and every one of those registers is r15 downwards: callee-saved, so the function pays a `push`
     * and a `pop` for a scratch nothing asks for.
     *
     * Not monotone, so it cannot be what bounds the loop, and the loop bounds it instead: see
     * kMaxReserveShrinks in register.cpp. Nothing about *correctness* rests on the direction - the
     * loop only ever ends on a pass whose demand the reserve covers, whichever way it last moved.
     */
    bool shrinkTo(const TemporaryReserve& other);
};
