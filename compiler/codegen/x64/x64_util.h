#pragma once

#include "../../lower/lower_inst.h"
#include "gen.h"

enum class IntRegister: U8 {
    rax = 0, rcx, rdx, rbx, rsp, rbp, rsi, rdi,
    r8, r9, r10, r11, r12, r13, r14, r15,
};

enum class XmmRegister: U8 {
    xmm0 = 0x10, xmm1, xmm2, xmm3, xmm4, xmm5, xmm6, xmm7,
    xmm8, xmm9, xmm10, xmm11, xmm12, xmm13, xmm14, xmm15,
};

static constexpr Size kRegCount = 16;

// rsp is never handed out: push/pop/call/ret need it to be the stack pointer, in every function.
static constexpr U64 kReservedGenRegs = U64(1) << (Size)IntRegister::rsp;

// Every register of each class, and the ones a value can actually be given. Anything outside the
// allocatable set either does not exist or is rsp, so a calling convention's preserved set is
// stated relative to it: the registers a function has to give back are exactly the ones it could
// have taken in the first place.
inline RegSet allRegs() {
    RegSet set;
    set.classes[GenReg] = (U64(1) << kRegCount) - 1;
    set.classes[XmmReg] = (U64(1) << kRegCount) - 1;
    return set;
}

inline RegSet allocatableRegs() {
    auto set = allRegs();
    set.classes[GenReg] &= ~kReservedGenRegs;
    return set;
}

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
inline RegId framePointerReg() {
    return makeRegId(GenReg, U16(IntRegister::rbp));
}

inline RegSet framePointerRegs() {
    RegSet set;
    set.add(framePointerReg());
    return set;
}

/*
 * Scratch registers.
 *
 * A value living in the frame cannot be read by any encoder, so it is brought into a register at
 * each instruction that touches it. Those registers have to come from somewhere, and the simple,
 * defensible answer for a lean compiler is to hold a few back - but only in a function that turned
 * out to need them.
 *
 * A function is allocated once with nothing reserved. If nothing spilled, that is the answer and the
 * common case has paid nothing. If something did, the whole function is allocated again with these
 * held back. Two attempts, no heuristics, and the second cannot fail: whatever does not fit in a
 * register goes to the frame, and the frame has no limit.
 *
 * Three operand temporaries is what the widest instruction can want - two unconstrained operands and
 * a result that shares with neither - and a fixed-register operand needs none, since it is loaded
 * straight into the register the instruction demands. Two more serve the parallel copies, which need
 * somewhere to go through when a transfer has a frame slot at both ends or a cycle runs through one.
 *
 * They are taken from the top of the register file on purpose: r11-r15 are outside every described
 * convention's argument and result registers, so a scratch can never collide with a fixed register
 * the same instruction is also placing.
 */
static constexpr Size kMaxSpillTemps = 3;
static constexpr Size kMoveTemps = 2;
static constexpr Size kTotalSpillTemps = kMaxSpillTemps + kMoveTemps;

// Scratch register `index` of a class. The operand temporaries come first, then the two the move
// sequencer uses, so that the two pools cannot hand out the same register.
inline RegId spillTemp(RegClass cls, Size index) {
    assertTrue(index < kTotalSpillTemps);
    return makeRegId(cls, U16(kRegCount - 1 - index));
}

inline RegId moveTemp(RegClass cls, Size index) {
    assertTrue(index < kMoveTemps);
    return spillTemp(cls, kMaxSpillTemps + index);
}

inline RegSet spillTempRegs() {
    RegSet set;

    for(Size i = 0; i < kTotalSpillTemps; i++) {
        set.add(spillTemp(GenReg, i));
        set.add(spillTemp(XmmReg, i));
    }

    return set;
}

// Whether a location is a frame slot rather than a physical register. Slots and registers are the
// same kind of thing everywhere a location is handled, so this is the one question that separates
// them - a slot has an address instead of a number, and only the frame layout knows what it is.
inline bool isSlot(RegId id) {
    return getRegClass(id) == StackReg;
}

inline bool isImm(LowerValue* v) {
    return v->inst()->kind == LowerInst::Imm && (v->flags & LowerValue::Implicit);
}

inline bool isMem(LowerValue* v) {
    return v->inst()->kind == LowerInst::X86Address;
}

inline bool isReg(LowerValue* v) {
    return !isImm(v) && !isMem(v);
}

// True for values that need no location at all: embedded immediates, comparisons folded into flags,
// an elided direct callee, and the result of an argument store (which stands in for the argument in
// the call's operand list and is never read).
inline bool isImplicit(LowerValue* v) {
    return v->flags & LowerValue::Implicit;
}

inline bool needsRex(U8 reg) {
    return reg & 8;
}

inline bool needsRex(RegId reg) {
    return needsRex(U8(getRegIndex(reg)));
}

inline bool is64Bit(LowerType type) {
    return type == LowerType::Int64 || type == LowerType::Float64 || type == LowerType::Pointer;
}

// `v` is a LowerValue* pointing at a LowerImm's *embedded* `result` field, not at the start of
// the enclosing LowerImm object - `v->inst()` (not a raw `(LowerImm*)v` cast) is required to
// recover the real LowerImm* (it undoes the `result` field's offset via LowerValue::inset).
inline Maybe<U8> encodeImm8(LowerValue* v) {
    assertTrue(v->inst()->kind == LowerInst::Imm);

    auto imm = ((LowerImm*)v->inst())->i;
    if((imm & 0xffffffffffffff80) == 0xffffffffffffff80 || (imm & 0x7f) == imm) {
        return Just(U8(imm));
    } else {
        return Nothing();
    }
}

inline Maybe<U32> encodeImm32(LowerValue* v) {
    assertTrue(v->inst()->kind == LowerInst::Imm);

    auto imm = ((LowerImm*)v->inst())->i;
    if((imm & 0xffffffff80000000) == 0xffffffff80000000 || (imm & 0x7fffffff) == imm) {
        return Just(U32(imm));
    } else {
        return Nothing();
    }
}
